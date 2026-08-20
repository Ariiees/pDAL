#pragma once

#include <filesystem>
#include <memory>

#include "pdal/storage/storage_backend.h"

namespace pdal {

class AvsStorageBackend final : public StorageBackend {
 public:
  explicit AvsStorageBackend(std::filesystem::path ssd_root);
  ~AvsStorageBackend() override;

  BackendCapabilities GetCapabilities() const override;
  void Resolve(const ResourceDescriptor& resource) const override;
  std::vector<BackendRecord> Query(
      const ExecutionTask& task, const ResourceCatalog& catalog) const override;
  std::uint64_t Read(const BackendRecord& record,
                     const ByteSink& sink) const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pdal
