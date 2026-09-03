#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <yaml-cpp/yaml.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "pdal/adapter/request_translator.h"
#include "pdal/audit/audit.h"
#include "pdal/catalog/resource_catalog.h"
#include "pdal/model/error.h"
#include "pdal/model/json.h"
#include "pdal/pipeline.h"
#include "pdal/privacy/human_blur.h"
#include "pdal/query_engine.h"
#include "pdal/security/authenticator.h"
#include "pdal/security/engine_policy_hook.h"
#include "pdal/security/hooks.h"
#ifdef PDAL_WITH_AVS
#include "pdal/storage/avs_storage_backend.h"
#endif
#ifdef PDAL_WITH_ROS_LIVE
#include "pdal/live/ros_live_data_source.h"
#endif
#include "pdal/transport/http_server.h"

namespace {

struct AppConfig {
  std::filesystem::path resources;
  std::filesystem::path policy;
  std::filesystem::path ssd_root;
  std::filesystem::path hdd_root;
  std::filesystem::path audit_log;
  std::string continuation_secret;
  std::size_t max_concurrent_bulk_queries = 2;
  std::size_t max_live_subscriptions = 8;
  std::size_t live_queue_bytes = 8 * 1024 * 1024;
  bool auth_present = false;
  pdal::AuthConfig auth;
  bool privacy_enabled = true;
  pdal::HumanBlurConfig human_blur;
  pdal::HttpServerConfig http;
};

std::filesystem::path ResolvePath(const std::filesystem::path& base,
                                  const std::string& configured) {
  std::filesystem::path path(configured);
  return path.is_absolute() ? path : base / path;
}

std::string ReadSecretFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) return {};
  std::ostringstream out;
  out << input.rdbuf();
  std::string value = out.str();
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
    value.pop_back();
  }
  return value;
}

AppConfig LoadConfig(const std::filesystem::path& path) {
  const auto root = YAML::LoadFile(path.string());
  const auto base = path.parent_path();
  AppConfig config;
  config.resources = ResolvePath(base, root["resource_catalog"].as<std::string>());
  config.policy = ResolvePath(base, root["policy"].as<std::string>());
  config.ssd_root = root["storage"]["ssd_root"].as<std::string>("/home/avs/DATA/SSD");
  config.hdd_root = root["storage"]["hdd_root"].as<std::string>("/home/avs/DATA/HDD");
  config.audit_log = root["audit_log"].as<std::string>("/tmp/pdal-audit.jsonl");
  config.continuation_secret = root["continuation_secret"].as<std::string>();
  const auto runtime = root["runtime"];
  config.max_concurrent_bulk_queries =
      runtime["max_concurrent_bulk_queries"].as<std::size_t>(2);
  config.max_live_subscriptions =
      runtime["max_live_subscriptions"].as<std::size_t>(8);
  config.live_queue_bytes =
      runtime["live_queue_bytes"].as<std::size_t>(8 * 1024 * 1024);
  if (const auto auth = root["auth"]) {
    config.auth_present = true;
    config.auth.required = auth["required"].as<bool>(true);
    config.auth.issuer = auth["issuer"].as<std::string>("");
    config.auth.audience = auth["audience"].as<std::string>("");
    config.auth.algorithm = auth["algorithm"].as<std::string>("HS256");
    config.auth.clock_skew_s = auth["clock_skew_s"].as<std::uint64_t>(60);
    std::string secret;
    if (const auto env = auth["hmac_secret_env"]) {
      if (const char* value = std::getenv(env.as<std::string>().c_str())) {
        secret = value;
      }
    }
    if (secret.empty() && auth["hmac_secret_file"]) {
      secret = ReadSecretFile(
          ResolvePath(base, auth["hmac_secret_file"].as<std::string>()));
    }
    if (secret.empty() && auth["hmac_secret"]) {
      secret = auth["hmac_secret"].as<std::string>();
    }
    config.auth.hmac_secret = std::move(secret);
  }
  if (const auto privacy = root["privacy"]) {
    config.privacy_enabled = privacy["enabled"].as<bool>(true);
    if (privacy["model_path"]) {
      config.human_blur.model_path =
          ResolvePath(base, privacy["model_path"].as<std::string>()).string();
    }
    config.human_blur.score_threshold =
        privacy["score_threshold"].as<float>(0.25F);
    config.human_blur.nms_threshold = privacy["nms_threshold"].as<float>(0.45F);
    config.human_blur.jpeg_quality = privacy["jpeg_quality"].as<int>(90);
    config.human_blur.blur_sigma = privacy["blur_sigma"].as<double>(30.0);
    config.human_blur.blur_kernel_divisor =
        privacy["blur_kernel_divisor"].as<int>(3);
    config.human_blur.intra_op_threads =
        privacy["intra_op_threads"].as<int>(2);
  }
  const auto server = root["server"];
  config.http.address = server["address"].as<std::string>("127.0.0.1");
  config.http.port = server["port"].as<std::uint16_t>(8080);
  config.http.max_body_bytes = server["max_body_bytes"].as<std::size_t>(1024 * 1024);
  config.http.max_connections = server["max_connections"].as<std::size_t>(8);
  config.http.demo_html_path = ResolvePath(base, server["demo_html"].as<std::string>("../web/index.html")).string();
  return config;
}

struct Runtime {
  std::shared_ptr<pdal::PdalPipeline> pipeline;
  std::shared_ptr<pdal::QueryEngine> query_engine;
  std::shared_ptr<pdal::Authenticator> authenticator;
};

std::shared_ptr<pdal::PrivacyStage> BuildPrivacyStage(const AppConfig& config) {
#ifdef PDAL_WITH_PRIVACY
  if (!config.privacy_enabled) {
    std::cerr << "WARNING: camera privacy stage is disabled in config; camera "
                 "requests will fail closed.\n";
    return pdal::MakeFailClosedStage("disabled in configuration");
  }
  try {
    auto stage = pdal::MakeHumanBlur(config.human_blur);
    std::cerr << "camera privacy: human blur active (model "
              << config.human_blur.model_path << ")\n";
    return stage;
  } catch (const std::exception& error) {
    std::cerr << "WARNING: camera privacy stage failed to initialise ("
              << error.what() << "); camera requests will fail closed.\n";
    return pdal::MakeFailClosedStage(error.what());
  }
#else
  (void)config;
  std::cerr << "WARNING: this build has no camera privacy stage "
               "(PDAL_WITH_PRIVACY=OFF); camera requests will fail closed.\n";
  return pdal::MakeFailClosedStage("built without PDAL_WITH_PRIVACY");
#endif
}

std::shared_ptr<pdal::Authenticator> BuildAuthenticator(const AppConfig& config) {
  if (config.auth_present && config.auth.required) {
    return std::make_shared<pdal::BearerTokenAuthenticator>(config.auth);
  }
  std::cerr << "WARNING: pDAL authentication is DISABLED. Identity is taken from "
               "unverified X-PDAL-* headers. Set an enforced auth: block before "
               "production use.\n";
  return std::make_shared<pdal::DevelopmentAuthenticator>();
}

Runtime BuildRuntime(const AppConfig& config) {
  auto catalog = pdal::ResourceCatalog::LoadYaml(config.resources);
  auto backends = std::make_shared<pdal::BackendRegistry>();
#ifdef PDAL_WITH_AVS
  backends->Register(std::make_shared<pdal::AvsStorageBackend>(
      config.ssd_root, config.hdd_root));
#else
  throw std::runtime_error("this build does not include the AVS backend");
#endif
  auto live_sources = std::make_shared<pdal::LiveSourceRegistry>();
#ifdef PDAL_WITH_ROS_LIVE
  live_sources->Register(std::make_shared<pdal::RosLiveDataSource>(
      catalog, config.max_live_subscriptions, config.live_queue_bytes));
#endif
  auto audit = std::make_shared<pdal::JsonLinesAuditSink>(config.audit_log);
  // One policy decision, shared by the compatible bulk pipeline and the
  // operation-oriented QueryEngine.
  auto policy_engine = std::make_shared<pdal::YamlPolicyEngine>(
      pdal::YamlPolicyEngine::Load(config.policy));
  // One privacy stage, shared by both paths. Only camera (image) payloads are
  // routed through it; GPS/LiDAR stay byte-identical.
  std::shared_ptr<const pdal::PrivacyStage> privacy = BuildPrivacyStage(config);
  auto pipeline = std::make_shared<pdal::PdalPipeline>(
      catalog, policy_engine, backends, audit,
      pdal::ContinuationCodec(config.continuation_secret),
      std::make_shared<pdal::RequestRegistry>(), privacy);
  auto policy_hook =
      std::make_shared<pdal::EnginePolicyHook>(catalog, policy_engine);
  auto query_engine = std::make_shared<pdal::QueryEngine>(
      std::move(catalog), backends, live_sources, policy_hook,
      std::make_shared<pdal::NoOpPrivacy>(), audit,
      config.max_concurrent_bulk_queries, privacy);
  return {std::move(pipeline), std::move(query_engine),
          BuildAuthenticator(config)};
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
    const auto runtime = BuildRuntime(config);
    if (command == "serve") {
      std::cerr << "pDAL listening on " << config.http.address << ':'
                << config.http.port << '\n';
      pdal::HttpServer(config.http, runtime.pipeline, runtime.query_engine,
                       runtime.authenticator)
          .Run();
      return 0;
    }
    if (command == "query") return RunQuery(runtime.pipeline, arguments);
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
