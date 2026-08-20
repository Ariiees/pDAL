#include "pdal/catalog/resource_catalog.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <set>
#include <utility>

#include "pdal/model/error.h"

namespace pdal {

ResourceCatalog ResourceCatalog::LoadYaml(const std::filesystem::path& path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path.string());
  } catch (const YAML::Exception& error) {
    throw std::runtime_error("cannot load resource catalog " + path.string() +
                             ": " + error.what());
  }
  const auto resources = root["resources"];
  if (!resources || !resources.IsMap()) {
    throw std::runtime_error("resource catalog requires a resources map");
  }

  std::vector<ResourceDescriptor> parsed;
  for (const auto& entry : resources) {
    ResourceDescriptor resource;
    resource.resource_id = entry.first.as<std::string>();
    const auto node = entry.second;
    resource.resource_type = node["resource_type"].as<std::string>("sensor-data");
    resource.semantic_name = node["semantic_name"].as<std::string>(resource.resource_id);
    resource.modality = node["modality"].as<std::string>();
    resource.binding.backend_id = node["backend"].as<std::string>();
    resource.binding.source = node["source"].as<std::string>();
    if (const auto range = node["available_time_range"]) {
      if (range["start_ns"]) resource.available_time_range.start_ns = range["start_ns"].as<std::uint64_t>();
      if (range["end_ns"]) resource.available_time_range.end_ns = range["end_ns"].as<std::uint64_t>();
    }
    if (const auto representations = node["representations"];
        representations && representations.IsSequence()) {
      for (const auto& representation : representations) {
        resource.representations.push_back(
            {representation["format"].as<std::string>(),
             representation["content_type"].as<std::string>(),
             representation["stored"].as<bool>(true)});
      }
    }
    if (const auto attributes = node["attributes"]; attributes && attributes.IsMap()) {
      for (const auto& attribute : attributes) {
        resource.attributes.emplace(attribute.first.as<std::string>(),
                                    attribute.second.as<std::string>());
      }
    }
    if (resource.resource_id.empty() || resource.modality.empty() ||
        resource.binding.backend_id.empty() || resource.binding.source.empty() ||
        resource.representations.empty()) {
      throw std::runtime_error("resource " + resource.resource_id +
                               " has incomplete catalog metadata");
    }
    parsed.emplace_back(std::move(resource));
  }
  return FromResources(std::move(parsed));
}

ResourceCatalog ResourceCatalog::FromResources(
    std::vector<ResourceDescriptor> resources) {
  ResourceCatalog catalog;
  for (auto& resource : resources) {
    const auto id = resource.resource_id;
    if (id.empty() || !catalog.resources_.emplace(id, std::move(resource)).second) {
      throw std::runtime_error("duplicate or empty resource id: " + id);
    }
  }
  return catalog;
}

const ResourceDescriptor& ResourceCatalog::Get(
    const std::string& resource_id) const {
  const auto found = resources_.find(resource_id);
  if (found == resources_.end()) {
    throw PdalError(ErrorClass::kResourceNotFound,
                    "unknown logical resource: " + resource_id,
                    {{"resource_id", resource_id}});
  }
  return found->second;
}

std::optional<ResourceDescriptor> ResourceCatalog::Find(
    const std::string& resource_id) const {
  const auto found = resources_.find(resource_id);
  if (found == resources_.end()) return std::nullopt;
  return found->second;
}

std::vector<ResourceDescriptor> ResourceCatalog::List() const {
  std::vector<ResourceDescriptor> out;
  out.reserve(resources_.size());
  for (const auto& [_, resource] : resources_) out.push_back(resource);
  std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
    return left.resource_id < right.resource_id;
  });
  return out;
}

std::vector<std::string> ResourceCatalog::BackendIds() const {
  std::set<std::string> ids;
  for (const auto& [_, resource] : resources_) ids.insert(resource.binding.backend_id);
  return {ids.begin(), ids.end()};
}

}  // namespace pdal
