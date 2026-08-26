#pragma once

#include <boost/json/object.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pdal {

inline constexpr const char* kApiVersion = "pdal/v1";

struct TimeRange {
  std::uint64_t start_ns = 0;
  std::uint64_t end_ns = 0;

  bool operator==(const TimeRange&) const = default;
};

struct Principal {
  std::string principal_id;
  std::string organization;
  std::string role;
  std::unordered_map<std::string, std::string> attributes;

  bool operator==(const Principal&) const = default;
};

struct SamplingSpec {
  std::optional<double> max_frequency_hz;
  std::uint32_t every_n = 1;

  bool operator==(const SamplingSpec&) const = default;
};

struct RepresentationRequest {
  std::string format = "native";
  std::optional<int> quality;
  SamplingSpec sampling;
  std::vector<std::string> transformations;

  bool operator==(const RepresentationRequest&) const = default;
};

enum class DeliveryMode { kStream, kMetadata };

struct DeliverySpec {
  DeliveryMode mode = DeliveryMode::kStream;
  std::uint64_t max_records = 100;
  std::uint64_t max_bytes = 64ULL * 1024ULL * 1024ULL;
  std::optional<std::string> continuation_token;

  bool operator==(const DeliverySpec&) const = default;
};

struct PdalRequest {
  std::string api_version = kApiVersion;
  std::string request_id;
  Principal principal;
  std::string purpose;
  std::vector<std::string> resources;
  TimeRange time;
  RepresentationRequest representation;
  DeliverySpec delivery;
  boost::json::object context;
};

struct AvailableTimeRange {
  std::optional<std::uint64_t> start_ns;
  std::optional<std::uint64_t> end_ns;
};

enum class ResourceKind { kSignal, kStream, kObject };

enum class Operation { kDiscover, kDescribe, kHistory, kLatest, kSubscribe };

struct SemanticReference {
  std::string model;
  std::string path;
};

struct ResourceLimits {
  std::uint64_t max_history_range_ns = 0;
  std::uint64_t max_records = 0;
  std::uint64_t max_bytes = 0;
  std::uint64_t stream_buffer_bytes = 0;
};

struct RepresentationDescriptor {
  std::string format;
  std::string content_type;
  bool stored = true;
};

struct ResourceDescriptor {
  std::string resource_id;
  std::string resource_type;
  std::string semantic_name;
  std::string modality;
  AvailableTimeRange available_time_range;
  std::vector<RepresentationDescriptor> representations;
  std::unordered_map<std::string, std::string> attributes;
  ResourceKind kind = ResourceKind::kStream;
  std::string description;
  std::string schema;
  std::vector<SemanticReference> semantic_references;
  std::vector<Operation> operations;
  ResourceLimits limits;
  bool historical_available = false;
  bool live_available = false;
};

struct AccessLimits {
  std::uint64_t max_records = 0;
  std::uint64_t max_bytes = 0;
};

struct AuthorizedResource {
  std::string resource_id;
  TimeRange time;
  RepresentationRequest representation;
};

class AuthorizedAccessPlan {
 public:
  AuthorizedAccessPlan(std::string request_id, std::string principal_id,
                       std::vector<AuthorizedResource> resources,
                       AccessLimits limits, std::uint64_t issued_at_ns,
                       std::uint64_t expires_at_ns, std::string policy_version);

  const std::string& request_id() const { return request_id_; }
  const std::string& principal_id() const { return principal_id_; }
  const std::vector<AuthorizedResource>& resources() const { return resources_; }
  const AccessLimits& limits() const { return limits_; }
  std::uint64_t issued_at_ns() const { return issued_at_ns_; }
  std::uint64_t expires_at_ns() const { return expires_at_ns_; }
  const std::string& policy_version() const { return policy_version_; }

 private:
  std::string request_id_;
  std::string principal_id_;
  std::vector<AuthorizedResource> resources_;
  AccessLimits limits_;
  std::uint64_t issued_at_ns_;
  std::uint64_t expires_at_ns_;
  std::string policy_version_;
};

enum class PlanOperation {
  kResolveResource,
  kLocateTimeRange,
  kSelectRecords,
  kReadPayload,
  kTransformRepresentation,
  kStreamResult,
};

struct ExecutionTask {
  std::string resource_id;
  std::string backend_id;
  TimeRange time;
  RepresentationRequest representation;
};

struct ExecutionPlan {
  std::string request_id;
  std::string principal_id;
  std::string policy_version;
  std::vector<PlanOperation> operations;
  std::vector<ExecutionTask> tasks;
  AccessLimits limits;
};

class RecordLocator {
 public:
  virtual ~RecordLocator() = default;
};

struct BackendRecord {
  std::string resource_id;
  std::string backend_id;
  std::uint64_t timestamp_ns = 0;
  std::uint64_t payload_size = 0;
  std::string storage_tier;
  std::shared_ptr<const RecordLocator> locator;
};

struct PlannedResult {
  ExecutionPlan plan;
  std::vector<BackendRecord> records;
  std::optional<std::string> continuation_token;
  std::uint64_t records_considered = 0;
  std::uint64_t bytes_selected = 0;
};

struct ResponseMetadata {
  std::string api_version = kApiVersion;
  std::string request_id;
  std::vector<std::string> resources;
  TimeRange time_range;
  std::unordered_map<std::string, std::string> representations;
  std::uint64_t record_count = 0;
  std::uint64_t byte_count = 0;
  std::optional<std::string> continuation;
  std::string policy_version;
};

std::uint64_t SystemNowNs();
std::string GenerateRequestId();
std::string DeliveryModeName(DeliveryMode mode);
std::string PlanOperationName(PlanOperation operation);
std::string ResourceKindName(ResourceKind kind);
std::string OperationName(Operation operation);
ResourceKind ParseResourceKind(const std::string& value);
Operation ParseOperation(const std::string& value);

}  // namespace pdal
