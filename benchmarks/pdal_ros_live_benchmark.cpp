#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pdal/audit/audit.h"
#include "pdal/catalog/resource_catalog.h"
#include "pdal/live/ros_live_data_source.h"
#include "pdal/query_engine.h"
#include "pdal/sdk/client.h"
#include "pdal/security/hooks.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::filesystem::path catalog = "config/resources.yaml";
  std::string resource;
  std::size_t iterations = 20;
  std::size_t subscriptions = 4;
  std::chrono::milliseconds timeout{10000};
};

struct Measurement {
  std::vector<double> latency_ms;
  std::uint64_t samples = 0;
  std::uint64_t bytes = 0;
  std::uint64_t user_cpu_us = 0;
  std::uint64_t system_cpu_us = 0;
  std::uint64_t peak_memory_kb = 0;
};

struct UsageValue {
  std::uint64_t user_us;
  std::uint64_t system_us;
  std::uint64_t peak_memory_kb;
};

UsageValue Usage() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  const auto micros = [](const timeval& value) {
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000ULL +
           static_cast<std::uint64_t>(value.tv_usec);
  };
  return {micros(usage.ru_utime), micros(usage.ru_stime),
          static_cast<std::uint64_t>(usage.ru_maxrss)};
}

double Percentile(std::vector<double> values, double percentile) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  return values[static_cast<std::size_t>(
      percentile * static_cast<double>(values.size() - 1))];
}

boost::json::object Report(const std::string& name,
                           const Measurement& measurement) {
  const auto seconds = std::accumulate(measurement.latency_ms.begin(),
                                       measurement.latency_ms.end(), 0.0) /
                       1000.0;
  return {{"name", name},
          {"iterations", measurement.latency_ms.size()},
          {"p50_latency_ms", Percentile(measurement.latency_ms, 0.50)},
          {"p95_latency_ms", Percentile(measurement.latency_ms, 0.95)},
          {"p99_latency_ms", Percentile(measurement.latency_ms, 0.99)},
          {"user_cpu_us", measurement.user_cpu_us},
          {"system_cpu_us", measurement.system_cpu_us},
          {"peak_memory_kb", measurement.peak_memory_kb},
          {"samples", measurement.samples},
          {"bytes_returned", measurement.bytes},
          {"throughput_bytes_per_second",
           seconds > 0 ? static_cast<double>(measurement.bytes) / seconds : 0}};
}

Options Parse(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto value = [&]() -> std::string {
      if (++index >= argc) throw std::runtime_error(argument + " requires a value");
      return argv[index];
    };
    if (argument == "--catalog") options.catalog = value();
    else if (argument == "--resource") options.resource = value();
    else if (argument == "--iterations") options.iterations = std::stoull(value());
    else if (argument == "--subscriptions") options.subscriptions = std::stoull(value());
    else if (argument == "--timeout-ms") {
      options.timeout = std::chrono::milliseconds(std::stoull(value()));
    } else {
      throw std::runtime_error("unknown option: " + argument);
    }
  }
  if (options.resource.empty() || options.iterations == 0 ||
      options.subscriptions == 0 || options.timeout.count() <= 0) {
    throw std::runtime_error(
        "--resource and positive iterations/subscriptions/timeout are required");
  }
  return options;
}

std::optional<pdal::DataSample> NextWithTimeout(
    pdal::DataStream& stream, std::chrono::milliseconds timeout) {
  auto pending = std::async(std::launch::async, [&stream] { return stream.Next(); });
  if (pending.wait_for(timeout) != std::future_status::ready) {
    stream.Cancel();
    pending.wait();
    throw std::runtime_error("timed out waiting for a live sample");
  }
  return pending.get();
}

template <typename Callable>
Measurement Measure(std::size_t iterations, Callable&& callable) {
  Measurement result;
  const auto before = Usage();
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    const auto started = Clock::now();
    const auto sample = callable();
    result.latency_ms.push_back(std::chrono::duration<double, std::milli>(
                                    Clock::now() - started).count());
    if (sample) {
      ++result.samples;
      result.bytes += sample->payload ? sample->payload->size() : 0;
    }
  }
  const auto after = Usage();
  result.user_cpu_us = after.user_us - before.user_us;
  result.system_cpu_us = after.system_us - before.system_us;
  result.peak_memory_kb = after.peak_memory_kb;
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = Parse(argc, argv);
    auto catalog = pdal::ResourceCatalog::LoadYaml(options.catalog);
    const auto& internal = catalog.Get(options.resource);
    if (!internal.live_binding || internal.live_binding->backend_id != "ros") {
      throw std::runtime_error("selected resource has no ROS live binding");
    }
    auto source = std::make_shared<pdal::RosLiveDataSource>(
        catalog, options.subscriptions + 2);
    auto live_sources = std::make_shared<pdal::LiveSourceRegistry>();
    live_sources->Register(source);
    auto engine = std::make_shared<pdal::QueryEngine>(
        catalog, std::make_shared<pdal::BackendRegistry>(), live_sources,
        std::make_shared<pdal::PassThroughPolicy>(),
        std::make_shared<pdal::NoOpPrivacy>(),
        std::make_shared<pdal::NullAuditSink>(), 1);
    pdal::PdalClient client(engine, {{}, "benchmark", {}});
    auto handle = client.open(options.resource);
    pdal::RepresentationRequest native;
    native.format = "native";

    const auto deadline = Clock::now() + options.timeout;
    while (!source->Latest(internal, native)) {
      if (Clock::now() >= deadline) {
        throw std::runtime_error(
            "no ROS samples arrived before timeout; start a publisher first");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto direct_latest = Measure(options.iterations, [&] {
      return source->Latest(internal, native);
    });
    const auto pdal_latest = Measure(options.iterations, [&] {
      return std::optional<pdal::DataSample>(handle.latest(native));
    });

    auto direct_stream = source->Subscribe(internal, native, {});
    const auto direct_subscribe = Measure(options.iterations, [&] {
      return NextWithTimeout(direct_stream, options.timeout);
    });
    direct_stream.Cancel();
    auto pdal_stream = handle.subscribe({}, native);
    const auto pdal_subscribe = Measure(options.iterations, [&] {
      return NextWithTimeout(pdal_stream, options.timeout);
    });
    pdal_stream.Cancel();

    const auto multiple = Measure(options.iterations, [&]() {
      std::vector<pdal::DataStream> streams;
      streams.reserve(options.subscriptions);
      for (std::size_t index = 0; index < options.subscriptions; ++index) {
        streams.push_back(handle.subscribe({}, native));
      }
      for (auto& stream : streams) stream.Cancel();
      return std::optional<pdal::DataSample>{};
    });

    boost::json::array measurements;
    measurements.emplace_back(Report("direct_live_latest", direct_latest));
    measurements.emplace_back(Report("pdal_latest", pdal_latest));
    measurements.emplace_back(Report("direct_live_subscribe", direct_subscribe));
    measurements.emplace_back(Report("pdal_subscribe", pdal_subscribe));
    measurements.emplace_back(Report("multiple_pdal_subscriptions_setup", multiple));
    const auto direct_latest_p50 = Percentile(direct_latest.latency_ms, 0.50);
    const auto pdal_latest_p50 = Percentile(pdal_latest.latency_ms, 0.50);
    std::cout << boost::json::serialize(boost::json::object{
                     {"resource", internal.resource_id},
                     {"subscriptions", options.subscriptions},
                     {"measurements", std::move(measurements)},
                     {"latest_framework_overhead_p50_ms",
                      pdal_latest_p50 - direct_latest_p50}})
              << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pdal_ros_live_benchmark: " << error.what() << '\n';
    return 1;
  }
}
