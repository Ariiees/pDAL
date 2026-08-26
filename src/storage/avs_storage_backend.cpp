#include "pdal/storage/avs_storage_backend.h"

#include <avs/historical_retrieve_api.h>

#include <memory>
#include <utility>

#include "pdal/model/error.h"

namespace pdal {
namespace {

class AvsRecordLocator final : public RecordLocator {
 public:
  explicit AvsRecordLocator(avs::HistoricalDataRef value) : ref(std::move(value)) {}
  avs::HistoricalDataRef ref;
};

class AvsHistoryCursor final : public HistoryCursor {
 public:
  AvsHistoryCursor(std::vector<BackendRecord> records, bool has_more,
                   std::uint64_t considered)
      : records_(std::move(records)), has_more_(has_more), considered_(considered) {}
  std::optional<BackendRecord> Next() override {
    if (position_ >= records_.size()) return std::nullopt;
    return std::move(records_[position_++]);
  }
  bool has_more() const override { return has_more_; }
  std::uint64_t records_considered() const override { return considered_; }

 private:
  std::vector<BackendRecord> records_;
  std::size_t position_ = 0;
  bool has_more_ = false;
  std::uint64_t considered_ = 0;
};

}  // namespace

class AvsStorageBackend::Impl {
 public:
  Impl(std::filesystem::path hot_root, std::filesystem::path cold_root)
      : ssd_root(std::move(hot_root)),
        hdd_root(std::move(cold_root)),
        retrieve(ssd_root, hdd_root) {}
  std::filesystem::path ssd_root;
  std::filesystem::path hdd_root;
  avs::HistoricalRetrieveAPI retrieve;
};

AvsStorageBackend::AvsStorageBackend(std::filesystem::path ssd_root,
                                     std::filesystem::path hdd_root)
    : impl_(std::make_unique<Impl>(std::move(ssd_root), std::move(hdd_root))) {}

AvsStorageBackend::~AvsStorageBackend() = default;

BackendCapabilities AvsStorageBackend::GetCapabilities() const {
  return {"avs", true, true, true, 100000};
}

void AvsStorageBackend::Resolve(const CatalogResource& resource) const {
  if (resource.historical_binding.backend_id != "avs" ||
      resource.historical_binding.source.empty()) {
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "resource cannot be resolved by the historical data source",
                    {{"resource_id", resource.resource_id}});
  }
}

std::vector<BackendRecord> AvsStorageBackend::Query(
    const ExecutionTask& task, const ResourceCatalog& catalog) const {
  auto cursor = OpenHistory({task, 0, 100000}, catalog);
  std::vector<BackendRecord> records;
  while (auto record = cursor->Next()) records.push_back(std::move(*record));
  return records;
}

std::unique_ptr<HistoryCursor> AvsStorageBackend::OpenHistory(
    const HistoryCursorRequest& request,
    const ResourceCatalog& catalog) const {
  const auto& task = request.task;
  const auto& resource = catalog.Get(task.resource_id);
  Resolve(resource);
  if (!std::filesystem::exists(impl_->ssd_root / "global.sqlite3") &&
      !std::filesystem::exists(impl_->hdd_root / "global.sqlite3")) {
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "historical catalogs are unavailable",
                    {{"resource_id", task.resource_id}});
  }
  const auto folder = resource.historical_binding.options.find("topic_folder");
  if (folder == resource.historical_binding.options.end()) {
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "historical resource mapping is incomplete");
  }
  std::string error;
  auto page = impl_->retrieve.QueryRefs(
      resource.historical_binding.source, folder->second, task.time.start_ns,
      task.time.end_ns, request.offset, request.max_records, &error);
  if (page.refs.empty() && !error.empty()) {
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "historical query failed",
                    {{"resource_id", task.resource_id}});
  }
  std::vector<BackendRecord> records;
  records.reserve(page.refs.size());
  for (auto& ref : page.refs) {
    const auto tier = ref.tier == avs::HistoricalStorageTier::kHot ? "hot" : "cold";
    records.push_back({task.resource_id, "avs", ref.ts_ns, ref.payload_size, tier,
                       std::make_shared<AvsRecordLocator>(std::move(ref))});
  }
  return std::make_unique<AvsHistoryCursor>(
      std::move(records), page.has_more, page.records_considered);
}

std::uint64_t AvsStorageBackend::Read(const BackendRecord& record,
                                      const ByteSink& sink) const {
  const auto payload = ReadPayload(record);
  if (!payload->empty() && !sink(payload->data(), payload->size())) {
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "payload consumer disconnected");
  }
  return payload->size();
}

std::shared_ptr<const std::vector<std::uint8_t>>
AvsStorageBackend::ReadPayload(const BackendRecord& record) const {
  const auto locator = std::dynamic_pointer_cast<const AvsRecordLocator>(record.locator);
  if (!locator) {
    throw PdalError(ErrorClass::kInternalError,
                    "historical record locator is invalid");
  }
  auto payload = std::make_shared<std::vector<std::uint8_t>>();
  std::string error;
  if (!impl_->retrieve.LoadPayload(locator->ref, *payload, &error)) {
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "historical payload read failed",
                    {{"resource_id", record.resource_id},
                     {"timestamp_ns", record.timestamp_ns}});
  }
  if (payload->size() != record.payload_size) {
    throw PdalError(ErrorClass::kPartialRead,
                    "data source returned an incomplete historical record",
                    {{"resource_id", record.resource_id},
                     {"timestamp_ns", record.timestamp_ns}});
  }
  return payload;
}

}  // namespace pdal
