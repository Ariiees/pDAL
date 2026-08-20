#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <cstdint>
#include <functional>
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

pdal::ResourceDescriptor Camera(std::string backend = "fake") {
  return {"vehicle.camera.front", "sensor-data", "Front camera", "image", {},
          {{"jpeg", "image/jpeg", true}}, {{"orientation", "front"}},
          {std::move(backend), "/secret/ros/camera/topic"}};
}

pdal::ResourceDescriptor Position(std::string backend = "fake") {
  return {"vehicle.position", "sensor-data", "Position", "gps", {},
          {{"avs-gps-binary", "application/vnd.avs.gps", true}}, {},
          {std::move(backend), "/secret/ros/gps/topic"}};
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

  void Resolve(const pdal::ResourceDescriptor& resource) const override {
    CHECK(resource.binding.backend_id == "fake");
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

  std::uint64_t Read(const pdal::BackendRecord& record,
                     const pdal::ByteSink& sink) const override {
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

 private:
  std::vector<std::uint64_t> timestamps_;
};

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
  CHECK(camera.modality == "image");
  CHECK(camera.binding.source == "/my_camera/pylon_ros2_camera_node/image_raw");
  const auto public_json = boost::json::serialize(pdal::ResourceToJson(camera));
  CHECK(public_json.find("/my_camera") == std::string::npos);
  const auto public_object = pdal::ResourceToJson(camera);
  CHECK(!public_object.contains("source"));
  CHECK(!public_object.contains("binding"));
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
}

void TestPipelineAuthorizationBoundaryAndStreaming() {
  auto backend = std::make_shared<FakeBackend>(
      std::vector<std::uint64_t>{100, 150, 250, 350, 400});
  auto pipeline = TestPipeline(backend);
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
  tampered.back() = tampered.back() == 'A' ? 'B' : 'A';
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
