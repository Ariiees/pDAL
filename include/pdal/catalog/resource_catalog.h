#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pdal/model/types.h"

namespace pdal {

class ResourceCatalog {
 public:
  static ResourceCatalog LoadYaml(const std::filesystem::path& path);
  static ResourceCatalog FromResources(std::vector<ResourceDescriptor> resources);

  const ResourceDescriptor& Get(const std::string& resource_id) const;
  std::optional<ResourceDescriptor> Find(const std::string& resource_id) const;
  std::vector<ResourceDescriptor> List() const;
  std::vector<std::string> BackendIds() const;

 private:
  std::unordered_map<std::string, ResourceDescriptor> resources_;
};

}  // namespace pdal
