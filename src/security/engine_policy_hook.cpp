#include "pdal/security/engine_policy_hook.h"

#include <algorithm>
#include <utility>

#include "pdal/model/error.h"

namespace pdal {

EnginePolicyHook::EnginePolicyHook(ResourceCatalog catalog,
                                   std::shared_ptr<const PolicyEngine> engine)
    : catalog_(std::move(catalog)), engine_(std::move(engine)) {
  if (!engine_) {
    throw std::invalid_argument("EnginePolicyHook requires a policy engine");
  }
}

DataQuery EnginePolicyHook::Apply(const DataQuery& query,
                                  const ResourceDescriptor&) const {
  // ToPdalRequest carries the verified principal, purpose, resource, time
  // window, representation, and requested limits across to the shared engine.
  PdalRequest request = ToPdalRequest(query);
  const auto plan = engine_->Authorize(request, catalog_, query.operation);
  if (plan.resources().empty()) {
    throw PdalError(ErrorClass::kForbidden,
                    "resource is not permitted for this principal");
  }
  const auto& authorized = plan.resources().front();

  DataQuery narrowed = query;
  if (query.operation == Operation::kHistory && query.selector.time_range) {
    TimeRange range = *query.selector.time_range;
    range.start_ns = std::max(range.start_ns, authorized.time.start_ns);
    range.end_ns = std::min(range.end_ns, authorized.time.end_ns);
    if (range.start_ns > range.end_ns) {
      throw PdalError(ErrorClass::kForbidden,
                      "requested time range is outside the permitted range");
    }
    narrowed.selector.time_range = range;
  }
  if (plan.limits().max_records > 0) {
    narrowed.options.max_records =
        std::min(narrowed.options.max_records, plan.limits().max_records);
  }
  if (plan.limits().max_bytes > 0) {
    narrowed.options.max_bytes =
        std::min(narrowed.options.max_bytes, plan.limits().max_bytes);
  }
  narrowed.representation.transformations =
      authorized.representation.transformations;
  return narrowed;
}

}  // namespace pdal
