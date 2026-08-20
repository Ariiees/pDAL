#include "pdal/model/types.h"

#include <array>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

namespace pdal {

AuthorizedAccessPlan::AuthorizedAccessPlan(
    std::string request_id, std::string principal_id,
    std::vector<AuthorizedResource> resources, AccessLimits limits,
    std::uint64_t issued_at_ns, std::uint64_t expires_at_ns,
    std::string policy_version)
    : request_id_(std::move(request_id)),
      principal_id_(std::move(principal_id)),
      resources_(std::move(resources)),
      limits_(limits),
      issued_at_ns_(issued_at_ns),
      expires_at_ns_(expires_at_ns),
      policy_version_(std::move(policy_version)) {}

std::uint64_t SystemNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string GenerateRequestId() {
  thread_local std::mt19937_64 random(std::random_device{}());
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t i = 0; i < bytes.size(); i += sizeof(std::uint64_t)) {
    const auto value = random();
    for (std::size_t j = 0; j < sizeof(value); ++j) {
      bytes[i + j] = static_cast<std::uint8_t>(value >> (8 * j));
    }
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
    out << std::setw(2) << static_cast<unsigned>(bytes[i]);
  }
  return out.str();
}

std::string DeliveryModeName(DeliveryMode mode) {
  return mode == DeliveryMode::kMetadata ? "metadata" : "stream";
}

std::string PlanOperationName(PlanOperation operation) {
  switch (operation) {
    case PlanOperation::kResolveResource: return "ResolveResource";
    case PlanOperation::kLocateTimeRange: return "LocateTimeRange";
    case PlanOperation::kSelectRecords: return "SelectRecords";
    case PlanOperation::kReadPayload: return "ReadPayload";
    case PlanOperation::kTransformRepresentation: return "TransformRepresentation";
    case PlanOperation::kStreamResult: return "StreamResult";
  }
  return "Unknown";
}

}  // namespace pdal
