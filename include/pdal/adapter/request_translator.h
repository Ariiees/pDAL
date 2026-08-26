#pragma once

#include <boost/json/value.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pdal/model/types.h"
#include "pdal/model/data.h"

namespace pdal {

using ExternalHeaders = std::unordered_map<std::string, std::string>;

class RequestTranslator {
 public:
  virtual ~RequestTranslator() = default;
  virtual PdalRequest ToCanonicalRequest(
      const boost::json::value& external,
      const ExternalHeaders& headers = {}) const = 0;
};

class NativeHttpAdapter final : public RequestTranslator {
 public:
  PdalRequest ToCanonicalRequest(
      const boost::json::value& external,
      const ExternalHeaders& headers = {}) const override;
  DataQuery ToDataQuery(const boost::json::value& external,
                        Operation operation,
                        const ExternalHeaders& headers = {}) const;
};

class SovdAdapter final : public RequestTranslator {
 public:
  PdalRequest ToCanonicalRequest(
      const boost::json::value& external,
      const ExternalHeaders& headers = {}) const override;
  DataQuery ToDataQuery(const boost::json::value& external,
                        const ExternalHeaders& headers = {}) const;
};

class LocalCliAdapter {
 public:
  PdalRequest ToCanonicalRequest(const std::vector<std::string>& arguments) const;
  DataQuery ToDataQuery(const std::vector<std::string>& arguments,
                        Operation operation = Operation::kHistory) const;
};

void ValidateRequest(const PdalRequest& request);
bool SemanticallyEquivalent(const PdalRequest& left,
                            const PdalRequest& right);

}  // namespace pdal
