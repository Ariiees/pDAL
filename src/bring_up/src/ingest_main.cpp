#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/image.hpp"
#include "std_msgs/msg/int64.hpp"

#include "avs/img_dedup.h"
#include "avs/common.h"
#include "avs/append_logger.h"
#include "avs/trip_manager.h"
#include "avs/topic_map.h"

#include "pacc/encrypto.h"

#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <chrono>
#include <string>
#include <memory>
#include <vector>
#include <utility>

using std::placeholders::_1;
namespace fs = std::filesystem;

namespace avs
{

static std::vector<uint8_t> PackEncryptedPayload(
  const std::vector<unsigned char>& iv,
  const std::vector<unsigned char>& tag,
  const std::vector<unsigned char>& cipher)
{
  if (iv.size() != avs::crypto::AESGCMEncryptor::IV_LENGTH ||
      tag.size() != avs::crypto::AESGCMEncryptor::TAG_LENGTH) {
    throw std::runtime_error("Invalid iv or tag length");
  }

  std::vector<uint8_t> out;
  out.reserve(iv.size() + tag.size() + cipher.size());

  out.insert(out.end(), iv.begin(), iv.end());
  out.insert(out.end(), tag.begin(), tag.end());
  out.insert(out.end(), cipher.begin(), cipher.end());

  return out;
}

class ImgProcessNode : public rclcpp::Node
{
public:
  ImgProcessNode()
  : Node("avs_image_ingest")
  {
    this->declare_parameter<std::string>("config_path", "/home/avs/PaCC/src/avs/config/config.yaml");
    this->declare_parameter<std::string>("topic_map_path", "/home/avs/PaCC/src/avs/config/topics.yaml");

    std::string config_path = this->get_parameter("config_path").as_string();
    std::string topic_map_path = this->get_parameter("topic_map_path").as_string();

    YAML::Node root = YAML::LoadFile(config_path);
    YAML::Node common = root["common"];
    YAML::Node crypto = root["crypto"];

    image_topic_ = common["img_topic"].as<std::string>();
    std::string ssd_root = common["ssd_root"].as<std::string>();

    std::string folder_name = avs::GetTopicFolder(avs::LoadTopicMap(topic_map_path), image_topic_);
    std::string current_day = avs::getCurrentDayFolder();

    image_path_ = (fs::path(ssd_root) / fs::path(folder_name) / fs::path(current_day)).string();
    std::error_code ec;
    if (!avs::ensureDirectory(image_path_, &ec)) {
      RCLCPP_WARN(
        this->get_logger(),
        "Image directory %s did not exist. Attempted to create it (ec=%d: %s).",
        image_path_.c_str(),
        ec.value(),
        ec.message().c_str());
    }

    deduplicator_ = std::make_shared<avs::ImgDeduplicator>(image_path_, config_path);

    trip_mgr_ = std::make_shared<avs::TripManager>();
    append_logger_ = std::make_shared<avs::AppendLogger>(ssd_root, image_topic_);

    int trip_id = trip_mgr_->GetTripId(image_path_);

    const uint64_t trip_start_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

    append_logger_->startTrip(current_day, folder_name, trip_id, trip_start_ns);

    bool crypto_enabled = true;
    if (crypto && crypto["enabled"]) crypto_enabled = crypto["enabled"].as<bool>(true);

    if (crypto_enabled) {
      std::string key_path = crypto["key_path"].as<std::string>("/tmp/pacc_aes_gcm.key");
      bool generate_if_missing = crypto["generate_if_missing"].as<bool>(true);

      if (fs::exists(key_path)) {
        encryptor_ = std::make_shared<avs::crypto::AESGCMEncryptor>(
          avs::crypto::AESGCMEncryptor::FromKeyFile(key_path));
        RCLCPP_INFO(this->get_logger(), "PaCC crypto enabled. Loaded key from %s", key_path.c_str());
      } else if (generate_if_missing) {
        auto e = avs::crypto::AESGCMEncryptor();
        if (!e.saveKey(key_path)) {
          throw std::runtime_error("Failed to write crypto key to " + key_path);
        }
        encryptor_ = std::make_shared<avs::crypto::AESGCMEncryptor>(e);
        RCLCPP_WARN(this->get_logger(), "PaCC crypto enabled. Generated new key at %s", key_path.c_str());
      } else {
        throw std::runtime_error("PaCC crypto enabled but key missing at " + key_path);
      }
    } else {
      RCLCPP_WARN(this->get_logger(), "PaCC crypto disabled. Payloads will be appended in plaintext.");
    }

    subscription_ = this->create_subscription<sensor_msgs::msg::Image>(
      image_topic_, make_auto_qos(image_topic_),
      std::bind(&ImgProcessNode::imageCallback, this, _1));


    RCLCPP_INFO(
      this->get_logger(),
      "AVS ingest started. Subscribed to %s. Append logging under %s trip=%d",
      image_topic_.c_str(),
      image_path_.c_str(),
      trip_id);
  }

  ~ImgProcessNode() override
  {
    subscription_.reset();
    uint64_t trip_end_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    try {
      if (append_logger_) append_logger_->endTrip(trip_end_ns);
    } catch (...) {}
  }

private:
  void imageCallback(const sensor_msgs::msg::Image::SharedPtr msg)
  {
    uint64_t ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

    bool is_unique = false;
    std::vector<uint8_t> plain_bytes;

    try {
      is_unique = deduplicator_->isUniqueAndGetBytes(*msg, plain_bytes);
    } catch (const std::exception& e) {
      RCLCPP_ERROR(this->get_logger(), "Dedup GetBytes failed: %s", e.what());
      return;
    }

    if (!is_unique) return;

    std::vector<uint8_t> final_payload;

    try {
      if (encryptor_) {
        std::vector<unsigned char> cipher;
        std::vector<unsigned char> iv;
        std::vector<unsigned char> tag;

        const std::vector<unsigned char> plain(plain_bytes.begin(), plain_bytes.end());

        if (!encryptor_->encrypt(plain, cipher, iv, tag)) {
          RCLCPP_ERROR(this->get_logger(), "PaCC encrypt failed. Dropping frame.");
          return;
        }

        final_payload = PackEncryptedPayload(iv, tag, cipher);
      } else {
        final_payload = std::move(plain_bytes);
      }
    } catch (const std::exception& e) {
      RCLCPP_ERROR(this->get_logger(), "Crypto packing failed: %s", e.what());
      return;
    }

    try {
      append_logger_->appendRecord(ts_ns, final_payload);
      uint64_t ts_final = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
      RCLCPP_INFO(
        this->get_logger(),
        "PaCC ingest latency: %0.2f ms",
        static_cast<double>(ts_final - ts_ns) / 1e6);
    } catch (const std::exception& e) {
      RCLCPP_ERROR(this->get_logger(), "AppendLogger appendRecord failed: %s", e.what());
    }
  }

  rclcpp::QoS make_auto_qos(const std::string& topic)
  {
    rclcpp::QoS qos = rclcpp::SensorDataQoS();
    qos.durability(rclcpp::DurabilityPolicy::Volatile);

    for (int i = 0; i < 50; ++i) {
      auto infos = this->get_publishers_info_by_topic(topic);
      if (!infos.empty()) {
        auto offered = infos.front().qos_profile();
        qos.reliability(offered.reliability());
        RCLCPP_INFO(
          this->get_logger(),
          "QoS for %s depth=5 reliability=%d durability=%d",
          topic.c_str(),
          static_cast<int>(qos.get_rmw_qos_profile().reliability),
          static_cast<int>(qos.get_rmw_qos_profile().durability));
        return qos;
      }
      rclcpp::sleep_for(std::chrono::milliseconds(100));
    }

    RCLCPP_WARN(
      this->get_logger(),
      "No publisher QoS detected on %s. Using SensorDataQoS fallback.",
      topic.c_str());
    return qos;
  }

private:
  std::string image_topic_;
  std::string image_path_;

  std::shared_ptr<avs::ImgDeduplicator> deduplicator_;
  std::shared_ptr<avs::AppendLogger> append_logger_;
  std::shared_ptr<avs::TripManager> trip_mgr_;

  std::shared_ptr<avs::crypto::AESGCMEncryptor> encryptor_;

  rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr subscription_;
};

} // namespace avs

int main(int argc, char* argv[])
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<avs::ImgProcessNode>());
  rclcpp::shutdown();
  return 0;
}
