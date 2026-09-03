#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include "pdal/audit/audit.h"
#include "pdal/catalog/resource_catalog.h"
#include "pdal/live/live_data_source.h"
#include "pdal/model/data.h"
#include "pdal/privacy/privacy_stage.h"
#include "pdal/representation/representation_provider.h"
#include "pdal/security/hooks.h"
#include "pdal/storage/storage_backend.h"

namespace pdal {

class QueryEngine {
 public:
  QueryEngine(ResourceCatalog catalog,
              std::shared_ptr<const BackendRegistry> historical_backends,
              std::shared_ptr<const LiveSourceRegistry> live_sources,
              std::shared_ptr<const PolicyHook> policy_hook,
              std::shared_ptr<const PrivacyHook> privacy_hook,
              std::shared_ptr<AuditSink> audit,
              std::size_t max_concurrent_bulk_queries = 2,
              std::shared_ptr<const PrivacyStage> privacy_stage = nullptr);

  std::vector<ResourceDescriptor> Discover(std::string request_id = {}) const;
  ResourceDescriptor Describe(const std::string& resource_id,
                              std::string request_id = {}) const;
  DataResult Execute(DataQuery query) const;
  std::string CanonicalResourceId(const std::string& resource_id) const;

 private:
  struct BulkGate;

  ResourceCatalog catalog_;
  std::shared_ptr<const BackendRegistry> historical_backends_;
  std::shared_ptr<const LiveSourceRegistry> live_sources_;
  std::shared_ptr<const PolicyHook> policy_hook_;
  std::shared_ptr<const PrivacyHook> privacy_hook_;
  std::shared_ptr<const PrivacyStage> privacy_stage_;
  std::shared_ptr<AuditSink> audit_;
  std::shared_ptr<BulkGate> bulk_gate_;
  RepresentationProvider representations_;
};

}  // namespace pdal
