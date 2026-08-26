#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pdal/model/types.h"

namespace pdal {

struct BackendBinding {
  std::string backend_id;
  std::string source;
  std::unordered_map<std::string, std::string> options;
};

// Private catalog record. Physical mappings never appear in the stable
// ResourceDescriptor returned by QueryEngine or PdalClient.
struct CatalogResource : ResourceDescriptor {
  BackendBinding historical_binding;
  std::optional<BackendBinding> live_binding;
  std::vector<std::string> aliases;
};

class ResourceCatalog {
 public:
  static ResourceCatalog LoadYaml(const std::filesystem::path& path);
  static ResourceCatalog FromResources(std::vector<CatalogResource> resources);

  const CatalogResource& Get(const std::string& resource_id) const;
  std::string CanonicalId(const std::string& resource_id) const;
  std::optional<ResourceDescriptor> Find(const std::string& resource_id) const;
  std::vector<ResourceDescriptor> List() const;
  std::vector<CatalogResource> Entries() const;
  std::vector<std::string> BackendIds() const;

 private:
  std::unordered_map<std::string, CatalogResource> resources_;
  std::unordered_map<std::string, std::string> aliases_;
};

}  // namespace pdal
