#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <yaml-cpp/yaml.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "pdal/adapter/request_translator.h"
#include "pdal/audit/audit.h"
#include "pdal/catalog/resource_catalog.h"
#include "pdal/model/error.h"
#include "pdal/model/json.h"
#include "pdal/pipeline.h"
#ifdef PDAL_WITH_AVS
#include "pdal/storage/avs_storage_backend.h"
#endif
#include "pdal/transport/http_server.h"

namespace {

struct AppConfig {
  std::filesystem::path resources;
  std::filesystem::path policy;
  std::filesystem::path ssd_root;
  std::filesystem::path audit_log;
  std::string continuation_secret;
  pdal::HttpServerConfig http;
};

std::filesystem::path ResolvePath(const std::filesystem::path& base,
                                  const std::string& configured) {
  std::filesystem::path path(configured);
  return path.is_absolute() ? path : base / path;
}

AppConfig LoadConfig(const std::filesystem::path& path) {
  const auto root = YAML::LoadFile(path.string());
  const auto base = path.parent_path();
  AppConfig config;
  config.resources = ResolvePath(base, root["resource_catalog"].as<std::string>());
  config.policy = ResolvePath(base, root["policy"].as<std::string>());
  config.ssd_root = root["storage"]["ssd_root"].as<std::string>("/home/avs/DATA/SSD");
  config.audit_log = root["audit_log"].as<std::string>("/tmp/pdal-audit.jsonl");
  config.continuation_secret = root["continuation_secret"].as<std::string>();
  const auto server = root["server"];
  config.http.address = server["address"].as<std::string>("127.0.0.1");
  config.http.port = server["port"].as<std::uint16_t>(8080);
  config.http.max_body_bytes = server["max_body_bytes"].as<std::size_t>(1024 * 1024);
  config.http.max_connections = server["max_connections"].as<std::size_t>(8);
  config.http.demo_html_path = ResolvePath(base, server["demo_html"].as<std::string>("../web/index.html")).string();
  return config;
}

std::shared_ptr<pdal::PdalPipeline> BuildPipeline(const AppConfig& config) {
  auto backends = std::make_shared<pdal::BackendRegistry>();
#ifdef PDAL_WITH_AVS
  backends->Register(std::make_shared<pdal::AvsStorageBackend>(config.ssd_root));
#else
  throw std::runtime_error("this build does not include the AVS backend");
#endif
  return std::make_shared<pdal::PdalPipeline>(
      pdal::ResourceCatalog::LoadYaml(config.resources),
      std::make_shared<pdal::YamlPolicyEngine>(
          pdal::YamlPolicyEngine::Load(config.policy)),
      std::move(backends),
      std::make_shared<pdal::JsonLinesAuditSink>(config.audit_log),
      pdal::ContinuationCodec(config.continuation_secret),
      std::make_shared<pdal::RequestRegistry>());
}

void PutBigEndian(std::uint8_t* output, std::uint64_t value, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) {
    output[bytes - i - 1] = static_cast<std::uint8_t>(value & 0xffU);
    value >>= 8U;
  }
}

int RunQuery(const std::shared_ptr<pdal::PdalPipeline>& pipeline,
             const std::vector<std::string>& arguments) {
  auto request = pdal::LocalCliAdapter().ToCanonicalRequest(arguments);
  auto prepared = pipeline->Prepare(std::move(request));
  const auto metadata = boost::json::serialize(
      pdal::ResponseMetadataToJson(prepared.metadata));
  if (prepared.request.delivery.mode == pdal::DeliveryMode::kMetadata ||
      prepared.request.representation.format == "metadata" ||
      prepared.request.representation.format == "metadata-only") {
    pipeline->Stream(prepared, {});
    std::cout << metadata << '\n';
    return 0;
  }

  std::cerr << metadata << '\n';
  std::cout.write("PDALSTR1", 8);
  pipeline->Stream(
      prepared,
      {[&](const pdal::BackendRecord& record) {
         const auto format = prepared.metadata.representations.at(record.resource_id);
         const auto header_json = boost::json::serialize(boost::json::object{
             {"resource_id", record.resource_id},
             {"timestamp_ns", record.timestamp_ns},
             {"representation", format},
             {"payload_size", record.payload_size}});
         std::array<std::uint8_t, 12> header{};
         PutBigEndian(header.data(), header_json.size(), 4);
         PutBigEndian(header.data() + 4, record.payload_size, 8);
         std::cout.write(reinterpret_cast<const char*>(header.data()), header.size());
         std::cout.write(header_json.data(), static_cast<std::streamsize>(header_json.size()));
         return std::cout.good();
       },
       [&](const std::uint8_t* data, std::size_t size) {
         std::cout.write(reinterpret_cast<const char*>(data),
                         static_cast<std::streamsize>(size));
         return std::cout.good();
       },
       [&]() { return std::cout.good(); }});
  return 0;
}

void Usage() {
  std::cerr << "usage:\n"
            << "  pdal serve [--config config/pdal.yaml]\n"
            << "  pdal query --resource ID --start NS --end NS --purpose PURPOSE "
               "[--format FORMAT] [--max-records N] [--max-bytes N] "
               "[--metadata] [--config PATH]\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage();
    return 2;
  }
  try {
    const std::string command = argv[1];
    std::filesystem::path config_path = "config/pdal.yaml";
    std::vector<std::string> arguments;
    if (command == "query") arguments.push_back("query");
    for (int i = 2; i < argc; ++i) {
      if (std::string(argv[i]) == "--config") {
        if (++i >= argc) throw std::runtime_error("--config requires a path");
        config_path = argv[i];
      } else {
        arguments.emplace_back(argv[i]);
      }
    }
    const auto config = LoadConfig(config_path);
    const auto pipeline = BuildPipeline(config);
    if (command == "serve") {
      std::cerr << "pDAL listening on " << config.http.address << ':'
                << config.http.port << '\n';
      pdal::HttpServer(config.http, pipeline).Run();
      return 0;
    }
    if (command == "query") return RunQuery(pipeline, arguments);
    Usage();
    return 2;
  } catch (const pdal::PdalError& error) {
    std::cerr << boost::json::serialize(
                     pdal::ErrorEnvelope(error, pdal::GenerateRequestId()))
              << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "pdal: " << error.what() << '\n';
    return 1;
  }
}
