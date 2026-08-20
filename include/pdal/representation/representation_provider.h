#pragma once

#include <string>

#include "pdal/model/types.h"

namespace pdal {

struct NegotiatedRepresentation {
  std::string format;
  std::string content_type;
  bool include_payload = true;
};

class RepresentationProvider {
 public:
  NegotiatedRepresentation Negotiate(
      const ResourceDescriptor& resource,
      const RepresentationRequest& request) const;
};

}  // namespace pdal
