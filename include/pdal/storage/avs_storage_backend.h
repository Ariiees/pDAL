#pragma once

#include <filesystem>
#include <memory>

#include "pdal/storage/storage_backend.h"

namespace pdal {

class AvsStorageBackend final : public StorageBackend {
 public:
  explicit AvsStorageBackend(
      std::filesystem::path ssd_root,
      std::filesystem::path hdd_root = "/home/avs/DATA/HDD");
  ~AvsStorageBackend() override;

  BackendCapabilities GetCapabilities() const override;
  void Resolve(const CatalogResource& resource) const override;
  std::vector<BackendRecord> Query(
      const ExecutionTask& task, const ResourceCatalog& catalog) const override;
  std::unique_ptr<HistoryCursor> OpenHistory(
      const HistoryCursorRequest& request,
      const ResourceCatalog& catalog) const override;
  std::uint64_t Read(const BackendRecord& record,
                     const ByteSink& sink) const override;
  std::shared_ptr<const std::vector<std::uint8_t>> ReadPayload(
      const BackendRecord& record) const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pdal
