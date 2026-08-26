#include "pdal/planner/query_planner.h"

namespace pdal {

ExecutionPlan QueryPlanner::Plan(const AuthorizedAccessPlan& access_plan,
                                 const ResourceCatalog& catalog) const {
  ExecutionPlan plan;
  plan.request_id = access_plan.request_id();
  plan.principal_id = access_plan.principal_id();
  plan.policy_version = access_plan.policy_version();
  plan.limits = access_plan.limits();
  plan.operations = {PlanOperation::kResolveResource,
                     PlanOperation::kLocateTimeRange,
                     PlanOperation::kSelectRecords,
                     PlanOperation::kReadPayload,
                     PlanOperation::kTransformRepresentation,
                     PlanOperation::kStreamResult};
  for (const auto& authorized : access_plan.resources()) {
    const auto& descriptor = catalog.Get(authorized.resource_id);
    plan.tasks.push_back({authorized.resource_id,
                          descriptor.historical_binding.backend_id,
                          authorized.time, authorized.representation});
  }
  return plan;
}

}  // namespace pdal
