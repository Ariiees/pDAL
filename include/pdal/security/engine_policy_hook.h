#pragma once

#include <memory>

#include "pdal/catalog/resource_catalog.h"
#include "pdal/policy/policy_engine.h"
#include "pdal/security/hooks.h"

namespace pdal {

// Production PolicyHook for the QueryEngine. It reuses the same PolicyEngine
// decision as the compatible bulk pipeline, so both request paths enforce one
// set of role/purpose/resource/time/record/byte rules. The hook may only
// narrow a DataQuery; a denial throws PdalError(kForbidden|kUnauthenticated)
// before any backend access.
class EnginePolicyHook final : public PolicyHook {
 public:
  EnginePolicyHook(ResourceCatalog catalog,
                   std::shared_ptr<const PolicyEngine> engine);

  DataQuery Apply(const DataQuery& query,
                  const ResourceDescriptor& resource) const override;

 private:
  ResourceCatalog catalog_;
  std::shared_ptr<const PolicyEngine> engine_;
};

}  // namespace pdal
