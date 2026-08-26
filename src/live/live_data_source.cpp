#include "pdal/live/live_data_source.h"

#include <stdexcept>
#include <utility>

#include "pdal/model/error.h"

namespace pdal {

void LiveSourceRegistry::Register(std::shared_ptr<ILiveDataSource> source) {
  if (!source) throw std::invalid_argument("cannot register null live source");
  const auto id = source->GetCapabilities().source_id;
  if (id.empty() || !sources_.emplace(id, std::move(source)).second) {
    throw std::invalid_argument("duplicate or empty live source id: " + id);
  }
}

const ILiveDataSource& LiveSourceRegistry::Get(
    const std::string& source_id) const {
  const auto found = sources_.find(source_id);
  if (found == sources_.end()) {
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "configured live source is unavailable");
  }
  return *found->second;
}

std::vector<LiveSourceCapabilities> LiveSourceRegistry::Capabilities() const {
  std::vector<LiveSourceCapabilities> result;
  result.reserve(sources_.size());
  for (const auto& [_, source] : sources_) {
    result.push_back(source->GetCapabilities());
  }
  return result;
}

}  // namespace pdal
