#include "pdal/sdk/client.h"

#include <stdexcept>
#include <utility>

#include "pdal/model/error.h"

namespace pdal {

ResourceHandle::ResourceHandle(std::shared_ptr<const QueryEngine> engine,
                               std::string resource_id,
                               RequestContext context)
    : engine_(std::move(engine)),
      resource_id_(std::move(resource_id)),
      context_(std::move(context)) {
  if (!engine_) throw std::invalid_argument("ResourceHandle requires a QueryEngine");
  resource_id_ = engine_->CanonicalResourceId(resource_id_);
}

ResourceDescriptor ResourceHandle::describe() const {
  return engine_->Describe(resource_id_);
}

DataQuery ResourceHandle::Base(Operation operation,
                               RepresentationRequest representation,
                               QueryOptions options) const {
  if (options.sampling == SamplingSpec{} &&
      representation.sampling != SamplingSpec{}) {
    options.sampling = representation.sampling;
  }
  representation.sampling = options.sampling;
  DataQuery query;
  query.request_id = GenerateRequestId();
  query.resource = resource_id_;
  query.operation = operation;
  query.representation = std::move(representation);
  query.options = std::move(options);
  query.context = context_;
  return query;
}

DataStream ResourceHandle::history(
    std::uint64_t start_ns, std::uint64_t end_ns, QueryOptions options,
    RepresentationRequest representation) const {
  auto query = Base(Operation::kHistory, std::move(representation),
                    std::move(options));
  query.selector.time_range = TimeRange{start_ns, end_ns};
  return engine_->Execute(std::move(query)).stream;
}

DataSample ResourceHandle::latest(
    RepresentationRequest representation) const {
  auto result = engine_->Execute(
      Base(Operation::kLatest, std::move(representation), {}));
  auto sample = result.stream.Next();
  if (!sample) throw PdalError(ErrorClass::kNoData, "no live sample is available");
  return std::move(*sample);
}

DataStream ResourceHandle::subscribe(
    QueryOptions options, RepresentationRequest representation) const {
  return engine_->Execute(Base(Operation::kSubscribe,
                               std::move(representation),
                               std::move(options))).stream;
}

void ResourceHandle::subscribe(
    std::function<void(const DataSample&)> callback, QueryOptions options,
    RepresentationRequest representation) const {
  if (!callback) throw std::invalid_argument("subscribe callback is empty");
  auto stream = subscribe(std::move(options), std::move(representation));
  for (const auto& sample : stream) callback(sample);
}

PdalClient::PdalClient(std::shared_ptr<const QueryEngine> engine,
                       RequestContext context)
    : engine_(std::move(engine)), context_(std::move(context)) {
  if (!engine_) throw std::invalid_argument("PdalClient requires a QueryEngine");
}

std::vector<ResourceDescriptor> PdalClient::resources() const {
  return engine_->Discover();
}

ResourceDescriptor PdalClient::describe(
    const std::string& resource_id) const {
  return engine_->Describe(resource_id);
}

ResourceHandle PdalClient::open(const std::string& resource_id) const {
  return ResourceHandle(engine_, resource_id, context_);
}

ResourceHandle PdalClient::camera(const std::string& position) const {
  return open("camera." + position);
}

ResourceHandle PdalClient::lidar(const std::string& position) const {
  return open("lidar." + position);
}

ResourceHandle PdalClient::position() const { return open("position"); }

}  // namespace pdal
