#include "pdal/model/json.h"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

namespace pdal {
namespace {

boost::json::object TimeToJson(const TimeRange& time) {
  return {{"start_ns", time.start_ns}, {"end_ns", time.end_ns}};
}

boost::json::object RepresentationToJson(const RepresentationRequest& representation) {
  boost::json::object out{{"format", representation.format}};
  if (representation.quality) out["quality"] = *representation.quality;
  boost::json::object sampling{{"every_n", representation.sampling.every_n}};
  if (representation.sampling.max_frequency_hz) {
    sampling["max_frequency_hz"] = *representation.sampling.max_frequency_hz;
  }
  out["sampling"] = std::move(sampling);
  boost::json::array transformations;
  for (const auto& value : representation.transformations) {
    transformations.emplace_back(value);
  }
  out["transformations"] = std::move(transformations);
  return out;
}

}  // namespace

boost::json::object PrincipalToJson(const Principal& principal) {
  boost::json::object attributes;
  for (const auto& [key, value] : principal.attributes) attributes[key] = value;
  return {{"principal_id", principal.principal_id},
          {"organization", principal.organization},
          {"role", principal.role},
          {"attributes", std::move(attributes)}};
}

boost::json::object ResourceToJson(const ResourceDescriptor& resource) {
  boost::json::array representations;
  for (const auto& representation : resource.representations) {
    representations.emplace_back(boost::json::object{
        {"format", representation.format},
        {"content_type", representation.content_type},
        {"stored", representation.stored}});
  }
  boost::json::object attributes;
  for (const auto& [key, value] : resource.attributes) attributes[key] = value;
  boost::json::object available;
  if (resource.available_time_range.start_ns) {
    available["start_ns"] = *resource.available_time_range.start_ns;
  } else {
    available["start_ns"] = nullptr;
  }
  if (resource.available_time_range.end_ns) {
    available["end_ns"] = *resource.available_time_range.end_ns;
  } else {
    available["end_ns"] = nullptr;
  }
  return {{"resource_id", resource.resource_id},
          {"resource_type", resource.resource_type},
          {"semantic_name", resource.semantic_name},
          {"modality", resource.modality},
          {"available_time_range", std::move(available)},
          {"representations", std::move(representations)},
          {"attributes", std::move(attributes)}};
}

boost::json::object RequestToJson(const PdalRequest& request,
                                  bool include_principal) {
  boost::json::array resources;
  for (const auto& resource : request.resources) resources.emplace_back(resource);
  boost::json::object delivery{{"mode", DeliveryModeName(request.delivery.mode)},
                               {"max_records", request.delivery.max_records},
                               {"max_bytes", request.delivery.max_bytes}};
  if (request.delivery.continuation_token) {
    delivery["continuation_token"] = *request.delivery.continuation_token;
  }
  boost::json::object out{{"api_version", request.api_version},
                          {"request_id", request.request_id},
                          {"purpose", request.purpose},
                          {"resources", std::move(resources)},
                          {"time", TimeToJson(request.time)},
                          {"representation", RepresentationToJson(request.representation)},
                          {"delivery", std::move(delivery)},
                          {"context", request.context}};
  if (include_principal) out["principal"] = PrincipalToJson(request.principal);
  return out;
}

boost::json::object AccessPlanToJson(const AuthorizedAccessPlan& plan) {
  boost::json::array resources;
  for (const auto& resource : plan.resources()) {
    resources.emplace_back(boost::json::object{
        {"resource_id", resource.resource_id},
        {"time", TimeToJson(resource.time)},
        {"representation", RepresentationToJson(resource.representation)}});
  }
  return {{"request_id", plan.request_id()},
          {"principal_id", plan.principal_id()},
          {"authorized_resources", std::move(resources)},
          {"limits", boost::json::object{{"max_records", plan.limits().max_records},
                                          {"max_bytes", plan.limits().max_bytes}}},
          {"issued_at_ns", plan.issued_at_ns()},
          {"expires_at_ns", plan.expires_at_ns()},
          {"policy_version", plan.policy_version()}};
}

boost::json::object ExecutionPlanToJson(const ExecutionPlan& plan) {
  boost::json::array operations;
  for (const auto operation : plan.operations) {
    operations.emplace_back(PlanOperationName(operation));
  }
  boost::json::array tasks;
  for (const auto& task : plan.tasks) {
    tasks.emplace_back(boost::json::object{{"resource_id", task.resource_id},
                                           {"backend", task.backend_id},
                                           {"time", TimeToJson(task.time)},
                                           {"representation",
                                            RepresentationToJson(task.representation)}});
  }
  return {{"request_id", plan.request_id},
          {"principal_id", plan.principal_id},
          {"policy_version", plan.policy_version},
          {"operations", std::move(operations)},
          {"tasks", std::move(tasks)},
          {"limits", boost::json::object{{"max_records", plan.limits.max_records},
                                          {"max_bytes", plan.limits.max_bytes}}}};
}

boost::json::object ResponseMetadataToJson(const ResponseMetadata& metadata) {
  boost::json::array resources;
  for (const auto& resource : metadata.resources) resources.emplace_back(resource);
  boost::json::object representations;
  for (const auto& [resource, format] : metadata.representations) {
    representations[resource] = format;
  }
  boost::json::object out{{"api_version", metadata.api_version},
                          {"request_id", metadata.request_id},
                          {"resources", std::move(resources)},
                          {"time_range", TimeToJson(metadata.time_range)},
                          {"representations", std::move(representations)},
                          {"record_count", metadata.record_count},
                          {"byte_count", metadata.byte_count},
                          {"policy_version", metadata.policy_version}};
  out["continuation"] = metadata.continuation
                              ? boost::json::value(*metadata.continuation)
                              : boost::json::value(nullptr);
  return out;
}

}  // namespace pdal
