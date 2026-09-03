#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pdal {

struct AnonymizationResult {
  std::vector<std::uint8_t> jpeg;   // re-encoded frame, every human blurred
  std::uint64_t regions_blurred = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

// Pixel-domain transformer for camera frames. It sits after the payload read
// and before the frame is handed to the caller. Implementations MUST fail
// closed: any decode, detection, or re-encode failure throws, and the caller
// then emits no bytes for that record.
//
// This is deliberately NOT `PrivacyHook` (which only narrows a DataQuery).
class PrivacyStage {
 public:
  virtual ~PrivacyStage() = default;

  // Decode one JPEG, blur every detected person, re-encode as JPEG.
  virtual AnonymizationResult AnonymizeJpeg(const std::uint8_t* data,
                                            std::size_t size) const = 0;
};

}  // namespace pdal
