#include "pdal/storage/storage_backend.h"

#include <utility>

#include "pdal/model/error.h"

namespace pdal {

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
                    "configured storage backend is unavailable",
                    {{"backend", backend_id}});
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
