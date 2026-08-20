#include "pdal/representation/representation_provider.h"

#include "pdal/model/error.h"

namespace pdal {

NegotiatedRepresentation RepresentationProvider::Negotiate(
    const ResourceDescriptor& resource,
    const RepresentationRequest& request) const {
  if (!request.transformations.empty()) {
    throw PdalError(ErrorClass::kRepresentationNotSupported,
                    "requested transformations are not implemented in phase 1",
                    {{"resource_id", resource.resource_id}});
  }
  if (request.quality) {
    throw PdalError(ErrorClass::kRepresentationNotSupported,
                    "quality transformation is not implemented in phase 1",
                    {{"resource_id", resource.resource_id}});
  }
  if (request.format == "metadata" || request.format == "metadata-only") {
    return {"metadata", "application/json", false};
  }
  if (request.format == "native") {
    for (const auto& representation : resource.representations) {
      if (representation.stored) {
        return {representation.format, representation.content_type, true};
      }
    }
  } else {
    for (const auto& representation : resource.representations) {
      if (representation.format == request.format && representation.stored) {
        return {representation.format, representation.content_type, true};
      }
    }
  }
  throw PdalError(ErrorClass::kRepresentationNotSupported,
                  "representation is not available for logical resource",
                  {{"resource_id", resource.resource_id},
                   {"format", request.format}});
}

}  // namespace pdal
