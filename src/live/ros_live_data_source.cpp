#include "pdal/live/ros_live_data_source.h"

#include <cv_bridge/cv_bridge.hpp>
#include <gps_msgs/msg/gps_fix.hpp>
#include <opencv2/imgcodecs.hpp>
#include <pcl_conversions/pcl_conversions.h>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "avs/lidar_compress.h"
#include "pdal/model/error.h"

namespace pdal {
namespace {

std::uint64_t TimestampNs(const builtin_interfaces::msg::Time& stamp) {
  const auto value = static_cast<std::uint64_t>(stamp.sec) * 1000000000ULL +
                     static_cast<std::uint64_t>(stamp.nanosec);
  return value == 0 ? SystemNowNs() : value;
}

struct GpsPayload {
  double latitude;
  double longitude;
  double altitude;
  double cov_xx;
  double cov_yy;
  double cov_zz;
};

struct SubscriptionQueue {
  explicit SubscriptionQueue(std::size_t capacity, bool metadata)
      : capacity_bytes(std::max<std::size_t>(1, capacity)),
        metadata_only(metadata) {}

  void Push(const DataSample& sample) {
    const auto size = metadata_only ? 0 : (sample.payload ? sample.payload->size() : 0);
    std::lock_guard lock(mutex);
    if (closed) return;
    while (!samples.empty() && bytes + size > capacity_bytes) {
      bytes -= samples.front().payload ? samples.front().payload->size() : 0;
      samples.pop_front();
      ++dropped;
    }
    if (size > capacity_bytes) {
      ++dropped;
      return;
    }
    auto queued = sample;
    if (metadata_only) queued.payload = std::make_shared<const std::vector<std::uint8_t>>();
    samples.push_back(std::move(queued));
    bytes += metadata_only ? 0 : size;
    condition.notify_one();
  }

  std::optional<DataSample> Next() {
    std::unique_lock lock(mutex);
    condition.wait(lock, [&] { return closed || !samples.empty(); });
    if (samples.empty()) return std::nullopt;
    auto sample = std::move(samples.front());
    bytes -= sample.payload ? sample.payload->size() : 0;
    samples.pop_front();
    sample.metadata["samples_dropped"] = dropped;
    dropped = 0;
    return sample;
  }

  void Close() {
    std::lock_guard lock(mutex);
    closed = true;
    samples.clear();
    bytes = 0;
    condition.notify_all();
  }

  std::mutex mutex;
  std::condition_variable condition;
  std::deque<DataSample> samples;
  std::size_t capacity_bytes;
  std::size_t bytes = 0;
  std::uint64_t dropped = 0;
  bool metadata_only = false;
  bool closed = false;
};

}  // namespace

class RosLiveDataSource::Impl {
 public:
  Impl(const ResourceCatalog& catalog, std::size_t maximum_subscriptions,
       std::size_t queue_bytes)
      : max_subscriptions(std::max<std::size_t>(1, maximum_subscriptions)),
        default_queue_bytes(std::max<std::size_t>(1, queue_bytes)) {
    if (!rclcpp::ok()) {
      int argc = 0;
      char** argv = nullptr;
      rclcpp::init(argc, argv);
    }
    node = std::make_shared<rclcpp::Node>("pdal_ros_live_source");
    for (const auto& resource : catalog.Entries()) {
      if (!resource.live_binding || resource.live_binding->backend_id != "ros") continue;
      const auto type = resource.live_binding->options.find("message_type");
      if (type == resource.live_binding->options.end()) continue;
      const auto topic = resource.live_binding->source;
      if (type->second == "sensor_msgs/msg/Image") {
        auto subscription = node->create_subscription<sensor_msgs::msg::Image>(
            topic, rclcpp::SensorDataQoS(),
            [this, id = resource.resource_id](sensor_msgs::msg::Image::ConstSharedPtr message) {
              try {
                const auto image = cv_bridge::toCvCopy(message, "bgr8")->image;
                auto bytes = std::make_shared<std::vector<std::uint8_t>>();
                if (!cv::imencode(".jpg", image, *bytes,
                                  {cv::IMWRITE_JPEG_QUALITY, 95})) return;
                Publish({id, TimestampNs(message->header.stamp), "jpeg", "image/jpeg",
                         std::move(bytes), {}});
              } catch (const std::exception&) {
              }
            });
        subscriptions.push_back(subscription);
      } else if (type->second == "gps_msgs/msg/GPSFix") {
        auto subscription = node->create_subscription<gps_msgs::msg::GPSFix>(
            topic, rclcpp::SensorDataQoS(),
            [this, id = resource.resource_id](gps_msgs::msg::GPSFix::ConstSharedPtr message) {
              const GpsPayload gps{message->latitude, message->longitude, message->altitude,
                                   message->position_covariance[0],
                                   message->position_covariance[4],
                                   message->position_covariance[8]};
              auto bytes = std::make_shared<std::vector<std::uint8_t>>(sizeof(gps));
              std::memcpy(bytes->data(), &gps, sizeof(gps));
              Publish({id, TimestampNs(message->header.stamp), "position-binary-v1",
                       "application/vnd.pdal.position-v1", std::move(bytes), {}});
            });
        subscriptions.push_back(subscription);
      } else if (type->second == "sensor_msgs/msg/PointCloud2") {
        auto subscription = node->create_subscription<sensor_msgs::msg::PointCloud2>(
            topic, rclcpp::SensorDataQoS(),
            [this, id = resource.resource_id](sensor_msgs::msg::PointCloud2::ConstSharedPtr message) {
              try {
                auto cloud = std::make_shared<pcl::PointCloud<pcl::PointXYZI>>();
                pcl::fromROSMsg(*message, *cloud);
                auto bytes = std::make_shared<std::vector<std::uint8_t>>();
                lidar_compressor.getLAZ(cloud, *bytes);
                Publish({id, TimestampNs(message->header.stamp), "laz",
                         "application/vnd.laszip", std::move(bytes), {}});
              } catch (const std::exception&) {
              }
            });
        subscriptions.push_back(subscription);
      }
    }
    executor = std::make_shared<rclcpp::executors::SingleThreadedExecutor>();
    executor->add_node(node);
    spin_thread = std::thread([executor = executor] { executor->spin(); });
  }

  ~Impl() {
    {
      std::lock_guard lock(mutex);
      for (auto& [_, queues] : subscribers) {
        for (const auto& weak : queues) {
          if (const auto queue = weak.lock()) queue->Close();
        }
      }
    }
    executor->cancel();
    if (spin_thread.joinable()) spin_thread.join();
    executor->remove_node(node);
  }

  void Publish(DataSample sample) {
    std::vector<std::shared_ptr<SubscriptionQueue>> targets;
    {
      std::lock_guard lock(mutex);
      latest[sample.resource_id] = sample;
      auto& queues = subscribers[sample.resource_id];
      std::erase_if(queues, [](const auto& weak) { return weak.expired(); });
      for (const auto& weak : queues) {
        if (const auto queue = weak.lock()) targets.push_back(queue);
      }
    }
    for (const auto& queue : targets) queue->Push(sample);
  }

  mutable std::mutex mutex;
  mutable std::size_t active_subscriptions = 0;
  std::size_t max_subscriptions;
  std::size_t default_queue_bytes;
  std::unordered_map<std::string, DataSample> latest;
  std::unordered_map<std::string, std::vector<std::weak_ptr<SubscriptionQueue>>> subscribers;
  std::shared_ptr<rclcpp::Node> node;
  std::shared_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
  std::vector<rclcpp::SubscriptionBase::SharedPtr> subscriptions;
  std::thread spin_thread;
  avs::LidarCompressor lidar_compressor{"/tmp/pdal-live-lidar"};
};

RosLiveDataSource::RosLiveDataSource(const ResourceCatalog& catalog,
                                     std::size_t max_subscriptions,
                                     std::size_t default_queue_bytes)
    : impl_(std::make_shared<Impl>(catalog, max_subscriptions,
                                   default_queue_bytes)) {}

RosLiveDataSource::~RosLiveDataSource() = default;

LiveSourceCapabilities RosLiveDataSource::GetCapabilities() const {
  return {"ros", true, true, impl_->max_subscriptions};
}

std::optional<DataSample> RosLiveDataSource::Latest(
    const ResourceDescriptor& resource,
    const RepresentationRequest& representation) const {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->latest.find(resource.resource_id);
  if (found == impl_->latest.end()) return std::nullopt;
  auto sample = found->second;
  if (representation.format == "metadata" ||
      representation.format == "metadata-only") {
    sample.payload = std::make_shared<const std::vector<std::uint8_t>>();
    sample.representation = "metadata";
    sample.content_type = "application/json";
  }
  return sample;
}

DataStream RosLiveDataSource::Subscribe(
    const ResourceDescriptor& resource,
    const RepresentationRequest& representation,
    const QueryOptions&) const {
  const bool metadata = representation.format == "metadata" ||
                        representation.format == "metadata-only";
  const auto configured = resource.limits.stream_buffer_bytes > 0
                              ? resource.limits.stream_buffer_bytes
                              : impl_->default_queue_bytes;
  auto queue = std::make_shared<SubscriptionQueue>(configured, metadata);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->active_subscriptions >= impl_->max_subscriptions) {
      throw PdalError(ErrorClass::kQueryTooLarge,
                      "maximum live subscriptions reached");
    }
    ++impl_->active_subscriptions;
    impl_->subscribers[resource.resource_id].push_back(queue);
  }
  auto released = std::make_shared<std::atomic_bool>(false);
  auto release = [impl = impl_, queue, released] {
    if (released->exchange(true)) return;
    queue->Close();
    std::lock_guard lock(impl->mutex);
    if (impl->active_subscriptions > 0) --impl->active_subscriptions;
  };
  return DataStream([queue] { return queue->Next(); }, std::move(release));
}

}  // namespace pdal
