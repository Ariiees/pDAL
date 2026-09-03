#include "pdal/privacy/human_blur.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace pdal {
namespace {

struct Letterbox {
  cv::Mat image;   // side x side, BGR
  float scale = 1.0F;
  int pad_x = 0;
  int pad_y = 0;
};

Letterbox MakeLetterbox(const cv::Mat& src, int side) {
  Letterbox lb;
  lb.scale = std::min(static_cast<float>(side) / static_cast<float>(src.cols),
                      static_cast<float>(side) / static_cast<float>(src.rows));
  const int rw = std::max(1, static_cast<int>(std::round(src.cols * lb.scale)));
  const int rh = std::max(1, static_cast<int>(std::round(src.rows * lb.scale)));
  lb.pad_x = (side - rw) / 2;
  lb.pad_y = (side - rh) / 2;
  cv::Mat resized;
  cv::resize(src, resized, cv::Size(rw, rh), 0, 0, cv::INTER_LINEAR);
  lb.image.create(side, side, src.type());
  lb.image.setTo(cv::Scalar(114, 114, 114));
  resized.copyTo(lb.image(cv::Rect(lb.pad_x, lb.pad_y, rw, rh)));
  return lb;
}

// Minimal IoU non-max suppression on person boxes.
std::vector<int> Nms(const std::vector<cv::Rect>& boxes,
                     const std::vector<float>& scores, float iou_threshold) {
  std::vector<int> order(boxes.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
  std::sort(order.begin(), order.end(),
            [&](int a, int b) { return scores[a] > scores[b]; });
  std::vector<int> keep;
  std::vector<char> removed(boxes.size(), 0);
  for (std::size_t i = 0; i < order.size(); ++i) {
    const int a = order[i];
    if (removed[a]) continue;
    keep.push_back(a);
    for (std::size_t j = i + 1; j < order.size(); ++j) {
      const int b = order[j];
      if (removed[b]) continue;
      const int inter = (boxes[a] & boxes[b]).area();
      const int uni = boxes[a].area() + boxes[b].area() - inter;
      if (uni > 0 && static_cast<float>(inter) / static_cast<float>(uni) > iou_threshold) {
        removed[b] = 1;
      }
    }
  }
  return keep;
}

class OnnxHumanBlur final : public PrivacyStage {
 public:
  explicit OnnxHumanBlur(HumanBlurConfig config)
      : config_(std::move(config)),
        env_(ORT_LOGGING_LEVEL_WARNING, "pdal-privacy") {
    if (config_.model_path.empty()) {
      throw std::runtime_error("human blur: model_path is empty");
    }
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(std::max(1, config_.intra_op_threads));
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    try {
      session_ = std::make_unique<Ort::Session>(env_, config_.model_path.c_str(),
                                                options);
    } catch (const Ort::Exception& error) {
      throw std::runtime_error(std::string("human blur: cannot load model: ") +
                               error.what());
    }
    Ort::AllocatorWithDefaultOptions allocator;
    input_name_ = session_->GetInputNameAllocated(0, allocator).get();
    output_name_ = session_->GetOutputNameAllocated(0, allocator).get();
  }

  AnonymizationResult AnonymizeJpeg(const std::uint8_t* data,
                                    std::size_t size) const override {
    if (data == nullptr || size == 0) {
      throw std::runtime_error("human blur: empty frame");
    }
    const cv::Mat encoded(1, static_cast<int>(size), CV_8U,
                          const_cast<std::uint8_t*>(data));
    cv::Mat frame = cv::imdecode(encoded, cv::IMREAD_COLOR);
    if (frame.empty()) {
      throw std::runtime_error("human blur: JPEG decode failed");
    }

    const auto boxes = DetectPersons(frame);
    for (const auto& box : boxes) BlurRegion(frame, box);

    const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, config_.jpeg_quality};
    std::vector<std::uint8_t> out;
    if (!cv::imencode(".jpg", frame, out, params) || out.empty()) {
      throw std::runtime_error("human blur: JPEG re-encode failed");
    }

    AnonymizationResult result;
    result.jpeg = std::move(out);
    result.regions_blurred = boxes.size();
    result.width = static_cast<std::uint32_t>(frame.cols);
    result.height = static_cast<std::uint32_t>(frame.rows);
    return result;
  }

 private:
  std::vector<cv::Rect> DetectPersons(const cv::Mat& frame) const {
    const int side = config_.input_size;
    const Letterbox lb = MakeLetterbox(frame, side);

    cv::Mat rgb;
    cv::cvtColor(lb.image, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(rgb, CV_32F, 1.0 / 255.0);
    std::array<cv::Mat, 3> channels;
    cv::split(rgb, channels.data());

    const std::size_t plane = static_cast<std::size_t>(side) * side;
    std::vector<float> input(plane * 3);
    for (int c = 0; c < 3; ++c) {
      std::memcpy(input.data() + c * plane, channels[c].ptr<float>(),
                  plane * sizeof(float));
    }

    const std::array<std::int64_t, 4> shape{1, 3, side, side};
    const auto memory =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    std::vector<Ort::Value> outputs;
    {
      // Ort::Session::Run is thread-safe, but keep detection serial so at most
      // one camera frame is in flight, matching the buffering guarantee.
      std::lock_guard<std::mutex> lock(mutex_);
      Ort::Value tensor = Ort::Value::CreateTensor<float>(
          memory, input.data(), input.size(), shape.data(), shape.size());
      const char* input_names[] = {input_name_.c_str()};
      const char* output_names[] = {output_name_.c_str()};
      outputs = session_->Run(Ort::RunOptions{nullptr}, input_names, &tensor, 1,
                              output_names, 1);
    }

    const auto info = outputs[0].GetTensorTypeAndShapeInfo();
    const auto dims = info.GetShape();
    if (dims.size() != 3 || dims[0] != 1) {
      throw std::runtime_error("human blur: unexpected model output rank");
    }
    const int attrs = static_cast<int>(dims[1]);
    const int anchors = static_cast<int>(dims[2]);
    if (attrs <= 4 || config_.person_class_id >= attrs - 4) {
      throw std::runtime_error("human blur: unexpected model output shape");
    }
    const float* out = outputs[0].GetTensorData<float>();
    // Layout is [1, attrs, anchors]; row r, anchor a is out[r * anchors + a].
    const int person_row = 4 + config_.person_class_id;

    std::vector<cv::Rect> boxes;
    std::vector<float> scores;
    const cv::Rect bounds(0, 0, frame.cols, frame.rows);
    for (int a = 0; a < anchors; ++a) {
      const float score = out[person_row * anchors + a];
      if (score < config_.score_threshold) continue;
      float best = 0.0F;
      for (int r = 4; r < attrs; ++r) best = std::max(best, out[r * anchors + a]);
      if (score < best) continue;  // person must be the dominant class

      const float cx = out[0 * anchors + a];
      const float cy = out[1 * anchors + a];
      const float w = out[2 * anchors + a];
      const float h = out[3 * anchors + a];
      const int x0 = cvRound((cx - w / 2.0F - lb.pad_x) / lb.scale);
      const int y0 = cvRound((cy - h / 2.0F - lb.pad_y) / lb.scale);
      const cv::Rect box = cv::Rect(x0, y0, cvRound(w / lb.scale),
                                    cvRound(h / lb.scale)) & bounds;
      if (box.width > 1 && box.height > 1) {
        boxes.push_back(box);
        scores.push_back(score);
      }
    }

    std::vector<cv::Rect> result;
    for (const int index : Nms(boxes, scores, config_.nms_threshold)) {
      result.push_back(boxes[index]);
    }
    return result;
  }

  void BlurRegion(cv::Mat& frame, const cv::Rect& box) const {
    cv::Mat roi = frame(box);
    int kernel =
        std::max(15, box.width / std::max(1, config_.blur_kernel_divisor));
    if (kernel % 2 == 0) ++kernel;
    cv::GaussianBlur(roi, roi, cv::Size(kernel, kernel), config_.blur_sigma);
  }

  HumanBlurConfig config_;
  Ort::Env env_;
  std::unique_ptr<Ort::Session> session_;
  std::string input_name_;
  std::string output_name_;
  mutable std::mutex mutex_;
};

}  // namespace

std::unique_ptr<PrivacyStage> MakeHumanBlur(const HumanBlurConfig& config) {
  return std::make_unique<OnnxHumanBlur>(config);
}

}  // namespace pdal
