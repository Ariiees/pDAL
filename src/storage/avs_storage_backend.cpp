#include "pdal/storage/avs_storage_backend.h"

#include <avs/retrieve_api.h>

#include <memory>
#include <utility>

#include "pdal/model/error.h"

namespace pdal {
namespace {

class AvsRecordLocator final : public RecordLocator {
 public:
  explicit AvsRecordLocator(avs::DataRef value) : ref(std::move(value)) {}
  avs::DataRef ref;
};

}  // namespace

class AvsStorageBackend::Impl {
 public:
  explicit Impl(std::filesystem::path root)
      : ssd_root(std::move(root)), retrieve(ssd_root) {}
  std::filesystem::path ssd_root;
  avs::RetrieveAPI retrieve;
};

AvsStorageBackend::AvsStorageBackend(std::filesystem::path ssd_root)
    : impl_(std::make_unique<Impl>(std::move(ssd_root))) {}

AvsStorageBackend::~AvsStorageBackend() = default;

BackendCapabilities AvsStorageBackend::GetCapabilities() const {
  return {"avs", true, false, true, 0};
}

void AvsStorageBackend::Resolve(const ResourceDescriptor& resource) const {
  if (resource.binding.backend_id != "avs" || resource.binding.source.empty()) {
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "resource cannot be resolved by the AVS backend",
                    {{"resource_id", resource.resource_id}});
  }
}

std::vector<BackendRecord> AvsStorageBackend::Query(
    const ExecutionTask& task, const ResourceCatalog& catalog) const {
  const auto& resource = catalog.Get(task.resource_id);
  Resolve(resource);
  if (!std::filesystem::exists(impl_->ssd_root / "global.sqlite3")) {
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "AVS hot-tier catalog is unavailable",
                    {{"resource_id", task.resource_id}, {"backend", "avs"}});
  }
  std::string error;
  auto refs = impl_->retrieve.QueryRefs(resource.binding.source, task.time.start_ns,
                                        task.time.end_ns, &error);
  if (refs.empty() && !error.empty() && error != "no matching trips") {
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "AVS hot-tier query failed",
                    {{"resource_id", task.resource_id}, {"backend", "avs"}});
  }
  std::vector<BackendRecord> records;
  records.reserve(refs.size());
  for (auto& ref : refs) {
    records.push_back({task.resource_id, "avs", ref.ts_ns, ref.payload_size, "hot",
                       std::make_shared<AvsRecordLocator>(std::move(ref))});
  }
  return records;
}

std::uint64_t AvsStorageBackend::Read(const BackendRecord& record,
                                      const ByteSink& sink) const {
  const auto locator = std::dynamic_pointer_cast<const AvsRecordLocator>(record.locator);
  if (!locator) {
    throw PdalError(ErrorClass::kInternalError,
                    "record locator does not belong to the AVS backend");
  }
  std::vector<std::uint8_t> payload;
  std::string error;
  if (!impl_->retrieve.LoadPayload(locator->ref, payload, &error)) {
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "AVS payload read failed",
                    {{"resource_id", record.resource_id},
                     {"timestamp_ns", record.timestamp_ns}});
  }
  if (!payload.empty() && !sink(payload.data(), payload.size())) {
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "payload consumer disconnected");
  }
  return payload.size();
}

}  // namespace pdal
