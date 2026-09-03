#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pdal/catalog/resource_catalog.h"
#include "pdal/model/types.h"

namespace pdal {

struct RolePolicy {
  std::unordered_set<std::string> resources;
  std::unordered_set<std::string> purposes;
  std::unordered_set<std::string> transformations;
  std::optional<std::uint64_t> earliest_time_ns;
  std::optional<std::uint64_t> latest_time_ns;
  std::uint64_t max_time_span_ns = 0;
  std::uint64_t max_records = 0;
  std::uint64_t max_bytes = 0;
};

class PolicyEngine {
 public:
  virtual ~PolicyEngine() = default;
  // `operation` lets live operations (LATEST/SUBSCRIBE) skip the historical
  // time-window checks while still enforcing role, purpose, resource, and
  // record/byte limits. HISTORY keeps the full check set.
  virtual AuthorizedAccessPlan Authorize(
      const PdalRequest& request, const ResourceCatalog& catalog,
      Operation operation = Operation::kHistory) const = 0;
  virtual std::string version() const = 0;
};

class YamlPolicyEngine final : public PolicyEngine {
 public:
  static YamlPolicyEngine Load(const std::filesystem::path& path);
  static YamlPolicyEngine FromPolicies(
      std::string version, std::uint64_t plan_ttl_ns,
      std::unordered_map<std::string, RolePolicy> policies);

  AuthorizedAccessPlan Authorize(
      const PdalRequest& request, const ResourceCatalog& catalog,
      Operation operation = Operation::kHistory) const override;
  std::string version() const override { return version_; }

 private:
  std::string version_;
  std::uint64_t plan_ttl_ns_ = 60ULL * 1000ULL * 1000ULL * 1000ULL;
  std::unordered_map<std::string, RolePolicy> policies_;
};

}  // namespace pdal
