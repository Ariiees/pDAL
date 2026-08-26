#include "pdal/storage/storage_backend.h"

#include <utility>

#include "pdal/model/error.h"

namespace pdal {
namespace {

class VectorHistoryCursor final : public HistoryCursor {
 public:
  VectorHistoryCursor(std::vector<BackendRecord> records, std::uint64_t offset,
                      std::uint64_t max_records)
      : considered_(records.size()) {
    const auto begin = std::min<std::uint64_t>(offset, records.size());
    const auto end = std::min<std::uint64_t>(
        records.size(), begin + max_records + 1);
    has_more_ = end > begin + max_records;
    for (std::uint64_t index = begin;
         index < std::min<std::uint64_t>(records.size(), begin + max_records);
         ++index) {
      page_.push_back(std::move(records[index]));
    }
  }

  std::optional<BackendRecord> Next() override {
    if (position_ >= page_.size()) return std::nullopt;
    return std::move(page_[position_++]);
  }
  bool has_more() const override { return has_more_; }
  std::uint64_t records_considered() const override { return considered_; }

 private:
  std::vector<BackendRecord> page_;
  std::size_t position_ = 0;
  bool has_more_ = false;
  std::uint64_t considered_ = 0;
};

}  // namespace

std::unique_ptr<HistoryCursor> StorageBackend::OpenHistory(
    const HistoryCursorRequest& request, const ResourceCatalog& catalog) const {
  return std::make_unique<VectorHistoryCursor>(Query(request.task, catalog),
                                                request.offset,
                                                request.max_records);
}

std::shared_ptr<const std::vector<std::uint8_t>> StorageBackend::ReadPayload(
    const BackendRecord& record) const {
  auto payload = std::make_shared<std::vector<std::uint8_t>>();
  payload->reserve(record.payload_size);
  const auto read = Read(record, [&](const std::uint8_t* data, std::size_t size) {
    payload->insert(payload->end(), data, data + size);
    return true;
  });
  if (read != record.payload_size || payload->size() != record.payload_size) {
    throw PdalError(ErrorClass::kPartialRead,
                    "historical backend returned an incomplete record");
  }
  return payload;
}

void BackendRegistry::Register(std::shared_ptr<StorageBackend> backend) {
  if (!backend) throw std::invalid_argument("cannot register null backend");
  const auto id = backend->GetCapabilities().backend_id;
  if (id.empty() || !backends_.emplace(id, std::move(backend)).second) {
    throw std::invalid_argument("duplicate or empty backend id: " + id);
  }
}

const StorageBackend& BackendRegistry::Get(const std::string& backend_id) const {
  const auto found = backends_.find(backend_id);
  if (found == backends_.end()) {
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "configured historical data source is unavailable");
  }
  return *found->second;
}

std::vector<BackendCapabilities> BackendRegistry::Capabilities() const {
  std::vector<BackendCapabilities> out;
  out.reserve(backends_.size());
  for (const auto& [_, backend] : backends_) {
    out.push_back(backend->GetCapabilities());
  }
  return out;
}

}  // namespace pdal
