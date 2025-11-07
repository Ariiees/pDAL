// img_encrypt_node.cpp
// ROS2 node that subscribes to an image topic, performs deduplication,
// writes a JPEG for unique frames, encrypts the JPEG with AES GCM,
// stores key and metadata, deletes the plaintext JPEG, records a DB row,
// and prints per frame timing and throughput.

#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/image.hpp"

#include <yaml-cpp/yaml.h>
#include <cv_bridge/cv_bridge.hpp>

#include <filesystem>
#include <fstream>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <system_error>

#include "avs/img_dedup.h"
#include "avs/db_operation.h"
#include "common.h"
#include "pacc/AESGCMEncryptor.h"

using std::placeholders::_1;
namespace fs = std::filesystem;

namespace avs {

class ImgEncryptNode final : public rclcpp::Node {
public:
  ImgEncryptNode() : rclcpp::Node("img_encrypt_node") {
    // parameters
    this->declare_parameter<std::string>("config_path", "/home/avs/AVS-PI/src/avs/config/avs_config.yaml");
    const std::string config_path = this->get_parameter("config_path").as_string();

    YAML::Node root   = YAML::LoadFile(config_path);
    YAML::Node common = root["common"];
    YAML::Node dedup  = root["image_dedup"];

    image_topic_   = common["img_topic"].as<std::string>();
    img_root_dir_  = common["img_ssd_dir"].as<std::string>();
    db_dir_        = common["hot_db_dir"].as<std::string>();
    img_ext_       = dedup["img_format"].as<std::string>();

    if (common["enc_img_dir"]) {
      enc_root_dir_ = common["enc_img_dir"].as<std::string>();
    } else {
      enc_root_dir_ = img_root_dir_;
    }

    // day partitions
    img_day_dir_  = (fs::path(img_root_dir_) / avs::getCurrentDayFolder()).string();
    enc_day_dir_  = (fs::path(enc_root_dir_) / avs::getCurrentDayFolder()).string();
    meta_dir_     = (fs::path(enc_day_dir_) / "metadata").string();

    std::error_code ec;
    (void)avs::ensureDirectory(img_day_dir_,  &ec);
    (void)avs::ensureDirectory(enc_day_dir_,  &ec);
    (void)avs::ensureDirectory(meta_dir_,     &ec);

    // database
    fs::create_directories(db_dir_);
    const std::string db_path = (fs::path(db_dir_) / "avs_image.sqlite3").string();
    std::string dberr;
    if (!db_.open(db_path, &dberr)) {
      throw std::runtime_error("DB open failed: " + dberr);
    }

    // deduplicator
    dedup_ = std::make_shared<ImgDeduplicator>(img_day_dir_, config_path);

    // encryption
    encryptor_ = std::make_unique<avs::crypto::AESGCMEncryptor>();
    const std::string key_path = (fs::path(enc_day_dir_) / "encryption_key.bin").string();
    if (!encryptor_->saveKey(key_path)) {
      throw std::runtime_error("Failed to save AES key to " + key_path);
    }
    RCLCPP_INFO(this->get_logger(), "Saved AES key at %s", key_path.c_str());

    // subscription
    sub_ = this->create_subscription<sensor_msgs::msg::Image>(
      image_topic_, choose_qos(image_topic_), std::bind(&ImgEncryptNode::onImage, this, _1));

    RCLCPP_INFO(this->get_logger(),
                "Started. Topic %s, temp JPEG dir %s, encrypted dir %s, DB %s",
                image_topic_.c_str(), img_day_dir_.c_str(), enc_day_dir_.c_str(), db_path.c_str());
  }

private:
  rclcpp::QoS choose_qos(const std::string& topic) {
    using rclcpp::ReliabilityPolicy;
    rclcpp::QoS qos(10);
    qos.reliability(ReliabilityPolicy::Reliable);
    qos.durability(rclcpp::DurabilityPolicy::Volatile);

    for (int i = 0; i < 20; ++i) {
      auto infos = this->get_publishers_info_by_topic(topic);
      if (!infos.empty()) {
        auto offered = infos.front().qos_profile();
        qos.reliability(offered.reliability());
        return qos;
      }
      rclcpp::sleep_for(std::chrono::milliseconds(100));
    }
    RCLCPP_WARN(this->get_logger(), "No publisher QoS found, using default reliable volatile");
    return qos;
  }

  static bool writeText(const std::string& path, const std::string& s) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << s;
    return f.good();
  }

  void onImage(const sensor_msgs::msg::Image::SharedPtr msg) {
    const auto t0 = std::chrono::steady_clock::now();

    const long long ts_ms = static_cast<long long>(msg->header.stamp.sec) * 1000LL
                          + static_cast<long long>(msg->header.stamp.nanosec) / 1000000LL;

    const std::string stem      = std::to_string(ts_ms);
    const std::string plain_jpg = (fs::path(img_day_dir_) / (stem + "." + img_ext_)).string();
    const std::string enc_path  = (fs::path(enc_day_dir_) / (stem + ".enc")).string();
    const std::string meta_path = (fs::path(meta_dir_) / (stem + ".meta")).string();
    const std::string name_path = (fs::path(meta_dir_) / (stem + ".filename")).string();

    bool unique_flag = false;
    try {
      // deduplicator writes JPEG if unique
      unique_flag = dedup_->isUniqueAndStore(*msg, plain_jpg);
    } catch (const std::exception& e) {
      RCLCPP_ERROR(this->get_logger(), "Dedup error: %s", e.what());
      return;
    }

    if (!unique_flag) {
      const auto t1 = std::chrono::steady_clock::now();
      const double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      RCLCPP_INFO(this->get_logger(), "[DUP] skipped %lld total %.3f ms", ts_ms, total_ms);
      return;
    }

    // read JPEG bytes
    std::vector<unsigned char> jpeg_bytes;
    if (!avs::crypto::AESGCMEncryptor::readFile(plain_jpg, jpeg_bytes)) {
      RCLCPP_ERROR(this->get_logger(), "Failed to read JPEG %s", plain_jpg.c_str());
      return;
    }

    // encrypt
    std::vector<unsigned char> cipher, iv, tag;
    const auto te0 = std::chrono::steady_clock::now();
    if (!encryptor_->encrypt(jpeg_bytes, cipher, iv, tag)) {
      RCLCPP_ERROR(this->get_logger(), "Encrypt failed for %lld", ts_ms);
      return;
    }
    const auto te1 = std::chrono::steady_clock::now();

    // write encrypted payload and meta
    const auto meta = avs::crypto::AESGCMEncryptor::packMeta(iv, tag);
    if (!avs::crypto::AESGCMEncryptor::writeFile(enc_path, cipher)) {
      RCLCPP_ERROR(this->get_logger(), "Write enc failed %s", enc_path.c_str());
      return;
    }
    if (!avs::crypto::AESGCMEncryptor::writeFile(meta_path, meta)) {
      RCLCPP_ERROR(this->get_logger(), "Write meta failed %s", meta_path.c_str());
      return;
    }
    (void)writeText(name_path, stem + "." + img_ext_);

    // remove plaintext JPEG
    std::error_code ec;
    fs::remove(plain_jpg, ec);
    if (ec) {
      RCLCPP_WARN(this->get_logger(), "Could not remove %s ec=%d %s",
                  plain_jpg.c_str(), ec.value(), ec.message().c_str());
    }

    // DB insert
    AvsRow row;
    row.sensor_id = image_topic_;
    row.data_type = "enc";
    row.ts_ms     = ts_ms;
    row.path      = enc_path;

    std::string dberr;
    if (!db_.insertRow(row, &dberr)) {
      RCLCPP_ERROR(this->get_logger(), "DB insert failed: %s", dberr.c_str());
    }

    // timing and throughput
    const auto t1 = std::chrono::steady_clock::now();
    const double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double enc_ms   = std::chrono::duration<double, std::milli>(te1 - te0).count();
    const double mb_plain = static_cast<double>(jpeg_bytes.size()) / (1024.0 * 1024.0);
    const double thr_mb_s = (total_ms > 0.0) ? (mb_plain / (total_ms / 1000.0)) : 0.0;

    RCLCPP_INFO(this->get_logger(),
                "[UNIQUE] ts=%lld plain=%.2f MB enc=%.2f MB total=%.3f ms enc=%.3f ms thr=%.2f MBps",
                ts_ms,
                mb_plain,
                static_cast<double>(cipher.size()) / (1024.0 * 1024.0),
                total_ms,
                enc_ms,
                thr_mb_s);
  }

private:
  // config
  std::string image_topic_;
  std::string img_root_dir_;
  std::string enc_root_dir_;
  std::string db_dir_;
  std::string img_ext_;

  // resolved paths
  std::string img_day_dir_;
  std::string enc_day_dir_;
  std::string meta_dir_;

  // modules
  std::shared_ptr<ImgDeduplicator> dedup_;
  AvsDb db_;
  std::unique_ptr<avs::crypto::AESGCMEncryptor> encryptor_;

  // ros
  rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr sub_;
};

} // namespace avs

int main(int argc, char* argv[]) {
  rclcpp::init(argc, argv);
  try {
    rclcpp::spin(std::make_shared<avs::ImgEncryptNode>());
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
  }
  rclcpp::shutdown();
  return 0;
}
