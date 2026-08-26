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

  std::vector<CatalogResource> parsed;
  for (const auto& entry : resources) {
    CatalogResource resource;
    resource.resource_id = entry.first.as<std::string>();
    const auto node = entry.second;
    resource.resource_type = node["resource_type"].as<std::string>("sensor-data");
    resource.semantic_name = node["display_name"].as<std::string>(
        node["semantic_name"].as<std::string>(resource.resource_id));
    resource.modality = node["modality"].as<std::string>();
    resource.kind = ParseResourceKind(node["kind"].as<std::string>("STREAM"));
    resource.description = node["description"].as<std::string>("");
    resource.schema = node["schema"].as<std::string>("");
    const auto historical = node["historical_binding"];
    if (historical) {
      resource.historical_binding.backend_id = historical["backend"].as<std::string>();
      resource.historical_binding.source = historical["source"].as<std::string>();
      if (const auto options = historical["options"]; options && options.IsMap()) {
        for (const auto& option : options) {
          resource.historical_binding.options.emplace(option.first.as<std::string>(),
                                                       option.second.as<std::string>());
        }
      }
    } else {
      resource.historical_binding.backend_id = node["backend"].as<std::string>("");
      resource.historical_binding.source = node["source"].as<std::string>("");
    }
    if (const auto live = node["live_binding"]) {
      BackendBinding binding;
      binding.backend_id = live["source_type"].as<std::string>(
          live["backend"].as<std::string>("ros"));
      binding.source = live["source"].as<std::string>();
      if (const auto options = live["options"]; options && options.IsMap()) {
        for (const auto& option : options) {
          binding.options.emplace(option.first.as<std::string>(),
                                  option.second.as<std::string>());
        }
      }
      resource.live_binding = std::move(binding);
    }
    if (const auto references = node["semantic_references"];
        references && references.IsSequence()) {
      for (const auto& reference : references) {
        resource.semantic_references.push_back(
            {reference["model"].as<std::string>(),
             reference["path"].as<std::string>()});
      }
    }
    if (const auto operations = node["operations"];
        operations && operations.IsSequence()) {
      for (const auto& operation : operations) {
        resource.operations.push_back(ParseOperation(operation.as<std::string>()));
      }
    }
    if (const auto aliases = node["aliases"]; aliases && aliases.IsSequence()) {
      for (const auto& alias : aliases) resource.aliases.push_back(alias.as<std::string>());
    }
    if (const auto limits = node["limits"]) {
      resource.limits.max_history_range_ns = limits["max_history_range_ns"].as<std::uint64_t>(0);
      resource.limits.max_records = limits["max_records"].as<std::uint64_t>(0);
      resource.limits.max_bytes = limits["max_bytes"].as<std::uint64_t>(0);
      resource.limits.stream_buffer_bytes = limits["stream_buffer_bytes"].as<std::uint64_t>(0);
    }
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
    const bool historical_incomplete =
        resource.historical_binding.backend_id.empty() !=
            resource.historical_binding.source.empty();
    const bool live_incomplete = resource.live_binding &&
        (resource.live_binding->backend_id.empty() ||
         resource.live_binding->source.empty());
    if (resource.resource_id.empty() || resource.modality.empty() ||
        (resource.historical_binding.backend_id.empty() && !resource.live_binding) ||
        historical_incomplete || live_incomplete ||
        resource.representations.empty() || resource.operations.empty()) {
      throw std::runtime_error("resource " + resource.resource_id +
                               " has incomplete catalog metadata");
    }
    parsed.emplace_back(std::move(resource));
  }
  return FromResources(std::move(parsed));
}

ResourceCatalog ResourceCatalog::FromResources(
    std::vector<CatalogResource> resources) {
  ResourceCatalog catalog;
  for (auto& resource : resources) {
    resource.historical_available = !resource.historical_binding.backend_id.empty();
    resource.live_available = resource.live_binding.has_value();
    const auto id = resource.resource_id;
    const auto aliases = resource.aliases;
    if (id.empty() || catalog.aliases_.contains(id) ||
        !catalog.resources_.emplace(id, std::move(resource)).second) {
      throw std::runtime_error("duplicate or empty resource id: " + id);
    }
    for (const auto& alias : aliases) {
      if (alias.empty() || catalog.resources_.contains(alias) ||
          !catalog.aliases_.emplace(alias, id).second) {
        throw std::runtime_error("duplicate or invalid resource alias: " + alias);
      }
    }
  }
  return catalog;
}

const CatalogResource& ResourceCatalog::Get(
    const std::string& resource_id) const {
  const auto canonical = CanonicalId(resource_id);
  const auto found = resources_.find(canonical);
  if (found == resources_.end()) {
    throw PdalError(ErrorClass::kResourceNotFound,
                    "unknown logical resource: " + resource_id,
                    {{"resource_id", resource_id}});
  }
  return found->second;
}

std::string ResourceCatalog::CanonicalId(const std::string& resource_id) const {
  if (resources_.contains(resource_id)) return resource_id;
  const auto alias = aliases_.find(resource_id);
  if (alias != aliases_.end()) return alias->second;
  throw PdalError(ErrorClass::kResourceNotFound,
                  "unknown logical resource: " + resource_id,
                  {{"resource_id", resource_id}});
}

std::optional<ResourceDescriptor> ResourceCatalog::Find(
    const std::string& resource_id) const {
  std::string canonical;
  try {
    canonical = CanonicalId(resource_id);
  } catch (const PdalError&) {
    return std::nullopt;
  }
  const auto found = resources_.find(canonical);
  if (found == resources_.end()) return std::nullopt;
  return static_cast<const ResourceDescriptor&>(found->second);
}

std::vector<ResourceDescriptor> ResourceCatalog::List() const {
  std::vector<ResourceDescriptor> out;
  out.reserve(resources_.size());
  for (const auto& [_, resource] : resources_) {
    out.push_back(static_cast<const ResourceDescriptor&>(resource));
  }
  std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
    return left.resource_id < right.resource_id;
  });
  return out;
}

std::vector<CatalogResource> ResourceCatalog::Entries() const {
  std::vector<CatalogResource> out;
  out.reserve(resources_.size());
  for (const auto& [_, resource] : resources_) out.push_back(resource);
  std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
    return left.resource_id < right.resource_id;
  });
  return out;
}

std::vector<std::string> ResourceCatalog::BackendIds() const {
  std::set<std::string> ids;
  for (const auto& [_, resource] : resources_) {
    if (!resource.historical_binding.backend_id.empty()) {
      ids.insert(resource.historical_binding.backend_id);
    }
  }
  return {ids.begin(), ids.end()};
}

}  // namespace pdal
