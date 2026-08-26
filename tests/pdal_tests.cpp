#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pdal/adapter/request_translator.h"
#include "pdal/audit/audit.h"
#include "pdal/catalog/resource_catalog.h"
#include "pdal/model/error.h"
#include "pdal/model/json.h"
#include "pdal/pipeline.h"
#include "pdal/planner/query_planner.h"
#include "pdal/policy/policy_engine.h"
#include "pdal/representation/representation_provider.h"
#include "pdal/sdk/client.h"
#include "pdal/security/hooks.h"
#include "pdal/storage/storage_backend.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: "            \
                << #condition << '\n';                                          \
      ++failures;                                                               \
    }                                                                           \
  } while (false)

template <typename Callable>
void CheckError(pdal::ErrorClass expected, Callable&& callable) {
  try {
    callable();
    std::cerr << "expected PdalError was not thrown\n";
    ++failures;
  } catch (const pdal::PdalError& error) {
    CHECK(error.error_class() == expected);
  }
}

pdal::CatalogResource Camera(std::string backend = "fake") {
  pdal::CatalogResource resource;
  resource.resource_id = "camera.front";
  resource.aliases = {"vehicle.camera.front"};
  resource.resource_type = "sensor-data";
  resource.semantic_name = "Front camera";
  resource.modality = "image";
  resource.schema = "pdal.image.v1";
  resource.representations = {{"jpeg", "image/jpeg", true}};
  resource.attributes = {{"orientation", "front"}};
  resource.historical_binding = {
      std::move(backend), "/secret/avs/camera/topic",
      {{"topic_folder", "secret_camera_folder"}}};
  resource.live_binding = pdal::BackendBinding{
      "fake-live", "/secret/ros/camera/topic",
      {{"message_type", "sensor_msgs/msg/Image"}}};
  resource.operations = {pdal::Operation::kDescribe, pdal::Operation::kHistory,
                         pdal::Operation::kLatest, pdal::Operation::kSubscribe};
  resource.limits = {1000, 100, 64ULL * 1024ULL * 1024ULL, 128};
  return resource;
}

pdal::CatalogResource Position(std::string backend = "fake") {
  pdal::CatalogResource resource;
  resource.resource_id = "position";
  resource.aliases = {"vehicle.position"};
  resource.resource_type = "sensor-data";
  resource.semantic_name = "Position";
  resource.modality = "gps";
  resource.schema = "pdal.position.v1";
  resource.representations = {
      {"position-binary-v1", "application/vnd.pdal.position-v1", true}};
  resource.historical_binding = {
      std::move(backend), "/secret/avs/gps/topic",
      {{"topic_folder", "secret_gps_folder"}}};
  resource.operations = {pdal::Operation::kDescribe, pdal::Operation::kHistory};
  resource.limits = {1000, 100, 64ULL * 1024ULL * 1024ULL, 128};
  return resource;
}

pdal::ResourceCatalog TestCatalog() {
  return pdal::ResourceCatalog::FromResources({Camera(), Position()});
}

pdal::YamlPolicyEngine TestPolicy(std::uint64_t max_records = 10,
                                  std::uint64_t max_bytes = 1000) {
  pdal::RolePolicy service;
  service.resources = {"*"};
  service.purposes = {"diagnostics"};
  service.transformations = {};
  service.earliest_time_ns = 150;
  service.latest_time_ns = 350;
  service.max_records = max_records;
  service.max_bytes = max_bytes;
  pdal::RolePolicy viewer;
  viewer.resources = {"vehicle.position"};
  viewer.purposes = {"diagnostics"};
  viewer.max_records = 1;
  viewer.max_bytes = 100;
  return pdal::YamlPolicyEngine::FromPolicies(
      "test-policy", 60000000000ULL,
      {{"service", std::move(service)}, {"viewer", std::move(viewer)}});
}

pdal::ExternalHeaders TestHeaders() {
  return {{"x-pdal-principal", "local-service-tool"},
          {"x-pdal-organization", "local"},
          {"x-pdal-role", "service"}};
}

boost::json::object NativeBody(std::uint64_t max_records = 100) {
  return {{"resources", boost::json::array{"vehicle.camera.front"}},
          {"time", boost::json::object{{"start_ns", 100}, {"end_ns", 400}}},
          {"purpose", "diagnostics"},
          {"representation", boost::json::object{{"format", "jpeg"}}},
          {"delivery", boost::json::object{{"mode", "stream"},
                                            {"max_records", max_records},
                                            {"max_bytes", 67108864}}}};
}

class MemoryLocator final : public pdal::RecordLocator {
 public:
  explicit MemoryLocator(std::vector<std::uint8_t> bytes)
      : payload(std::move(bytes)) {}
  std::vector<std::uint8_t> payload;
};

class FakeBackend final : public pdal::StorageBackend {
 public:
  explicit FakeBackend(std::vector<std::uint64_t> timestamps)
      : timestamps_(std::move(timestamps)) {}

  pdal::BackendCapabilities GetCapabilities() const override {
    return {"fake", true, true, true, 0};
  }

  void Resolve(const pdal::CatalogResource& resource) const override {
    CHECK(resource.historical_binding.backend_id == "fake");
  }

  std::vector<pdal::BackendRecord> Query(
      const pdal::ExecutionTask& task,
      const pdal::ResourceCatalog&) const override {
    last_query = task.time;
    std::vector<pdal::BackendRecord> records;
    for (const auto timestamp : timestamps_) {
      if (timestamp < task.time.start_ns || timestamp > task.time.end_ns) continue;
      std::vector<std::uint8_t> payload{
          static_cast<std::uint8_t>(timestamp), 2, 3, 4};
      records.push_back({task.resource_id, "fake", timestamp, payload.size(),
                         "memory",
                         std::make_shared<MemoryLocator>(std::move(payload))});
    }
    return records;
  }

  std::unique_ptr<pdal::HistoryCursor> OpenHistory(
      const pdal::HistoryCursorRequest& request,
      const pdal::ResourceCatalog& catalog) const override {
    last_cursor_limit = request.max_records;
    last_cursor_offset = request.offset;
    auto records = Query(request.task, catalog);
    class Cursor final : public pdal::HistoryCursor {
     public:
      Cursor(std::vector<pdal::BackendRecord> all, std::uint64_t offset,
             std::uint64_t limit)
          : has_more_(offset < all.size() && all.size() - offset > limit),
            considered_(std::min<std::uint64_t>(all.size(), offset + limit + 1)) {
        const auto begin = std::min<std::uint64_t>(offset, all.size());
        const auto end = std::min<std::uint64_t>(all.size(), begin + limit);
        for (auto index = begin; index < end; ++index) {
          records_.push_back(std::move(all[index]));
        }
      }
      std::optional<pdal::BackendRecord> Next() override {
        if (position_ == records_.size()) return std::nullopt;
        return std::move(records_[position_++]);
      }
      bool has_more() const override { return has_more_; }
      std::uint64_t records_considered() const override { return considered_; }
     private:
      std::vector<pdal::BackendRecord> records_;
      std::size_t position_ = 0;
      bool has_more_ = false;
      std::uint64_t considered_ = 0;
    };
    return std::make_unique<Cursor>(std::move(records), request.offset,
                                    request.max_records);
  }

  std::uint64_t Read(const pdal::BackendRecord& record,
                     const pdal::ByteSink& sink) const override {
    ++read_count;
    const auto locator = std::dynamic_pointer_cast<const MemoryLocator>(record.locator);
    if (!locator) throw std::runtime_error("bad memory locator");
    const auto midpoint = locator->payload.size() / 2;
    if (midpoint && !sink(locator->payload.data(), midpoint)) {
      throw pdal::PdalError(pdal::ErrorClass::kBackendUnavailable,
                            "consumer disconnected");
    }
    if (!sink(locator->payload.data() + midpoint,
              locator->payload.size() - midpoint)) {
      throw pdal::PdalError(pdal::ErrorClass::kBackendUnavailable,
                            "consumer disconnected");
    }
    return locator->payload.size();
  }

  mutable pdal::TimeRange last_query;
  mutable std::uint64_t last_cursor_limit = 0;
  mutable std::uint64_t last_cursor_offset = 0;
  mutable std::uint64_t read_count = 0;

 private:
  std::vector<std::uint64_t> timestamps_;
};

class FakeLiveSource final : public pdal::ILiveDataSource {
 public:
  pdal::LiveSourceCapabilities GetCapabilities() const override {
    return {"fake-live", true, true, 4};
  }
  std::optional<pdal::DataSample> Latest(
      const pdal::ResourceDescriptor& resource,
      const pdal::RepresentationRequest&) const override {
    ++latest_calls;
    return Sample(resource.resource_id, 501);
  }
  pdal::DataStream Subscribe(
      const pdal::ResourceDescriptor& resource,
      const pdal::RepresentationRequest&,
      const pdal::QueryOptions&) const override {
    ++subscribe_calls;
    auto sample = std::make_shared<std::optional<pdal::DataSample>>(
        Sample(resource.resource_id, 502));
    return pdal::DataStream([sample]() mutable {
      if (!*sample) return std::optional<pdal::DataSample>{};
      auto next = std::move(**sample);
      sample->reset();
      return std::optional<pdal::DataSample>(std::move(next));
    });
  }
  static pdal::DataSample Sample(const std::string& resource,
                                 std::uint64_t timestamp) {
    return {resource, timestamp, "jpeg", "image/jpeg",
            std::make_shared<const std::vector<std::uint8_t>>(
                std::vector<std::uint8_t>{9, 8, 7}), {}};
  }
  mutable std::uint64_t latest_calls = 0;
  mutable std::uint64_t subscribe_calls = 0;
};

class CapturingPolicy final : public pdal::PolicyHook {
 public:
  pdal::DataQuery Apply(
      const pdal::DataQuery& query,
      const pdal::ResourceDescriptor&) const override {
    last = query;
    return query;
  }
  mutable std::optional<pdal::DataQuery> last;
};

std::shared_ptr<pdal::QueryEngine> TestQueryEngine(
    const std::shared_ptr<FakeBackend>& backend,
    const std::shared_ptr<FakeLiveSource>& live,
    const std::shared_ptr<CapturingPolicy>& policy) {
  auto backends = std::make_shared<pdal::BackendRegistry>();
  backends->Register(backend);
  auto live_sources = std::make_shared<pdal::LiveSourceRegistry>();
  live_sources->Register(live);
  return std::make_shared<pdal::QueryEngine>(
      TestCatalog(), backends, live_sources, policy,
      std::make_shared<pdal::NoOpPrivacy>(),
      std::make_shared<pdal::NullAuditSink>(), 1);
}

std::shared_ptr<pdal::PdalPipeline> TestPipeline(
    const std::shared_ptr<FakeBackend>& backend,
    std::uint64_t max_records = 10) {
  auto registry = std::make_shared<pdal::BackendRegistry>();
  registry->Register(backend);
  return std::make_shared<pdal::PdalPipeline>(
      TestCatalog(),
      std::make_shared<pdal::YamlPolicyEngine>(TestPolicy(max_records)),
      registry, std::make_shared<pdal::NullAuditSink>(),
      pdal::ContinuationCodec("test-continuation-secret"),
      std::make_shared<pdal::RequestRegistry>());
}

void TestResourceMappingAndNoLeak() {
  const auto catalog = pdal::ResourceCatalog::LoadYaml("config/resources.yaml");
  const auto& camera = catalog.Get("vehicle.camera.front");
  CHECK(camera.resource_id == "camera.front");
  CHECK(camera.modality == "image");
  CHECK(camera.historical_binding.source ==
        "/my_camera/pylon_ros2_camera_node/image_raw");
  const auto public_json = boost::json::serialize(pdal::ResourceToJson(camera));
  CHECK(public_json.find("/my_camera") == std::string::npos);
  const auto public_object = pdal::ResourceToJson(camera);
  CHECK(!public_object.contains("source"));
  CHECK(!public_object.contains("binding"));
  CHECK(public_json.find("secret_camera_folder") == std::string::npos);
  CHECK(public_json.find("\"avs\"") == std::string::npos);
  CHECK(public_json.find("SSD") == std::string::npos);
  CHECK(public_json.find("HDD") == std::string::npos);
  CHECK(catalog.List().size() == 3);
  CheckError(pdal::ErrorClass::kResourceNotFound,
             [&] { (void)catalog.Get("/my_camera/pylon_ros2_camera_node/image_raw"); });
}

void TestCanonicalValidation() {
  auto invalid = NativeBody();
  invalid["time"] = boost::json::object{{"start_ns", 400}, {"end_ns", 100}};
  CheckError(pdal::ErrorClass::kInvalidRequest, [&] {
    (void)pdal::NativeHttpAdapter().ToCanonicalRequest(invalid, TestHeaders());
  });
  auto too_many = NativeBody(100001);
  CheckError(pdal::ErrorClass::kInvalidRequest, [&] {
    (void)pdal::NativeHttpAdapter().ToCanonicalRequest(too_many, TestHeaders());
  });
}

void TestTranslatorEquivalence() {
  const auto native = pdal::NativeHttpAdapter().ToCanonicalRequest(
      NativeBody(), TestHeaders());
  const boost::json::object sovd{
      {"resource", "vehicle.camera.front"},
      {"timeRange", boost::json::object{{"from", 100}, {"to", 400}}},
      {"reason", "diagnostics"},
      {"contentFormat", "jpeg"},
      {"deliveryMode", "stream"}};
  const auto from_sovd = pdal::SovdAdapter().ToCanonicalRequest(sovd, TestHeaders());
  const auto from_cli = pdal::LocalCliAdapter().ToCanonicalRequest(
      {"query", "--resource", "vehicle.camera.front", "--start", "100",
       "--end", "400", "--purpose", "diagnostics", "--format", "jpeg"});
  CHECK(pdal::SemanticallyEquivalent(native, from_sovd));
  CHECK(pdal::SemanticallyEquivalent(native, from_cli));

  const auto catalog = TestCatalog();
  const auto policy = TestPolicy();
  const pdal::QueryPlanner planner;
  const auto native_plan = planner.Plan(policy.Authorize(native, catalog), catalog);
  const auto sovd_plan = planner.Plan(policy.Authorize(from_sovd, catalog), catalog);
  const auto cli_plan = planner.Plan(policy.Authorize(from_cli, catalog), catalog);
  CHECK(pdal::ExecutionPlanToJson(native_plan)["tasks"] ==
        pdal::ExecutionPlanToJson(sovd_plan)["tasks"]);
  CHECK(pdal::ExecutionPlanToJson(native_plan)["tasks"] ==
        pdal::ExecutionPlanToJson(cli_plan)["tasks"]);
}

void TestPolicyNarrowingAndDenial() {
  const auto request = pdal::NativeHttpAdapter().ToCanonicalRequest(
      NativeBody(), TestHeaders());
  const auto catalog = TestCatalog();
  const auto plan = TestPolicy(2, 128).Authorize(request, catalog);
  CHECK(plan.resources().size() == 1);
  CHECK(plan.resources()[0].time.start_ns == 150);
  CHECK(plan.resources()[0].time.end_ns == 350);
  CHECK(plan.limits().max_records == 2);
  CHECK(plan.limits().max_bytes == 128);

  auto denied_body = NativeBody();
  denied_body["purpose"] = "advertising";
  const auto denied = pdal::NativeHttpAdapter().ToCanonicalRequest(
      denied_body, TestHeaders());
  CheckError(pdal::ErrorClass::kForbidden,
             [&] { (void)TestPolicy().Authorize(denied, catalog); });

  auto viewer_headers = TestHeaders();
  viewer_headers["x-pdal-role"] = "viewer";
  const auto viewer = pdal::NativeHttpAdapter().ToCanonicalRequest(
      NativeBody(), viewer_headers);
  CheckError(pdal::ErrorClass::kForbidden,
             [&] { (void)TestPolicy().Authorize(viewer, catalog); });
}

void TestPlanningAndRepresentation() {
  const auto request = pdal::NativeHttpAdapter().ToCanonicalRequest(
      NativeBody(), TestHeaders());
  const auto catalog = TestCatalog();
  const auto access = TestPolicy().Authorize(request, catalog);
  const auto execution = pdal::QueryPlanner().Plan(access, catalog);
  CHECK(execution.tasks.size() == 1);
  CHECK(execution.tasks[0].resource_id == "vehicle.camera.front");
  CHECK(execution.tasks[0].backend_id == "fake");
  CHECK(execution.tasks[0].time == (pdal::TimeRange{150, 350}));
  CHECK(execution.operations.front() == pdal::PlanOperation::kResolveResource);
  CHECK(execution.operations.back() == pdal::PlanOperation::kStreamResult);

  const pdal::RepresentationProvider provider;
  const auto jpeg = provider.Negotiate(Camera(), request.representation);
  CHECK(jpeg.content_type == "image/jpeg");
  auto unsupported = request.representation;
  unsupported.format = "png";
  CheckError(pdal::ErrorClass::kRepresentationNotSupported,
             [&] { (void)provider.Negotiate(Camera(), unsupported); });
}

void TestStableErrorResponse() {
  const pdal::PdalError error(pdal::ErrorClass::kResourceNotFound, "missing");
  const auto envelope = pdal::ErrorEnvelope(error, "request-1");
  const auto& body = envelope.at("error").as_object();
  CHECK(body.at("code") == "PDAL_RESOURCE_NOT_FOUND");
  CHECK(body.at("class") == "resource_not_found");
  CHECK(body.at("request_id") == "request-1");
  CHECK(pdal::HttpStatus(error.error_class()) == 404);
  pdal::DataQuery invalid;
  invalid.request_id = "request-2";
  invalid.resource = "camera.front";
  CheckError(pdal::ErrorClass::kInvalidQuery,
             [&] { pdal::ValidateDataQuery(invalid); });
  CHECK(pdal::ErrorCode(pdal::ErrorClass::kInvalidQuery) ==
        "PDAL_INVALID_QUERY");
}

void TestPipelineAuthorizationBoundaryAndStreaming() {
  auto backend = std::make_shared<FakeBackend>(
      std::vector<std::uint64_t>{100, 150, 250, 350, 400});
  auto pipeline = TestPipeline(backend);
  const auto capabilities = boost::json::serialize(pipeline->CapabilitiesJson());
  CHECK(capabilities.find("fake") == std::string::npos);
  CHECK(capabilities.find("hot_tier") == std::string::npos);
  CHECK(capabilities.find("cold_tier") == std::string::npos);
  auto request = pdal::NativeHttpAdapter().ToCanonicalRequest(
      NativeBody(), TestHeaders());
  auto prepared = pipeline->Prepare(std::move(request));
  CHECK(backend->last_query == (pdal::TimeRange{150, 350}));
  CHECK(prepared.result.records.size() == 3);
  for (const auto& record : prepared.result.records) {
    CHECK(record.timestamp_ns >= 150 && record.timestamp_ns <= 350);
  }
  std::uint64_t begins = 0;
  std::uint64_t bytes = 0;
  const auto returned = pipeline->Stream(
      prepared,
      {[&](const pdal::BackendRecord&) { ++begins; return true; },
       [&](const std::uint8_t*, std::size_t size) { bytes += size; return true; },
       [] { return true; }});
  CHECK(begins == 3);
  CHECK(returned == 12);
  CHECK(bytes == 12);
  const auto status = pipeline->registry().Get(prepared.request.request_id);
  CHECK(status && status->state == pdal::RequestState::kCompleted);
}

void TestContinuationAndMetadata() {
  auto backend = std::make_shared<FakeBackend>(
      std::vector<std::uint64_t>{150, 200, 250, 300, 350});
  auto pipeline = TestPipeline(backend, 2);
  auto first_request = pdal::NativeHttpAdapter().ToCanonicalRequest(
      NativeBody(), TestHeaders());
  const auto first = pipeline->Prepare(first_request);
  CHECK(first.result.records.size() == 2);
  CHECK(first.metadata.continuation.has_value());
  CheckError(pdal::ErrorClass::kInvalidRequest,
             [&] { (void)pipeline->Prepare(first_request); });

  auto tampered_body = NativeBody();
  std::string tampered = *first.metadata.continuation;
  const auto signature = tampered.find('.') + 2;
  tampered[signature] = tampered[signature] == 'A' ? 'B' : 'A';
  tampered_body.at("delivery").as_object()["continuation_token"] = tampered;
  auto tampered_request = pdal::NativeHttpAdapter().ToCanonicalRequest(
      tampered_body, TestHeaders());
  CheckError(pdal::ErrorClass::kInvalidRequest,
             [&] { (void)pipeline->Prepare(tampered_request); });

  auto next_body = NativeBody();
  next_body.at("delivery").as_object()["continuation_token"] =
      *first.metadata.continuation;
  auto next_request = pdal::NativeHttpAdapter().ToCanonicalRequest(
      next_body, TestHeaders());
  const auto second = pipeline->Prepare(std::move(next_request));
  CHECK(second.result.records.size() == 2);
  CHECK(second.result.records.front().timestamp_ns == 250);

  auto metadata_body = NativeBody();
  metadata_body.at("delivery").as_object()["mode"] = "metadata";
  metadata_body.at("delivery").as_object()["max_bytes"] = 1;
  auto metadata_request = pdal::NativeHttpAdapter().ToCanonicalRequest(
      metadata_body, TestHeaders());
  const auto metadata = pipeline->Prepare(std::move(metadata_request));
  CHECK(metadata.result.records.size() == 2);
  CHECK(metadata.metadata.byte_count == 0);
}

void TestStableContractsAndOperationRouting() {
  auto backend = std::make_shared<FakeBackend>(
      std::vector<std::uint64_t>{100, 200, 300, 400});
  auto live = std::make_shared<FakeLiveSource>();
  auto policy = std::make_shared<CapturingPolicy>();
  auto engine = TestQueryEngine(backend, live, policy);

  const auto native = pdal::NativeHttpAdapter().ToDataQuery(
      NativeBody(), pdal::Operation::kHistory, TestHeaders());
  const boost::json::object sovd{
      {"resource", "vehicle.camera.front"},
      {"timeRange", boost::json::object{{"from", 100}, {"to", 400}}},
      {"reason", "diagnostics"},
      {"contentFormat", "jpeg"},
      {"deliveryMode", "stream"}};
  const auto from_sovd = pdal::SovdAdapter().ToDataQuery(sovd, TestHeaders());
  CHECK(pdal::SemanticallyEquivalent(native, from_sovd));

  pdal::RequestContext context;
  context.principal = {"local-service-tool", "local", "service", {}};
  context.purpose = "diagnostics";
  pdal::PdalClient client(engine, context);
  CHECK(client.resources().size() == 2);
  const auto public_camera = client.describe("vehicle.camera.front");
  CHECK(public_camera.resource_id == "camera.front");
  CHECK(public_camera.historical_available);
  CHECK(public_camera.live_available);
  const auto sdk_json = boost::json::serialize(pdal::ResourceToJson(public_camera));
  CHECK(sdk_json.find("secret") == std::string::npos);
  auto camera = client.open("vehicle.camera.front");
  CHECK(camera.id() == "camera.front");

  pdal::RepresentationRequest jpeg;
  jpeg.format = "jpeg";
  auto history = camera.history(100, 400, {}, jpeg);
  CHECK(policy->last.has_value());
  auto sdk_query = *policy->last;
  auto canonical_native = native;
  canonical_native.resource = "camera.front";
  CHECK(pdal::SemanticallyEquivalent(sdk_query, canonical_native));
  const auto historical_sample = history.Next();
  CHECK(historical_sample.has_value());
  CHECK(historical_sample->resource_id == "camera.front");
  CHECK(backend->read_count == 1);
  CHECK(live->latest_calls == 0);
  CHECK(live->subscribe_calls == 0);
  history.Cancel();

  const auto latest = camera.latest(jpeg);
  CHECK(latest.resource_id == "camera.front");
  CHECK(policy->last->operation == pdal::Operation::kLatest);
  CHECK(policy->last->resource == "camera.front");
  CHECK(live->latest_calls == 1);

  auto subscription = camera.subscribe({}, jpeg);
  const auto live_sample = subscription.Next();
  CHECK(live_sample.has_value());
  CHECK(live_sample->resource_id == "camera.front");
  CHECK(policy->last->operation == pdal::Operation::kSubscribe);
  CHECK(policy->last->resource == "camera.front");
  CHECK(live->subscribe_calls == 1);
  std::uint64_t callback_samples = 0;
  camera.subscribe(
      [&](const pdal::DataSample& sample) {
        CHECK(sample.resource_id == "camera.front");
        ++callback_samples;
      },
      {}, jpeg);
  CHECK(callback_samples == 1);
  CHECK(live->subscribe_calls == 2);

  pdal::DataQuery metadata = canonical_native;
  metadata.request_id = pdal::GenerateRequestId();
  metadata.representation.format = "metadata";
  metadata.options.max_records = 2;
  const auto reads_before_metadata = backend->read_count;
  auto metadata_result = engine->Execute(std::move(metadata));
  const auto metadata_sample = metadata_result.stream.Next();
  CHECK(metadata_sample.has_value());
  CHECK(metadata_sample->payload->empty());
  CHECK(metadata_sample->metadata.at("payload_size") == 4);
  CHECK(backend->read_count == reads_before_metadata);
  CHECK(backend->last_cursor_limit == 2);
  metadata_result.stream.Cancel();

  pdal::DataQuery sampled = canonical_native;
  sampled.request_id = pdal::GenerateRequestId();
  sampled.options.max_records = 2;
  sampled.options.sampling.every_n = 2;
  sampled.representation.sampling = sampled.options.sampling;
  auto sampled_result = engine->Execute(std::move(sampled));
  const auto sampled_first = sampled_result.stream.Next();
  const auto sampled_second = sampled_result.stream.Next();
  CHECK(sampled_first && sampled_first->timestamp_ns == 100);
  CHECK(sampled_second && sampled_second->timestamp_ns == 300);
  CHECK(backend->last_cursor_limit == 4);
}

void TestBulkIsolationKeepsControlResponsive() {
  auto backend = std::make_shared<FakeBackend>(
      std::vector<std::uint64_t>{100, 200, 300, 400});
  auto engine = TestQueryEngine(backend, std::make_shared<FakeLiveSource>(),
                                std::make_shared<CapturingPolicy>());
  pdal::RequestContext context;
  context.purpose = "diagnostics";
  pdal::PdalClient client(engine, context);
  auto held = client.camera("front").history(100, 400);

  auto control = std::async(std::launch::async, [engine] {
    return engine->Describe("position").resource_id;
  });
  CHECK(control.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(control.get() == "position");

  auto metadata = std::async(std::launch::async, [engine, context]() mutable {
    pdal::RepresentationRequest representation;
    representation.format = "metadata";
    auto stream = pdal::PdalClient(engine, std::move(context))
        .camera("front").history(100, 400, {}, representation);
    const auto sample = stream.Next();
    return sample ? sample->resource_id : std::string{};
  });
  CHECK(metadata.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(metadata.get() == "camera.front");

  auto second = std::async(std::launch::async, [engine, context]() mutable {
    return pdal::PdalClient(engine, std::move(context))
        .camera("front").history(100, 400);
  });
  CHECK(second.wait_for(std::chrono::milliseconds(20)) ==
        std::future_status::timeout);
  held.Cancel();
  CHECK(second.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  auto admitted = second.get();
  admitted.Cancel();
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"resource mapping and public encapsulation", TestResourceMappingAndNoLeak},
      {"canonical request validation", TestCanonicalValidation},
      {"translator equivalence", TestTranslatorEquivalence},
      {"policy narrowing and denial", TestPolicyNarrowingAndDenial},
      {"planning and representation", TestPlanningAndRepresentation},
      {"stable errors", TestStableErrorResponse},
      {"authorization boundary and streaming", TestPipelineAuthorizationBoundaryAndStreaming},
      {"continuation and metadata", TestContinuationAndMetadata},
      {"stable contracts and operation routing", TestStableContractsAndOperationRouting},
      {"bulk isolation keeps control responsive", TestBulkIsolationKeepsControlResponsive},
  };
  for (const auto& [name, test] : tests) {
    try {
      test();
      if (failures == 0) std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
      ++failures;
    }
  }
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return 1;
  }
  std::cout << "all pDAL tests passed\n";
  return 0;
}
