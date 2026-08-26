#include "pdal/pipeline.h"

#include <boost/json/array.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include "pdal/adapter/request_translator.h"
#include "pdal/model/error.h"
#include "pdal/model/json.h"

namespace pdal {
namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string Base64UrlEncode(const unsigned char* data, std::size_t size) {
  std::string out;
  out.reserve((size * 4 + 2) / 3);
  for (std::size_t i = 0; i < size; i += 3) {
    const std::uint32_t value = static_cast<std::uint32_t>(data[i]) << 16U |
                                (i + 1 < size ? static_cast<std::uint32_t>(data[i + 1]) << 8U : 0U) |
                                (i + 2 < size ? static_cast<std::uint32_t>(data[i + 2]) : 0U);
    out.push_back(kBase64Alphabet[(value >> 18U) & 63U]);
    out.push_back(kBase64Alphabet[(value >> 12U) & 63U]);
    if (i + 1 < size) out.push_back(kBase64Alphabet[(value >> 6U) & 63U]);
    if (i + 2 < size) out.push_back(kBase64Alphabet[value & 63U]);
  }
  return out;
}

std::string Base64UrlEncode(const std::string& value) {
  return Base64UrlEncode(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

std::vector<unsigned char> Base64UrlDecode(const std::string& input) {
  std::array<int, 256> table{};
  table.fill(-1);
  for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(kBase64Alphabet[i])] = i;
  std::vector<unsigned char> out;
  out.reserve(input.size() * 3 / 4 + 2);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const unsigned char ch : input) {
    if (table[ch] < 0) throw PdalError(ErrorClass::kInvalidRequest, "invalid continuation token encoding");
    buffer = (buffer << 6U) | static_cast<std::uint32_t>(table[ch]);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<unsigned char>((buffer >> bits) & 0xffU));
    }
  }
  return out;
}

std::string RequestFingerprint(PdalRequest request) {
  request.request_id.clear();
  request.delivery.continuation_token.reset();
  const auto serialized = boost::json::serialize(RequestToJson(request));
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char*>(serialized.data()), serialized.size(),
         digest.data());
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    out.push_back(hex[byte >> 4U]);
    out.push_back(hex[byte & 0x0fU]);
  }
  return out;
}

std::array<unsigned char, SHA256_DIGEST_LENGTH> Sign(
    const std::string& secret, const std::string& payload) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> signature{};
  unsigned int size = 0;
  HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
       reinterpret_cast<const unsigned char*>(payload.data()), payload.size(),
       signature.data(), &size);
  if (size != signature.size()) throw std::runtime_error("HMAC-SHA256 failed");
  return signature;
}

double ElapsedMs(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

struct Usage {
  std::uint64_t user_cpu_us = 0;
  std::uint64_t system_cpu_us = 0;
  std::uint64_t max_rss_kb = 0;
};

Usage CurrentUsage() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return {};
  const auto micros = [](const timeval& value) {
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000ULL +
           static_cast<std::uint64_t>(value.tv_usec);
  };
  return {micros(usage.ru_utime), micros(usage.ru_stime),
          static_cast<std::uint64_t>(usage.ru_maxrss)};
}

}  // namespace

ContinuationCodec::ContinuationCodec(std::string development_secret)
    : secret_(std::move(development_secret)) {
  if (secret_.size() < 16) {
    throw std::invalid_argument("continuation signing secret must be at least 16 bytes");
  }
}

std::string ContinuationCodec::Encode(const PdalRequest& request,
                                      std::uint64_t next_offset,
                                      std::uint64_t expires_at_ns) const {
  const std::string payload = boost::json::serialize(boost::json::object{
      {"v", 1}, {"offset", next_offset}, {"expires_at_ns", expires_at_ns},
      {"fingerprint", RequestFingerprint(request)}});
  const auto signature = Sign(secret_, payload);
  return Base64UrlEncode(payload) + "." +
         Base64UrlEncode(signature.data(), signature.size());
}

std::uint64_t ContinuationCodec::DecodeAndValidate(
    const std::string& token, const PdalRequest& request) const {
  const auto separator = token.find('.');
  if (separator == std::string::npos || token.find('.', separator + 1) != std::string::npos) {
    throw PdalError(ErrorClass::kInvalidRequest, "malformed continuation token");
  }
  const auto payload_bytes = Base64UrlDecode(token.substr(0, separator));
  const auto provided_signature = Base64UrlDecode(token.substr(separator + 1));
  const std::string payload(payload_bytes.begin(), payload_bytes.end());
  const auto expected_signature = Sign(secret_, payload);
  if (provided_signature.size() != expected_signature.size() ||
      CRYPTO_memcmp(provided_signature.data(), expected_signature.data(),
                    expected_signature.size()) != 0) {
    throw PdalError(ErrorClass::kInvalidRequest, "invalid continuation token signature");
  }
  boost::json::value parsed;
  try {
    parsed = boost::json::parse(payload);
  } catch (const std::exception&) {
    throw PdalError(ErrorClass::kInvalidRequest, "invalid continuation token payload");
  }
  if (!parsed.is_object()) {
    throw PdalError(ErrorClass::kInvalidRequest, "invalid continuation token payload");
  }
  const auto& object = parsed.as_object();
  const auto* version = object.if_contains("v");
  const auto* offset = object.if_contains("offset");
  const auto* expiry = object.if_contains("expires_at_ns");
  const auto* fingerprint = object.if_contains("fingerprint");
  if (!version || !offset || !expiry || !fingerprint ||
      version->to_number<std::uint64_t>() != 1 || !fingerprint->is_string()) {
    throw PdalError(ErrorClass::kInvalidRequest, "invalid continuation token fields");
  }
  if (expiry->to_number<std::uint64_t>() < SystemNowNs()) {
    throw PdalError(ErrorClass::kInvalidRequest, "continuation token has expired");
  }
  if (fingerprint->as_string() != RequestFingerprint(request)) {
    throw PdalError(ErrorClass::kInvalidRequest,
                    "continuation token does not match this request");
  }
  return offset->to_number<std::uint64_t>();
}

bool RequestRegistry::Begin(RequestStatus status) {
  std::lock_guard lock(mutex_);
  if (statuses_.contains(status.request_id)) return false;
  order_.push_back(status.request_id);
  statuses_.emplace(status.request_id, std::move(status));
  while (order_.size() > capacity_) {
    statuses_.erase(order_.front());
    order_.erase(order_.begin());
  }
  return true;
}

void RequestRegistry::Put(RequestStatus status) {
  std::lock_guard lock(mutex_);
  if (!statuses_.contains(status.request_id)) order_.push_back(status.request_id);
  statuses_[status.request_id] = std::move(status);
  while (order_.size() > capacity_) {
    statuses_.erase(order_.front());
    order_.erase(order_.begin());
  }
}

std::optional<RequestStatus> RequestRegistry::Get(
    const std::string& request_id) const {
  std::lock_guard lock(mutex_);
  const auto found = statuses_.find(request_id);
  if (found == statuses_.end()) return std::nullopt;
  return found->second;
}

PdalPipeline::PdalPipeline(ResourceCatalog catalog,
                           std::shared_ptr<const PolicyEngine> policy,
                           std::shared_ptr<const BackendRegistry> backends,
                           std::shared_ptr<AuditSink> audit,
                           ContinuationCodec continuation,
                           std::shared_ptr<RequestRegistry> registry)
    : catalog_(std::move(catalog)),
      policy_(std::move(policy)),
      backends_(std::move(backends)),
      audit_(std::move(audit)),
      continuation_(std::move(continuation)),
      registry_(std::move(registry)) {
  if (!policy_ || !backends_ || !audit_ || !registry_) {
    throw std::invalid_argument("pDAL pipeline dependencies must not be null");
  }
}

PreparedQuery PdalPipeline::Prepare(PdalRequest request) const {
  const auto started_at_ns = SystemNowNs();
  const auto usage_start = CurrentUsage();
  ValidateRequest(request);
  for (auto& resource : request.resources) {
    resource = catalog_.CanonicalId(resource);
  }
  if (!registry_->Begin(
          {request.request_id, RequestState::kReceived, 0, 0, {}})) {
    throw PdalError(ErrorClass::kInvalidRequest,
                    "request_id has already been used",
                    {{"request_id", request.request_id}});
  }
  audit_->Write(AuditEvent("request_received", request.request_id,
                           {{"resource_count", request.resources.size()},
                            {"principal_id", request.principal.principal_id},
                            {"purpose", request.purpose}}));
  try {
    audit_->Write(AuditEvent("request_normalized", request.request_id,
                             {{"api_version", request.api_version}}));
    const auto policy_started = std::chrono::steady_clock::now();
    auto access = policy_->Authorize(request, catalog_);
    audit_->Write(AuditEvent("policy_decision", request.request_id,
                             {{"decision", "authorized"},
                              {"policy_version", access.policy_version()},
                              {"authorized_resource_count", access.resources().size()},
                              {"latency_ms", ElapsedMs(policy_started)}}));

    const auto planning_started = std::chrono::steady_clock::now();
    auto plan = planner_.Plan(access, catalog_);
    audit_->Write(AuditEvent("plan_created", request.request_id,
                             {{"task_count", plan.tasks.size()},
                              {"latency_ms", ElapsedMs(planning_started)}}));

    std::uint64_t offset = 0;
    if (request.delivery.continuation_token) {
      offset = continuation_.DecodeAndValidate(*request.delivery.continuation_token,
                                               request);
      if (offset > 1000000) {
        throw PdalError(ErrorClass::kQueryTooLarge,
                        "continuation offset exceeds the bounded scan limit");
      }
    }

    ResponseMetadata metadata;
    metadata.request_id = request.request_id;
    metadata.policy_version = access.policy_version();
    metadata.time_range = {std::numeric_limits<std::uint64_t>::max(), 0};
    for (const auto& authorized : access.resources()) {
      const auto& descriptor = catalog_.Get(authorized.resource_id);
      const auto negotiated = representations_.Negotiate(descriptor,
                                                          authorized.representation);
      metadata.resources.push_back(authorized.resource_id);
      metadata.representations[authorized.resource_id] = negotiated.format;
      metadata.time_range.start_ns = std::min(metadata.time_range.start_ns,
                                               authorized.time.start_ns);
      metadata.time_range.end_ns = std::max(metadata.time_range.end_ns,
                                             authorized.time.end_ns);
    }

    std::vector<BackendRecord> all_records;
    std::uint64_t records_considered = 0;
    bool backend_has_more = false;
    const bool single_resource = plan.tasks.size() == 1;
    for (const auto& task : plan.tasks) {
      const auto& descriptor = catalog_.Get(task.resource_id);
      const auto& backend = backends_->Get(task.backend_id);
      backend.Resolve(descriptor);
      audit_->Write(AuditEvent("backend_selected", request.request_id,
                               {{"resource_id", task.resource_id},
                                {"backend", task.backend_id}}));
      const auto query_started = std::chrono::steady_clock::now();
      const auto task_offset = single_resource ? offset : 0;
      const auto lookahead = plan.limits.max_records + 1;
      const auto task_limit = single_resource
                                  ? lookahead
                                  : std::min<std::uint64_t>(
                                        100000, lookahead + offset);
      auto cursor = backend.OpenHistory({task, task_offset, task_limit}, catalog_);
      std::vector<BackendRecord> records;
      while (auto record = cursor->Next()) records.push_back(std::move(*record));
      records_considered += cursor->records_considered();
      backend_has_more = backend_has_more || cursor->has_more();
      audit_->Write(AuditEvent("records_selected", request.request_id,
                               {{"resource_id", task.resource_id},
                                {"records_considered", cursor->records_considered()},
                                {"records_selected", records.size()},
                                {"backend_query_latency_ms", ElapsedMs(query_started)}}));

      std::uint64_t last_selected_ts = 0;
      std::uint64_t minimum_period_ns = 0;
      if (task.representation.sampling.max_frequency_hz) {
        minimum_period_ns = static_cast<std::uint64_t>(
            1000000000.0 / *task.representation.sampling.max_frequency_hz);
      }
      std::uint64_t index = 0;
      for (auto& record : records) {
        const bool every_n = index++ % task.representation.sampling.every_n == 0;
        const bool frequency = minimum_period_ns == 0 || last_selected_ts == 0 ||
                               record.timestamp_ns - last_selected_ts >= minimum_period_ns;
        if (every_n && frequency) {
          last_selected_ts = record.timestamp_ns;
          all_records.emplace_back(std::move(record));
        }
      }
    }
    std::sort(all_records.begin(), all_records.end(), [](const auto& left, const auto& right) {
      if (left.timestamp_ns != right.timestamp_ns) return left.timestamp_ns < right.timestamp_ns;
      return left.resource_id < right.resource_id;
    });

    const std::uint64_t page_offset = single_resource ? 0 : offset;
    if (!single_resource && page_offset > all_records.size()) {
        throw PdalError(ErrorClass::kInvalidRequest,
                        "continuation offset is beyond the current result set");
    }
    PlannedResult result;
    result.plan = plan;
    result.records_considered = records_considered;
    const bool metadata_only = request.delivery.mode == DeliveryMode::kMetadata ||
                               request.representation.format == "metadata" ||
                               request.representation.format == "metadata-only";
    std::size_t cursor = static_cast<std::size_t>(page_offset);
    while (cursor < all_records.size() &&
           result.records.size() < plan.limits.max_records) {
      const auto& candidate = all_records[cursor];
      const auto selected_size = metadata_only ? 0 : candidate.payload_size;
      if (selected_size > plan.limits.max_bytes - result.bytes_selected) break;
      result.bytes_selected += selected_size;
      result.records.push_back(candidate);
      ++cursor;
    }
    if (!metadata_only && result.records.empty() && cursor < all_records.size() &&
        all_records[cursor].payload_size > plan.limits.max_bytes) {
      throw PdalError(ErrorClass::kRangeNotSatisfiable,
                      "the next record exceeds the authorized byte limit",
                      {{"record_size", all_records[cursor].payload_size},
                       {"max_bytes", plan.limits.max_bytes}});
    }
    if (cursor < all_records.size() || backend_has_more) {
      constexpr std::uint64_t kContinuationTtlNs =
          15ULL * 60ULL * 1000ULL * 1000ULL * 1000ULL;
      result.continuation_token = continuation_.Encode(
          request, offset + result.records.size(),
          std::min<std::uint64_t>(access.expires_at_ns(),
                                  SystemNowNs() + kContinuationTtlNs));
    }
    metadata.record_count = result.records.size();
    metadata.byte_count = result.bytes_selected;
    metadata.continuation = result.continuation_token;
    registry_->Put({request.request_id, RequestState::kPlanned,
                    metadata.record_count, 0, {}});
    return {std::move(request), std::move(access), std::move(result),
            std::move(metadata), started_at_ns, usage_start.user_cpu_us,
            usage_start.system_cpu_us};
  } catch (const PdalError& error) {
    registry_->Put({request.request_id, RequestState::kFailed, 0, 0,
                    ErrorCode(error.error_class())});
    audit_->Write(AuditEvent("request_failed", request.request_id,
                             {{"error_code", ErrorCode(error.error_class())}}));
    throw;
  } catch (const std::exception&) {
    registry_->Put({request.request_id, RequestState::kFailed, 0, 0,
                    ErrorCode(ErrorClass::kInternalError)});
    audit_->Write(AuditEvent("request_failed", request.request_id,
                             {{"error_code", ErrorCode(ErrorClass::kInternalError)}}));
    throw PdalError(ErrorClass::kInternalError,
                    "an internal error occurred");
  }
}

std::uint64_t PdalPipeline::Stream(const PreparedQuery& prepared,
                                   const StreamCallbacks& callbacks) const {
  registry_->Put({prepared.request.request_id, RequestState::kStreaming,
                  prepared.metadata.record_count, 0, {}});
  std::uint64_t returned = 0;
  boost::json::object tier_bytes;
  try {
    const bool metadata_only = prepared.request.delivery.mode == DeliveryMode::kMetadata ||
                               prepared.request.representation.format == "metadata" ||
                               prepared.request.representation.format == "metadata-only";
    if (!metadata_only) {
      for (const auto& record : prepared.result.records) {
        if (callbacks.begin_record && !callbacks.begin_record(record)) {
          throw PdalError(ErrorClass::kBackendUnavailable,
                          "record consumer disconnected");
        }
        const auto& backend = backends_->Get(record.backend_id);
        const auto read = backend.Read(record, [&](const std::uint8_t* data, std::size_t size) {
          if (returned + size > prepared.result.plan.limits.max_bytes) return false;
          if (callbacks.write_bytes && !callbacks.write_bytes(data, size)) return false;
          returned += size;
          return true;
        });
        if (read != record.payload_size) {
          throw PdalError(ErrorClass::kPartialRead,
                          "backend returned an incomplete record");
        }
        const auto existing = tier_bytes.if_contains(record.storage_tier);
        const auto previous = existing ? existing->to_number<std::uint64_t>() : 0;
        tier_bytes[record.storage_tier] = previous + read;
        if (callbacks.end_record && !callbacks.end_record()) {
          throw PdalError(ErrorClass::kBackendUnavailable,
                          "record consumer disconnected");
        }
      }
    }
    registry_->Put({prepared.request.request_id, RequestState::kCompleted,
                    prepared.metadata.record_count, returned, {}});
    audit_->Write(AuditEvent("bytes_returned", prepared.request.request_id,
                             {{"bytes", returned}}));
    audit_->Write(AuditEvent("bytes_read", prepared.request.request_id,
                             {{"bytes", returned}, {"by_tier", tier_bytes}}));
    const auto usage_end = CurrentUsage();
    audit_->Write(AuditEvent("request_completed", prepared.request.request_id,
                             {{"records", prepared.metadata.record_count},
                              {"bytes", returned},
                              {"latency_ms", static_cast<double>(
                                   SystemNowNs() - prepared.started_at_ns) / 1000000.0},
                              {"user_cpu_us", usage_end.user_cpu_us -
                                                  prepared.user_cpu_us_start},
                              {"system_cpu_us", usage_end.system_cpu_us -
                                                    prepared.system_cpu_us_start},
                              {"max_rss_kb", usage_end.max_rss_kb}}));
    return returned;
  } catch (const PdalError& error) {
    registry_->Put({prepared.request.request_id, RequestState::kFailed,
                    prepared.metadata.record_count, returned,
                    ErrorCode(error.error_class())});
    audit_->Write(AuditEvent("request_failed", prepared.request.request_id,
                             {{"error_code", ErrorCode(error.error_class())},
                              {"bytes_returned", returned}}));
    throw;
  } catch (const std::exception&) {
    registry_->Put({prepared.request.request_id, RequestState::kFailed,
                    prepared.metadata.record_count, returned,
                    ErrorCode(ErrorClass::kInternalError)});
    audit_->Write(AuditEvent("request_failed", prepared.request.request_id,
                             {{"error_code", ErrorCode(ErrorClass::kInternalError)},
                              {"bytes_returned", returned}}));
    throw PdalError(ErrorClass::kInternalError,
                    "an internal error occurred");
  }
}

boost::json::object PdalPipeline::CapabilitiesJson() const {
  boost::json::array resources;
  for (const auto& resource : catalog_.List()) {
    resources.emplace_back(ResourceToJson(resource));
  }
  boost::json::array delivery_modes{"stream", "metadata"};
  boost::json::array query_features{"inclusive_time_range", "multi_resource",
                                     "sampling", "continuation"};
  return {{"api_version", kApiVersion},
          {"resources", std::move(resources)},
          {"query_features", std::move(query_features)},
          {"delivery_modes", std::move(delivery_modes)},
          {"limits", boost::json::object{{"max_resources", 16},
                                          {"max_records", 100000},
                                          {"max_bytes", 1073741824}}},
          {"historical_data", boost::json::object{{"available", true},
                                                    {"streaming", true}}},
          {"extensions", boost::json::object{{"sovd_style_bulk_data", "experimental"}}}};
}

std::string RequestStateName(RequestState state) {
  switch (state) {
    case RequestState::kReceived: return "received";
    case RequestState::kPlanned: return "planned";
    case RequestState::kStreaming: return "streaming";
    case RequestState::kCompleted: return "completed";
    case RequestState::kFailed: return "failed";
  }
  return "unknown";
}

}  // namespace pdal
