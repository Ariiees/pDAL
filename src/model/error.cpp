#include "pdal/model/error.h"

#include <utility>

namespace pdal {

PdalError::PdalError(ErrorClass error_class, std::string message,
                     boost::json::object details)
    : std::runtime_error(std::move(message)),
      error_class_(error_class),
      details_(std::move(details)) {}

std::string ErrorClassName(ErrorClass error_class) {
  switch (error_class) {
    case ErrorClass::kInvalidRequest: return "invalid_request";
    case ErrorClass::kUnauthenticated: return "unauthenticated";
    case ErrorClass::kForbidden: return "forbidden";
    case ErrorClass::kResourceNotFound: return "resource_not_found";
    case ErrorClass::kRepresentationNotSupported: return "representation_not_supported";
    case ErrorClass::kRangeNotSatisfiable: return "range_not_satisfiable";
    case ErrorClass::kBackendUnavailable: return "backend_unavailable";
    case ErrorClass::kInternalError: return "internal_error";
  }
  return "internal_error";
}

std::string ErrorCode(ErrorClass error_class) {
  switch (error_class) {
    case ErrorClass::kInvalidRequest: return "PDAL_INVALID_REQUEST";
    case ErrorClass::kUnauthenticated: return "PDAL_UNAUTHENTICATED";
    case ErrorClass::kForbidden: return "PDAL_FORBIDDEN";
    case ErrorClass::kResourceNotFound: return "PDAL_RESOURCE_NOT_FOUND";
    case ErrorClass::kRepresentationNotSupported:
      return "PDAL_REPRESENTATION_NOT_SUPPORTED";
    case ErrorClass::kRangeNotSatisfiable: return "PDAL_RANGE_NOT_SATISFIABLE";
    case ErrorClass::kBackendUnavailable: return "PDAL_BACKEND_UNAVAILABLE";
    case ErrorClass::kInternalError: return "PDAL_INTERNAL_ERROR";
  }
  return "PDAL_INTERNAL_ERROR";
}

int HttpStatus(ErrorClass error_class) {
  switch (error_class) {
    case ErrorClass::kInvalidRequest: return 400;
    case ErrorClass::kUnauthenticated: return 401;
    case ErrorClass::kForbidden: return 403;
    case ErrorClass::kResourceNotFound: return 404;
    case ErrorClass::kRepresentationNotSupported: return 406;
    case ErrorClass::kRangeNotSatisfiable: return 416;
    case ErrorClass::kBackendUnavailable: return 503;
    case ErrorClass::kInternalError: return 500;
  }
  return 500;
}

boost::json::object ErrorEnvelope(const PdalError& error,
                                  const std::string& request_id) {
  boost::json::object body;
  body["code"] = ErrorCode(error.error_class());
  body["class"] = ErrorClassName(error.error_class());
  body["message"] = error.what();
  body["request_id"] = request_id;
  body["details"] = error.details();
  return {{"error", std::move(body)}};
}

}  // namespace pdal
