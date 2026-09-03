#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pdal/audit/audit.h"
#include "pdal/catalog/resource_catalog.h"
#include "pdal/model/types.h"
#include "pdal/planner/query_planner.h"
#include "pdal/policy/policy_engine.h"
#include "pdal/privacy/privacy_stage.h"
#include "pdal/representation/representation_provider.h"
#include "pdal/storage/storage_backend.h"

namespace pdal {

class ContinuationCodec {
 public:
  explicit ContinuationCodec(std::string development_secret);
  std::string Encode(const PdalRequest& request,
                     std::uint64_t next_offset,
                     std::uint64_t expires_at_ns) const;
  std::uint64_t DecodeAndValidate(const std::string& token,
                                  const PdalRequest& request) const;

 private:
  std::string secret_;
};

enum class RequestState { kReceived, kPlanned, kStreaming, kCompleted, kFailed };

struct RequestStatus {
  std::string request_id;
  RequestState state = RequestState::kReceived;
  std::uint64_t records_selected = 0;
  std::uint64_t bytes_returned = 0;
  std::string error_code;
};

class RequestRegistry {
 public:
  explicit RequestRegistry(std::size_t capacity = 1024) : capacity_(capacity) {}
  bool Begin(RequestStatus status);
  void Put(RequestStatus status);
  std::optional<RequestStatus> Get(const std::string& request_id) const;

 private:
  std::size_t capacity_;
  mutable std::mutex mutex_;
  std::vector<std::string> order_;
  std::unordered_map<std::string, RequestStatus> statuses_;
};

struct PreparedQuery {
  PdalRequest request;
  AuthorizedAccessPlan access_plan;
  PlannedResult result;
  ResponseMetadata metadata;
  std::uint64_t started_at_ns = 0;
  std::uint64_t user_cpu_us_start = 0;
  std::uint64_t system_cpu_us_start = 0;
};

struct StreamCallbacks {
  std::function<bool(const BackendRecord&)> begin_record;
  ByteSink write_bytes;
  std::function<bool()> end_record;
};

class PdalPipeline {
 public:
  PdalPipeline(ResourceCatalog catalog,
               std::shared_ptr<const PolicyEngine> policy,
               std::shared_ptr<const BackendRegistry> backends,
               std::shared_ptr<AuditSink> audit,
               ContinuationCodec continuation,
               std::shared_ptr<RequestRegistry> registry,
               std::shared_ptr<const PrivacyStage> privacy_stage = nullptr);

  PreparedQuery Prepare(PdalRequest request) const;
  std::uint64_t Stream(const PreparedQuery& prepared,
                       const StreamCallbacks& callbacks) const;
  boost::json::object CapabilitiesJson() const;

  const ResourceCatalog& catalog() const { return catalog_; }
  const RequestRegistry& registry() const { return *registry_; }

 private:
  ResourceCatalog catalog_;
  std::shared_ptr<const PolicyEngine> policy_;
  std::shared_ptr<const BackendRegistry> backends_;
  std::shared_ptr<AuditSink> audit_;
  ContinuationCodec continuation_;
  std::shared_ptr<RequestRegistry> registry_;
  std::shared_ptr<const PrivacyStage> privacy_stage_;
  QueryPlanner planner_;
  RepresentationProvider representations_;
};

std::string RequestStateName(RequestState state);

}  // namespace pdal
