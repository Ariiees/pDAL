#pragma once

#include <boost/json/object.hpp>

#include <stdexcept>
#include <string>

namespace pdal {

enum class ErrorClass {
  kInvalidRequest,
  kInvalidQuery,
  kUnauthenticated,
  kForbidden,
  kResourceNotFound,
  kRepresentationNotSupported,
  kRangeNotSatisfiable,
  kNoData,
  kNotSupported,
  kQueryTooLarge,
  kPartialRead,
  kBackendUnavailable,
  kInternalError,
};

class PdalError : public std::runtime_error {
 public:
  PdalError(ErrorClass error_class, std::string message,
            boost::json::object details = {});

  ErrorClass error_class() const { return error_class_; }
  const boost::json::object& details() const { return details_; }

 private:
  ErrorClass error_class_;
  boost::json::object details_;
};

std::string ErrorCode(ErrorClass error_class);
std::string ErrorClassName(ErrorClass error_class);
int HttpStatus(ErrorClass error_class);
boost::json::object ErrorEnvelope(const PdalError& error,
                                  const std::string& request_id);

}  // namespace pdal
