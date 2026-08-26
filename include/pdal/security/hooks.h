#pragma once

#include <memory>

#include "pdal/catalog/resource_catalog.h"
#include "pdal/model/data.h"

namespace pdal {

class PolicyHook {
 public:
  virtual ~PolicyHook() = default;
  virtual DataQuery Apply(const DataQuery& query,
                          const ResourceDescriptor& resource) const = 0;
};

class PrivacyHook {
 public:
  virtual ~PrivacyHook() = default;
  virtual DataQuery Apply(const DataQuery& query,
                          const ResourceDescriptor& resource) const = 0;
};

class PassThroughPolicy final : public PolicyHook {
 public:
  DataQuery Apply(const DataQuery& query,
                  const ResourceDescriptor&) const override {
    return query;
  }
};

class NoOpPrivacy final : public PrivacyHook {
 public:
  DataQuery Apply(const DataQuery& query,
                  const ResourceDescriptor&) const override {
    return query;
  }
};

}  // namespace pdal
