#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pdal/model/data.h"
#include "pdal/query_engine.h"

namespace pdal {

class ResourceHandle {
 public:
  ResourceHandle(std::shared_ptr<const QueryEngine> engine,
                 std::string resource_id,
                 RequestContext context = {});

  ResourceDescriptor describe() const;
  DataStream history(std::uint64_t start_ns, std::uint64_t end_ns,
                     QueryOptions options = {},
                     RepresentationRequest representation = {}) const;
  DataSample latest(RepresentationRequest representation = {}) const;
  DataStream subscribe(QueryOptions options = {},
                       RepresentationRequest representation = {}) const;
  void subscribe(std::function<void(const DataSample&)> callback,
                 QueryOptions options = {},
                 RepresentationRequest representation = {}) const;
  const std::string& id() const { return resource_id_; }

 private:
  DataQuery Base(Operation operation,
                 RepresentationRequest representation,
                 QueryOptions options) const;

  std::shared_ptr<const QueryEngine> engine_;
  std::string resource_id_;
  RequestContext context_;
};

class PdalClient {
 public:
  explicit PdalClient(std::shared_ptr<const QueryEngine> engine,
                      RequestContext context = {});

  std::vector<ResourceDescriptor> resources() const;
  ResourceDescriptor describe(const std::string& resource_id) const;
  ResourceHandle open(const std::string& resource_id) const;
  ResourceHandle camera(const std::string& position) const;
  ResourceHandle lidar(const std::string& position) const;
  ResourceHandle position() const;

 private:
  std::shared_ptr<const QueryEngine> engine_;
  RequestContext context_;
};

}  // namespace pdal
