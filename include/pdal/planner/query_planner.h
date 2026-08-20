#pragma once

#include "pdal/catalog/resource_catalog.h"
#include "pdal/model/types.h"

namespace pdal {

class QueryPlanner {
 public:
  ExecutionPlan Plan(const AuthorizedAccessPlan& access_plan,
                     const ResourceCatalog& catalog) const;
};

}  // namespace pdal
