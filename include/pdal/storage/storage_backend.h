#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pdal/catalog/resource_catalog.h"
#include "pdal/model/types.h"

namespace pdal {

struct BackendCapabilities {
  std::string backend_id;
  bool supports_hot_tier = false;
  bool supports_cold_tier = false;
  bool supports_streaming = true;
  std::uint64_t max_query_records = 0;
};

using ByteSink = std::function<bool(const std::uint8_t*, std::size_t)>;

class StorageBackend {
 public:
  virtual ~StorageBackend() = default;
  virtual BackendCapabilities GetCapabilities() const = 0;
  virtual void Resolve(const ResourceDescriptor& resource) const = 0;
  virtual std::vector<BackendRecord> Query(
      const ExecutionTask& task, const ResourceCatalog& catalog) const = 0;
  virtual std::uint64_t Read(const BackendRecord& record,
                             const ByteSink& sink) const = 0;
};

class BackendRegistry {
 public:
  void Register(std::shared_ptr<StorageBackend> backend);
  const StorageBackend& Get(const std::string& backend_id) const;
  std::vector<BackendCapabilities> Capabilities() const;

 private:
  std::unordered_map<std::string, std::shared_ptr<StorageBackend>> backends_;
};

}  // namespace pdal
