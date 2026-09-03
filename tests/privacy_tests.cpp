#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pdal/privacy/human_blur.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: "         \
                << #condition << '\n';                                      \
      ++failures;                                                           \
    }                                                                       \
  } while (false)

std::vector<std::uint8_t> ReadFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Read width/height from a baseline JPEG's SOF0/SOF2 marker.
std::optional<std::pair<int, int>> JpegDimensions(
    const std::vector<std::uint8_t>& data) {
  if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8) return std::nullopt;
  std::size_t i = 2;
  while (i + 9 < data.size()) {
    if (data[i] != 0xFF) {
      ++i;
      continue;
    }
    const std::uint8_t marker = data[i + 1];
    const std::size_t length =
        (static_cast<std::size_t>(data[i + 2]) << 8) | data[i + 3];
    if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
      const int height = (data[i + 5] << 8) | data[i + 6];
      const int width = (data[i + 7] << 8) | data[i + 8];
      return std::make_pair(width, height);
    }
    if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
      i += 2;
    } else {
      i += 2 + length;
    }
  }
  return std::nullopt;
}

template <typename Callable>
bool Throws(Callable&& callable) {
  try {
    callable();
    return false;
  } catch (const std::exception&) {
    return true;
  }
}

const std::string kModel = "models/yolov8n.onnx";
const std::string kPeople = "tests/fixtures/street_two_people.jpg";
const std::string kBlank = "tests/fixtures/blank_scene.jpg";

void TestFailClosedStageAlwaysThrows() {
  auto stage = pdal::MakeFailClosedStage("test");
  const std::vector<std::uint8_t> anything{1, 2, 3};
  CHECK(Throws([&] { stage->AnonymizeJpeg(anything.data(), anything.size()); }));
}

void TestBadModelPathThrows() {
  pdal::HumanBlurConfig config;
  config.model_path = "does/not/exist.onnx";
  CHECK(Throws([&] { (void)pdal::MakeHumanBlur(config); }));
}

void TestBlursPeopleAndKeepsDimensions() {
  pdal::HumanBlurConfig config;
  config.model_path = kModel;
  auto stage = pdal::MakeHumanBlur(config);

  const auto original = ReadFile(kPeople);
  CHECK(!original.empty());
  const auto in_dims = JpegDimensions(original);
  CHECK(in_dims.has_value());

  const auto result = stage->AnonymizeJpeg(original.data(), original.size());
  CHECK(result.jpeg.size() >= 3);
  CHECK(result.jpeg[0] == 0xFF && result.jpeg[1] == 0xD8 && result.jpeg[2] == 0xFF);
  CHECK(result.regions_blurred >= 1);
  CHECK(result.width == static_cast<std::uint32_t>(in_dims->first));
  CHECK(result.height == static_cast<std::uint32_t>(in_dims->second));

  const auto out_dims = JpegDimensions(result.jpeg);
  CHECK(out_dims.has_value());
  CHECK(out_dims->first == in_dims->first);
  CHECK(out_dims->second == in_dims->second);
}

void TestNoPeopleLeavesRegionCountZero() {
  pdal::HumanBlurConfig config;
  config.model_path = kModel;
  auto stage = pdal::MakeHumanBlur(config);

  const auto original = ReadFile(kBlank);
  CHECK(!original.empty());
  const auto result = stage->AnonymizeJpeg(original.data(), original.size());
  CHECK(result.regions_blurred == 0);
  CHECK(result.jpeg[0] == 0xFF && result.jpeg[1] == 0xD8);
  const auto in_dims = JpegDimensions(original);
  CHECK(result.width == static_cast<std::uint32_t>(in_dims->first));
  CHECK(result.height == static_cast<std::uint32_t>(in_dims->second));
}

void TestFailsClosedOnBadInput() {
  pdal::HumanBlurConfig config;
  config.model_path = kModel;
  auto stage = pdal::MakeHumanBlur(config);

  const std::vector<std::uint8_t> garbage(4096, 0x5A);
  CHECK(Throws([&] { stage->AnonymizeJpeg(garbage.data(), garbage.size()); }));
  CHECK(Throws([&] { stage->AnonymizeJpeg(nullptr, 0); }));

  // A JPEG start-of-image followed by noise is undecodable -> fail closed.
  std::vector<std::uint8_t> corrupt{0xFF, 0xD8, 0xFF, 0xE0};
  corrupt.resize(8192, 0x37);
  CHECK(Throws([&] { stage->AnonymizeJpeg(corrupt.data(), corrupt.size()); }));
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"fail-closed stage always throws", TestFailClosedStageAlwaysThrows},
      {"bad model path throws", TestBadModelPathThrows},
      {"blurs people and keeps dimensions", TestBlursPeopleAndKeepsDimensions},
      {"no people leaves region count zero", TestNoPeopleLeavesRegionCountZero},
      {"fails closed on bad input", TestFailsClosedOnBadInput},
  };
  for (const auto& [name, test] : tests) {
    try {
      test();
      if (failures == 0) std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
      ++failures;
    }
  }
  if (failures != 0) {
    std::cerr << failures << " privacy test assertion(s) failed\n";
    return 1;
  }
  std::cout << "all pDAL privacy tests passed\n";
  return 0;
}
