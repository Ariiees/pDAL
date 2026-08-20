#include "pdal/policy/policy_engine.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <limits>
#include <utility>

#include "pdal/model/error.h"

namespace pdal {
namespace {

std::unordered_set<std::string> StringSet(const YAML::Node& node) {
  std::unordered_set<std::string> values;
  if (node && node.IsSequence()) {
    for (const auto& value : node) values.insert(value.as<std::string>());
  }
  return values;
}

bool Permits(const std::unordered_set<std::string>& values,
             const std::string& value) {
  return values.contains("*") || values.contains(value);
}

std::uint64_t Bounded(std::uint64_t requested, std::uint64_t allowed) {
  if (requested == 0) return allowed;
  if (allowed == 0) return requested;
  return std::min(requested, allowed);
}

}  // namespace

YamlPolicyEngine YamlPolicyEngine::Load(const std::filesystem::path& path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path.string());
  } catch (const YAML::Exception& error) {
    throw std::runtime_error("cannot load policy " + path.string() + ": " +
                             error.what());
  }
  std::unordered_map<std::string, RolePolicy> policies;
  const auto roles = root["roles"];
  if (!roles || !roles.IsMap()) throw std::runtime_error("policy requires roles map");
  for (const auto& entry : roles) {
    RolePolicy policy;
    const auto node = entry.second;
    policy.resources = StringSet(node["resources"]);
    policy.purposes = StringSet(node["purposes"]);
    policy.transformations = StringSet(node["transformations"]);
    if (node["earliest_time_ns"]) policy.earliest_time_ns = node["earliest_time_ns"].as<std::uint64_t>();
    if (node["latest_time_ns"]) policy.latest_time_ns = node["latest_time_ns"].as<std::uint64_t>();
    policy.max_time_span_ns = node["max_time_span_ns"].as<std::uint64_t>(0);
    policy.max_records = node["max_records"].as<std::uint64_t>(0);
    policy.max_bytes = node["max_bytes"].as<std::uint64_t>(0);
    policies.emplace(entry.first.as<std::string>(), std::move(policy));
  }
  return FromPolicies(root["version"].as<std::string>("development-v1"),
                      root["plan_ttl_ns"].as<std::uint64_t>(
                          60ULL * 1000ULL * 1000ULL * 1000ULL),
                      std::move(policies));
}

YamlPolicyEngine YamlPolicyEngine::FromPolicies(
    std::string version, std::uint64_t plan_ttl_ns,
    std::unordered_map<std::string, RolePolicy> policies) {
  YamlPolicyEngine engine;
  engine.version_ = std::move(version);
  engine.plan_ttl_ns_ = plan_ttl_ns;
  engine.policies_ = std::move(policies);
  return engine;
}

AuthorizedAccessPlan YamlPolicyEngine::Authorize(
    const PdalRequest& request, const ResourceCatalog& catalog) const {
  if (request.principal.principal_id.empty()) {
    throw PdalError(ErrorClass::kUnauthenticated, "a principal is required");
  }
  const auto policy_it = policies_.find(request.principal.role);
  if (policy_it == policies_.end()) {
    throw PdalError(ErrorClass::kForbidden, "principal role has no policy");
  }
  const auto& policy = policy_it->second;
  if (!Permits(policy.purposes, request.purpose)) {
    throw PdalError(ErrorClass::kForbidden,
                    "purpose is not permitted for this principal");
  }

  TimeRange narrowed = request.time;
  if (policy.earliest_time_ns) narrowed.start_ns = std::max(narrowed.start_ns, *policy.earliest_time_ns);
  if (policy.latest_time_ns) narrowed.end_ns = std::min(narrowed.end_ns, *policy.latest_time_ns);
  if (narrowed.start_ns > narrowed.end_ns) {
    throw PdalError(ErrorClass::kForbidden,
                    "requested time range is outside the permitted range");
  }
  if (policy.max_time_span_ns > 0 &&
      narrowed.end_ns - narrowed.start_ns > policy.max_time_span_ns) {
    narrowed.end_ns = narrowed.start_ns + policy.max_time_span_ns;
  }

  std::vector<AuthorizedResource> authorized;
  for (const auto& resource_id : request.resources) {
    (void)catalog.Get(resource_id);
    if (!Permits(policy.resources, resource_id)) continue;
    auto representation = request.representation;
    std::erase_if(representation.transformations, [&](const std::string& value) {
      return !Permits(policy.transformations, value);
    });
    authorized.push_back({resource_id, narrowed, std::move(representation)});
  }
  if (authorized.empty()) {
    throw PdalError(ErrorClass::kForbidden,
                    "none of the requested resources is permitted");
  }

  const auto issued = SystemNowNs();
  const auto expires = plan_ttl_ns_ > std::numeric_limits<std::uint64_t>::max() - issued
                           ? std::numeric_limits<std::uint64_t>::max()
                           : issued + plan_ttl_ns_;
  return AuthorizedAccessPlan(
      request.request_id, request.principal.principal_id, std::move(authorized),
      {Bounded(request.delivery.max_records, policy.max_records),
       Bounded(request.delivery.max_bytes, policy.max_bytes)},
      issued, expires, version_);
}

}  // namespace pdal
