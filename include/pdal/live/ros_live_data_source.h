#pragma once

#include <cstddef>
#include <memory>

#include "pdal/live/live_data_source.h"

namespace pdal {

class RosLiveDataSource final : public ILiveDataSource {
 public:
  RosLiveDataSource(const ResourceCatalog& catalog,
                    std::size_t max_subscriptions = 8,
                    std::size_t default_queue_bytes = 8 * 1024 * 1024);
  ~RosLiveDataSource() override;

  LiveSourceCapabilities GetCapabilities() const override;
  std::optional<DataSample> Latest(
      const ResourceDescriptor& resource,
      const RepresentationRequest& representation) const override;
  DataStream Subscribe(
      const ResourceDescriptor& resource,
      const RepresentationRequest& representation,
      const QueryOptions& options) const override;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace pdal
