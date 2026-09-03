#pragma once

#include <memory>
#include <string>

#include "pdal/privacy/privacy_stage.h"

namespace pdal {

struct HumanBlurConfig {
  std::string model_path;          // YOLOv8n ONNX (COCO classes)
  float score_threshold = 0.25f;
  float nms_threshold = 0.45f;
  int input_size = 640;
  int person_class_id = 0;         // COCO 'person'
  int blur_kernel_divisor = 3;     // kernel = max(15, box_width / divisor), forced odd
  double blur_sigma = 30.0;
  int jpeg_quality = 90;
  int intra_op_threads = 2;        // ORT CPU threads per inference
};

// YOLOv8n person detector + Gaussian blur, run in-process on the CPU through
// OpenCV's DNN module. No network calls, no files written, no pixels logged.
// Construction loads the model and throws std::runtime_error on failure.
// Available only when the build defines PDAL_WITH_PRIVACY.
std::unique_ptr<PrivacyStage> MakeHumanBlur(const HumanBlurConfig& config);

// A stage that always throws. Used when the privacy build is disabled or a
// model is unavailable, so a camera request can never return raw pixels.
std::unique_ptr<PrivacyStage> MakeFailClosedStage(std::string reason);

}  // namespace pdal
