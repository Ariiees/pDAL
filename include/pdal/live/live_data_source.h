#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pdal/catalog/resource_catalog.h"
#include "pdal/model/data.h"

namespace pdal {

struct LiveSourceCapabilities {
  std::string source_id;
  bool supports_latest = true;
  bool supports_subscribe = true;
  std::uint64_t max_subscriptions = 0;
};

class ILiveDataSource {
 public:
  virtual ~ILiveDataSource() = default;
  virtual LiveSourceCapabilities GetCapabilities() const = 0;
  virtual std::optional<DataSample> Latest(
      const ResourceDescriptor& resource,
      const RepresentationRequest& representation) const = 0;
  virtual DataStream Subscribe(
      const ResourceDescriptor& resource,
      const RepresentationRequest& representation,
      const QueryOptions& options) const = 0;
};

class LiveSourceRegistry {
 public:
  void Register(std::shared_ptr<ILiveDataSource> source);
  const ILiveDataSource& Get(const std::string& source_id) const;
  std::vector<LiveSourceCapabilities> Capabilities() const;

 private:
  std::unordered_map<std::string, std::shared_ptr<ILiveDataSource>> sources_;
};

}  // namespace pdal
