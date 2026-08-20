#pragma once

#include <boost/json/value.hpp>

#include <string>

#include "pdal/model/types.h"

namespace pdal {

boost::json::object PrincipalToJson(const Principal& principal);
boost::json::object ResourceToJson(const ResourceDescriptor& resource);
boost::json::object RequestToJson(const PdalRequest& request,
                                  bool include_principal = true);
boost::json::object AccessPlanToJson(const AuthorizedAccessPlan& plan);
boost::json::object ExecutionPlanToJson(const ExecutionPlan& plan);
boost::json::object ResponseMetadataToJson(const ResponseMetadata& metadata);

}  // namespace pdal
