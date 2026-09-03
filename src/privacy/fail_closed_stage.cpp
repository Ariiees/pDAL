#include <stdexcept>
#include <utility>

#include "pdal/privacy/human_blur.h"

namespace pdal {
namespace {

class FailClosedStage final : public PrivacyStage {
 public:
  explicit FailClosedStage(std::string reason) : reason_(std::move(reason)) {}

  AnonymizationResult AnonymizeJpeg(const std::uint8_t*,
                                    std::size_t) const override {
    throw std::runtime_error("camera privacy stage unavailable: " + reason_);
  }

 private:
  std::string reason_;
};

}  // namespace

std::unique_ptr<PrivacyStage> MakeFailClosedStage(std::string reason) {
  return std::make_unique<FailClosedStage>(std::move(reason));
}

}  // namespace pdal
