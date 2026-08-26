#include <avs/historical_retrieve_api.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "pdal/audit/audit.h"
#include "pdal/catalog/resource_catalog.h"
#include "pdal/live/live_data_source.h"
#include "pdal/query_engine.h"
#include "pdal/security/hooks.h"
#include "pdal/storage/avs_storage_backend.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::filesystem::path catalog = "config/resources.yaml";
  std::filesystem::path ssd = "/home/avs/DATA/SSD";
  std::filesystem::path hdd = "/home/avs/DATA/HDD";
  std::string resource;
  std::uint64_t start_ns = 0;
  std::uint64_t end_ns = 0;
  std::uint64_t max_records = 10;
  std::uint64_t max_bytes = 256ULL * 1024ULL * 1024ULL;
  std::size_t iterations = 10;
  std::size_t clients = 4;
};

struct Measurement {
  std::vector<double> latency_ms;
  std::uint64_t records = 0;
  std::uint64_t bytes = 0;
  std::uint64_t hot_bytes = 0;
  std::uint64_t cold_bytes = 0;
  std::uint64_t user_cpu_us = 0;
  std::uint64_t system_cpu_us = 0;
  std::uint64_t peak_memory_kb = 0;
};

struct CpuUsage {
  std::uint64_t user_us;
  std::uint64_t system_us;
  std::uint64_t peak_memory_kb;
};

CpuUsage Usage() {
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
  const auto index = static_cast<std::size_t>(
      percentile * static_cast<double>(values.size() - 1));
  return values[index];
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
          {"records", measurement.records},
          {"bytes_returned", measurement.bytes},
          {"hot_bytes", measurement.hot_bytes},
          {"cold_bytes", measurement.cold_bytes},
          {"throughput_bytes_per_second",
           seconds > 0 ? static_cast<double>(measurement.bytes) / seconds : 0}};
}

Measurement Merge(Measurement left, Measurement right) {
  left.latency_ms.insert(left.latency_ms.end(), right.latency_ms.begin(),
                         right.latency_ms.end());
  left.records += right.records;
  left.bytes += right.bytes;
  left.hot_bytes += right.hot_bytes;
  left.cold_bytes += right.cold_bytes;
  left.user_cpu_us += right.user_cpu_us;
  left.system_cpu_us += right.system_cpu_us;
  left.peak_memory_kb = std::max(left.peak_memory_kb, right.peak_memory_kb);
  return left;
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
    else if (argument == "--ssd-root") options.ssd = value();
    else if (argument == "--hdd-root") options.hdd = value();
    else if (argument == "--resource") options.resource = value();
    else if (argument == "--start") options.start_ns = std::stoull(value());
    else if (argument == "--end") options.end_ns = std::stoull(value());
    else if (argument == "--max-records") options.max_records = std::stoull(value());
    else if (argument == "--max-bytes") options.max_bytes = std::stoull(value());
    else if (argument == "--iterations") options.iterations = std::stoull(value());
    else if (argument == "--clients") options.clients = std::stoull(value());
    else throw std::runtime_error("unknown option: " + argument);
  }
  if (options.resource.empty() || options.start_ns == 0 ||
      options.end_ns < options.start_ns || options.iterations == 0 ||
      options.clients == 0) {
    throw std::runtime_error(
        "--resource, --start, --end, and a positive iteration count are required");
  }
  return options;
}

struct PdalRun {
  double latency_ms = 0;
  std::uint64_t records = 0;
  std::uint64_t bytes = 0;
};

PdalRun OnePdalRun(const Options& options,
                   const std::shared_ptr<pdal::QueryEngine>& engine,
                   bool metadata = false) {
  pdal::DataQuery query;
  query.request_id = pdal::GenerateRequestId();
  query.resource = options.resource;
  query.operation = pdal::Operation::kHistory;
  query.selector.time_range = pdal::TimeRange{options.start_ns, options.end_ns};
  query.representation.format = metadata ? "metadata" : "native";
  query.options.max_records = metadata ? 1 : options.max_records;
  query.options.max_bytes = options.max_bytes;
  query.context.purpose = "benchmark";
  PdalRun run;
  const auto started = Clock::now();
  auto response = engine->Execute(std::move(query));
  while (auto sample = response.stream.Next()) {
    ++run.records;
    run.bytes += sample->payload ? sample->payload->size() : 0;
  }
  run.latency_ms = std::chrono::duration<double, std::milli>(
                       Clock::now() - started).count();
  return run;
}

Measurement DirectAvs(const Options& options,
                      const pdal::CatalogResource& resource) {
  avs::HistoricalRetrieveAPI direct(options.ssd, options.hdd);
  Measurement result;
  const auto before = Usage();
  for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
    const auto started = Clock::now();
    std::string error;
    auto page = direct.QueryRefs(
        resource.historical_binding.source,
        resource.historical_binding.options.at("topic_folder"),
        options.start_ns, options.end_ns, 0, options.max_records, &error);
    if (!error.empty() && page.refs.empty()) throw std::runtime_error(error);
    for (const auto& ref : page.refs) {
      std::vector<std::uint8_t> payload;
      if (!direct.LoadPayload(ref, payload, &error)) throw std::runtime_error(error);
      ++result.records;
      result.bytes += payload.size();
      if (ref.tier == avs::HistoricalStorageTier::kHot) {
        result.hot_bytes += payload.size();
      } else {
        result.cold_bytes += payload.size();
      }
    }
    result.latency_ms.push_back(std::chrono::duration<double, std::milli>(
                                    Clock::now() - started).count());
  }
  const auto after = Usage();
  result.user_cpu_us = after.user_us - before.user_us;
  result.system_cpu_us = after.system_us - before.system_us;
  result.peak_memory_kb = after.peak_memory_kb;
  return result;
}

Measurement ThroughPdal(const Options& options,
                        const std::shared_ptr<pdal::QueryEngine>& engine) {
  Measurement result;
  const auto before = Usage();
  for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
    const auto run = OnePdalRun(options, engine);
    result.records += run.records;
    result.bytes += run.bytes;
    result.latency_ms.push_back(run.latency_ms);
  }
  const auto after = Usage();
  result.user_cpu_us = after.user_us - before.user_us;
  result.system_cpu_us = after.system_us - before.system_us;
  result.peak_memory_kb = after.peak_memory_kb;
  return result;
}

Measurement ConcurrentPdal(const Options& options,
                           const std::shared_ptr<pdal::QueryEngine>& engine) {
  Measurement result;
  const auto before = Usage();
  for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
    std::vector<std::future<PdalRun>> clients;
    clients.reserve(options.clients);
    for (std::size_t client = 0; client < options.clients; ++client) {
      clients.push_back(std::async(std::launch::async, [options, engine] {
        return OnePdalRun(options, engine);
      }));
    }
    for (auto& client : clients) {
      const auto run = client.get();
      result.records += run.records;
      result.bytes += run.bytes;
      result.latency_ms.push_back(run.latency_ms);
    }
  }
  const auto after = Usage();
  result.user_cpu_us = after.user_us - before.user_us;
  result.system_cpu_us = after.system_us - before.system_us;
  result.peak_memory_kb = after.peak_memory_kb;
  return result;
}

Measurement MetadataWhileBulk(
    const Options& options, const std::shared_ptr<pdal::QueryEngine>& engine) {
  pdal::DataQuery bulk;
  bulk.request_id = pdal::GenerateRequestId();
  bulk.resource = options.resource;
  bulk.operation = pdal::Operation::kHistory;
  bulk.selector.time_range = pdal::TimeRange{options.start_ns, options.end_ns};
  bulk.representation.format = "native";
  bulk.options.max_records = options.max_records;
  bulk.options.max_bytes = options.max_bytes;
  auto second_bulk = bulk;
  second_bulk.request_id = pdal::GenerateRequestId();
  auto held = engine->Execute(std::move(bulk));
  auto second_held = engine->Execute(std::move(second_bulk));

  Measurement result;
  const auto before = Usage();
  for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
    const auto run = OnePdalRun(options, engine, true);
    result.records += run.records;
    result.latency_ms.push_back(run.latency_ms);
  }
  held.stream.Cancel();
  second_held.stream.Cancel();
  const auto after = Usage();
  result.user_cpu_us = after.user_us - before.user_us;
  result.system_cpu_us = after.system_us - before.system_us;
  result.peak_memory_kb = after.peak_memory_kb;
  return result;
}

Measurement Control(const Options& options,
                    const std::shared_ptr<pdal::QueryEngine>& engine,
                    bool discovery) {
  Measurement result;
  const auto before = Usage();
  for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
    const auto started = Clock::now();
    if (discovery) {
      result.records += engine->Discover().size();
    } else {
      (void)engine->Describe(options.resource);
      ++result.records;
    }
    result.latency_ms.push_back(std::chrono::duration<double, std::milli>(
                                    Clock::now() - started).count());
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
    const auto& resource = catalog.Get(options.resource);
    if (resource.historical_binding.backend_id != "avs") {
      throw std::runtime_error("selected resource is not AVS historical data");
    }
    auto backends = std::make_shared<pdal::BackendRegistry>();
    backends->Register(std::make_shared<pdal::AvsStorageBackend>(
        options.ssd, options.hdd));
    auto engine = std::make_shared<pdal::QueryEngine>(
        catalog, backends, std::make_shared<pdal::LiveSourceRegistry>(),
        std::make_shared<pdal::PassThroughPolicy>(),
        std::make_shared<pdal::NoOpPrivacy>(),
        std::make_shared<pdal::NullAuditSink>(), 2);

    const auto discovery = Control(options, engine, true);
    const auto describe = Control(options, engine, false);
    auto warmup = options;
    warmup.iterations = 1;
    (void)DirectAvs(warmup, resource);
    (void)ThroughPdal(warmup, engine);
    auto first_half = options;
    first_half.iterations = (options.iterations + 1) / 2;
    auto second_half = options;
    second_half.iterations = options.iterations / 2;
    auto direct = DirectAvs(first_half, resource);
    auto pdal = ThroughPdal(first_half, engine);
    if (second_half.iterations > 0) {
      pdal = Merge(std::move(pdal), ThroughPdal(second_half, engine));
      direct = Merge(std::move(direct), DirectAvs(second_half, resource));
    }
    const auto concurrent = ConcurrentPdal(options, engine);
    const auto metadata_while_bulk = MetadataWhileBulk(options, engine);
    const auto direct_p50 = Percentile(direct.latency_ms, 0.50);
    const auto pdal_p50 = Percentile(pdal.latency_ms, 0.50);
    boost::json::array measurements;
    measurements.emplace_back(Report("resource_discovery", discovery));
    measurements.emplace_back(Report("resource_describe", describe));
    measurements.emplace_back(Report("direct_avs_history", direct));
    measurements.emplace_back(Report("pdal_avs_history", pdal));
    measurements.emplace_back(Report("concurrent_pdal_history", concurrent));
    measurements.emplace_back(
        Report("metadata_history_while_bulk", metadata_while_bulk));
    std::cout << boost::json::serialize(boost::json::object{
                     {"resource", resource.resource_id},
                     {"start_ns", options.start_ns},
                     {"end_ns", options.end_ns},
                     {"max_records", options.max_records},
                     {"concurrent_clients", options.clients},
                     {"measurements", std::move(measurements)},
                     {"framework_overhead_p50_ms", pdal_p50 - direct_p50},
                     {"framework_overhead_percent",
                      direct_p50 > 0 ? (pdal_p50 - direct_p50) * 100.0 /
                                           direct_p50
                                    : 0}})
              << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pdal_avs_benchmark: " << error.what() << '\n';
    return 1;
  }
}
