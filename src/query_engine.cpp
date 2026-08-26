#include "pdal/query_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

#include "pdal/model/error.h"

namespace pdal {
namespace {

bool Supports(const ResourceDescriptor& resource, Operation operation) {
  return std::find(resource.operations.begin(), resource.operations.end(),
                   operation) != resource.operations.end();
}

std::string ContentType(const ResourceDescriptor& resource,
                        const std::string& format) {
  if (format == "metadata") return "application/json";
  for (const auto& representation : resource.representations) {
    if (representation.format == format) return representation.content_type;
  }
  return "application/octet-stream";
}

bool IsNarrower(const DataQuery& original, const DataQuery& effective) {
  if (effective.request_id != original.request_id ||
      effective.resource != original.resource ||
      effective.operation != original.operation ||
      effective.options.max_records > original.options.max_records ||
      effective.options.max_bytes > original.options.max_bytes ||
      effective.options.sampling.every_n < original.options.sampling.every_n) {
    return false;
  }
  if (original.selector.time_range) {
    if (!effective.selector.time_range ||
        effective.selector.time_range->start_ns <
            original.selector.time_range->start_ns ||
        effective.selector.time_range->end_ns >
            original.selector.time_range->end_ns) {
      return false;
    }
  } else if (effective.selector.time_range) {
    return false;
  }
  if (original.options.sampling.max_frequency_hz &&
      (!effective.options.sampling.max_frequency_hz ||
       *effective.options.sampling.max_frequency_hz >
           *original.options.sampling.max_frequency_hz)) {
    return false;
  }
  return true;
}

}  // namespace

struct QueryEngine::BulkGate {
  explicit BulkGate(std::size_t maximum) : maximum(std::max<std::size_t>(1, maximum)) {}

  void Acquire() {
    std::unique_lock lock(mutex);
    condition.wait(lock, [&] { return active < maximum; });
    ++active;
  }
  void Release() {
    std::lock_guard lock(mutex);
    if (active > 0) --active;
    condition.notify_one();
  }

  std::mutex mutex;
  std::condition_variable condition;
  std::size_t maximum;
  std::size_t active = 0;
};

QueryEngine::QueryEngine(
    ResourceCatalog catalog,
    std::shared_ptr<const BackendRegistry> historical_backends,
    std::shared_ptr<const LiveSourceRegistry> live_sources,
    std::shared_ptr<const PolicyHook> policy_hook,
    std::shared_ptr<const PrivacyHook> privacy_hook,
    std::shared_ptr<AuditSink> audit,
    std::size_t max_concurrent_bulk_queries)
    : catalog_(std::move(catalog)),
      historical_backends_(std::move(historical_backends)),
      live_sources_(std::move(live_sources)),
      policy_hook_(std::move(policy_hook)),
      privacy_hook_(std::move(privacy_hook)),
      audit_(std::move(audit)),
      bulk_gate_(std::make_shared<BulkGate>(max_concurrent_bulk_queries)) {
  if (!historical_backends_ || !live_sources_ || !policy_hook_ ||
      !privacy_hook_ || !audit_) {
    throw std::invalid_argument("QueryEngine dependencies must not be null");
  }
}

std::vector<ResourceDescriptor> QueryEngine::Discover(std::string request_id) const {
  const auto started = std::chrono::steady_clock::now();
  if (request_id.empty()) request_id = GenerateRequestId();
  auto resources = catalog_.List();
  audit_->Write(AuditEvent(
      "discovery_completed", request_id,
      {{"resource_count", resources.size()},
       {"control_latency_us", static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count())}}));
  return resources;
}

ResourceDescriptor QueryEngine::Describe(
    const std::string& resource_id, std::string request_id) const {
  const auto started = std::chrono::steady_clock::now();
  if (request_id.empty()) request_id = GenerateRequestId();
  auto resource = static_cast<const ResourceDescriptor&>(catalog_.Get(resource_id));
  audit_->Write(AuditEvent(
      "describe_completed", request_id,
      {{"resource_id", resource.resource_id},
       {"control_latency_us", static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count())}}));
  return resource;
}

std::string QueryEngine::CanonicalResourceId(
    const std::string& resource_id) const {
  return catalog_.CanonicalId(resource_id);
}

DataResult QueryEngine::Execute(DataQuery query) const {
  const auto started = std::chrono::steady_clock::now();
  if (query.request_id.empty()) query.request_id = GenerateRequestId();
  const auto resolution_started = std::chrono::steady_clock::now();
  query.resource = catalog_.CanonicalId(query.resource);
  ValidateDataQuery(query);
  const auto& resource = catalog_.Get(query.resource);
  const auto resolution_completed = std::chrono::steady_clock::now();
  if (query.options.continuation_token) {
    throw PdalError(
        ErrorClass::kNotSupported,
        "continuation is available through the compatible bulk-history binding");
  }
  if (!Supports(resource, query.operation)) {
    throw PdalError(ErrorClass::kNotSupported,
                    "operation is not supported for this resource");
  }
  if (query.operation == Operation::kHistory &&
      !resource.historical_binding.backend_id.empty()) {
    const auto range = *query.selector.time_range;
    if (resource.limits.max_history_range_ns > 0 &&
        range.end_ns - range.start_ns > resource.limits.max_history_range_ns) {
      throw PdalError(ErrorClass::kQueryTooLarge,
                      "historical interval exceeds the resource limit");
    }
  }
  if (resource.limits.max_records > 0) {
    query.options.max_records = std::min(query.options.max_records,
                                         resource.limits.max_records);
  }
  if (resource.limits.max_bytes > 0) {
    query.options.max_bytes = std::min(query.options.max_bytes,
                                       resource.limits.max_bytes);
  }
  const auto bounded_query = query;
  query = policy_hook_->Apply(query, resource);
  query = privacy_hook_->Apply(query, resource);
  if (!IsNarrower(bounded_query, query)) {
    throw PdalError(ErrorClass::kForbidden,
                    "security/privacy hooks may only narrow a data query");
  }
  ValidateDataQuery(query);
  const auto negotiated = representations_.Negotiate(resource,
                                                       query.representation);

  audit_->Write(AuditEvent("data_query_routed", query.request_id,
                           {{"resource_id", query.resource},
                            {"operation", OperationName(query.operation)},
                            {"resource_resolution_latency_us",
                             static_cast<std::uint64_t>(
                                 std::chrono::duration_cast<std::chrono::microseconds>(
                                     resolution_completed - resolution_started).count())},
                            {"planning_latency_us", static_cast<std::uint64_t>(
                                 std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() -
                                     resolution_completed).count())},
                            {"control_latency_us", static_cast<std::uint64_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - started).count())}}));

  if (query.operation == Operation::kLatest) {
    if (!resource.live_binding) {
      throw PdalError(ErrorClass::kNotSupported,
                      "resource has no live binding");
    }
    std::optional<DataSample> sample;
    try {
      sample = live_sources_->Get(resource.live_binding->backend_id)
                   .Latest(resource, query.representation);
    } catch (const PdalError&) {
      throw;
    } catch (const std::exception&) {
      throw PdalError(ErrorClass::kBackendUnavailable,
                      "live source failed while reading the latest sample");
    }
    if (!sample) throw PdalError(ErrorClass::kNoData, "no live sample is available");
    audit_->Write(AuditEvent(
        "latest_sample_ready", query.request_id,
        {{"setup_latency_us", static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::steady_clock::now() - started).count())}}));
    auto state = std::make_shared<std::optional<DataSample>>(std::move(sample));
    return {query.request_id, query.resource, query.operation, negotiated.format,
            std::nullopt,
            DataStream([state]() mutable {
              if (!*state) return std::optional<DataSample>{};
              auto value = std::move(**state);
              state->reset();
              return std::optional<DataSample>(std::move(value));
            })};
  }

  if (query.operation == Operation::kSubscribe) {
    if (!resource.live_binding) {
      throw PdalError(ErrorClass::kNotSupported,
                      "resource has no live binding");
    }
    DataStream source;
    try {
      source = live_sources_->Get(resource.live_binding->backend_id)
                   .Subscribe(resource, query.representation, query.options);
    } catch (const PdalError&) {
      throw;
    } catch (const std::exception&) {
      throw PdalError(ErrorClass::kBackendUnavailable,
                      "live source failed while creating a subscription");
    }
    struct LiveState {
      LiveState(DataStream data_stream, std::shared_ptr<AuditSink> audit_sink,
                std::string request, SamplingSpec sample_policy)
          : stream(std::move(data_stream)), audit(std::move(audit_sink)),
            request_id(std::move(request)), sampling(std::move(sample_policy)),
            started(std::chrono::steady_clock::now()) {
        if (sampling.max_frequency_hz) {
          minimum_period_ns = static_cast<std::uint64_t>(
              1000000000.0 / *sampling.max_frequency_hz);
        }
      }
      DataStream stream;
      std::shared_ptr<AuditSink> audit;
      std::string request_id;
      SamplingSpec sampling;
      std::chrono::steady_clock::time_point started;
      std::uint64_t seen = 0;
      std::uint64_t last_timestamp_ns = 0;
      std::uint64_t minimum_period_ns = 0;
      std::uint64_t delivered = 0;
      std::uint64_t dropped = 0;
      std::uint64_t bytes = 0;
      std::atomic_bool released{false};
      void Release() {
        if (released.exchange(true)) return;
        stream.Cancel();
        const auto elapsed_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count());
        audit->Write(AuditEvent(
            "live_subscription_completed", request_id,
            {{"samples_delivered", delivered}, {"samples_dropped", dropped},
             {"bytes_returned", bytes},
             {"sample_rate_hz", elapsed_us > 0
                 ? static_cast<double>(delivered) * 1000000.0 / elapsed_us
                 : 0.0},
             {"stream_latency_us", elapsed_us}}));
      }
      ~LiveState() { Release(); }
    };
    auto state = std::make_shared<LiveState>(std::move(source), audit_,
                                             query.request_id,
                                             query.options.sampling);
    audit_->Write(AuditEvent(
        "live_subscription_ready", query.request_id,
        {{"setup_latency_us", static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::steady_clock::now() - started).count())}}));
    return {query.request_id, query.resource, query.operation, negotiated.format,
            std::nullopt,
            DataStream(
                [state]() -> std::optional<DataSample> {
                  try {
                    for (;;) {
                      auto sample = state->stream.Next();
                      if (!sample) {
                        state->Release();
                        return std::nullopt;
                      }
                      if (const auto* dropped = sample->metadata.if_contains(
                              "samples_dropped")) {
                        state->dropped += dropped->to_number<std::uint64_t>();
                      }
                      const bool every_n =
                          state->seen++ % state->sampling.every_n == 0;
                      const bool frequency = state->minimum_period_ns == 0 ||
                          state->last_timestamp_ns == 0 ||
                          (sample->timestamp_ns >= state->last_timestamp_ns &&
                           sample->timestamp_ns - state->last_timestamp_ns >=
                               state->minimum_period_ns);
                      if (!every_n || !frequency) continue;
                      state->last_timestamp_ns = sample->timestamp_ns;
                      ++state->delivered;
                      state->bytes += sample->payload ? sample->payload->size() : 0;
                      return sample;
                    }
                  } catch (const PdalError&) {
                    state->Release();
                    throw;
                  } catch (const std::exception&) {
                    state->Release();
                    throw PdalError(ErrorClass::kBackendUnavailable,
                                    "live subscription failed");
                  }
                },
                [state] { state->Release(); })};
  }

  if (resource.historical_binding.backend_id.empty()) {
    throw PdalError(ErrorClass::kNotSupported,
                    "resource has no historical binding");
  }
  const bool is_bulk = negotiated.format != "metadata" &&
                       (resource.modality == "image" ||
                        resource.modality == "lidar");
  if (is_bulk) bulk_gate_->Acquire();
  const StorageBackend* backend = nullptr;
  ExecutionTask task{query.resource, resource.historical_binding.backend_id,
                     *query.selector.time_range, query.representation};
  std::unique_ptr<HistoryCursor> cursor;
  std::uint64_t scan_limit = query.options.max_records;
  if (query.options.sampling.every_n > 1) {
    constexpr std::uint64_t kMaximumScanRecords = 100000;
    scan_limit = query.options.max_records >
                         kMaximumScanRecords / query.options.sampling.every_n
                     ? kMaximumScanRecords
                     : std::min<std::uint64_t>(
                           kMaximumScanRecords,
                           query.options.max_records *
                               query.options.sampling.every_n);
  }
  if (query.options.sampling.max_frequency_hz) scan_limit = 100000;
  try {
    const auto lookup_started = std::chrono::steady_clock::now();
    backend = &historical_backends_->Get(
        resource.historical_binding.backend_id);
    backend->Resolve(resource);
    cursor = backend->OpenHistory({task, 0, scan_limit}, catalog_);
    audit_->Write(AuditEvent(
        "history_lookup_completed", query.request_id,
        {{"records_considered", cursor->records_considered()},
         {"avs_lookup_latency_us", static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - lookup_started).count())}}));
  } catch (const PdalError&) {
    if (is_bulk) bulk_gate_->Release();
    throw;
  } catch (const std::exception&) {
    if (is_bulk) bulk_gate_->Release();
    throw PdalError(ErrorClass::kBackendUnavailable,
                    "historical backend query failed");
  }
  struct HistoryState {
    HistoryState(std::unique_ptr<HistoryCursor> history_cursor,
                 const StorageBackend* history_backend,
                 std::shared_ptr<BulkGate> bulk_gate,
                 std::shared_ptr<AuditSink> audit_sink,
                 std::string request,
                 std::string id, std::string format, std::string media_type,
                 std::uint64_t byte_limit, std::uint64_t record_limit,
                 SamplingSpec sample_policy, bool only_metadata)
        : cursor(std::move(history_cursor)),
          backend(history_backend),
          gate(std::move(bulk_gate)),
          audit(std::move(audit_sink)),
          request_id(std::move(request)),
          resource_id(std::move(id)),
          representation(std::move(format)),
          content_type(std::move(media_type)),
          max_bytes(byte_limit),
          max_records(record_limit),
          sampling(std::move(sample_policy)),
          metadata_only(only_metadata),
          stream_started(std::chrono::steady_clock::now()) {
      if (sampling.max_frequency_hz) {
        minimum_period_ns = static_cast<std::uint64_t>(
            1000000000.0 / *sampling.max_frequency_hz);
      }
    }
    std::unique_ptr<HistoryCursor> cursor;
    const StorageBackend* backend = nullptr;
    std::shared_ptr<BulkGate> gate;
    std::shared_ptr<AuditSink> audit;
    std::string request_id;
    std::string resource_id;
    std::string representation;
    std::string content_type;
    std::uint64_t max_bytes = 0;
    std::uint64_t max_records = 0;
    SamplingSpec sampling;
    bool metadata_only = false;
    std::uint64_t returned_bytes = 0;
    std::uint64_t records_returned = 0;
    std::uint64_t records_seen = 0;
    std::uint64_t last_timestamp_ns = 0;
    std::uint64_t minimum_period_ns = 0;
    std::uint64_t storage_read_latency_us = 0;
    boost::json::object tier_bytes;
    std::atomic_bool released{false};
    std::chrono::steady_clock::time_point stream_started;
    void Release() {
      if (!released.exchange(true)) {
        if (gate) gate->Release();
        audit->Write(AuditEvent(
            "history_stream_completed", request_id,
            {{"records_returned", records_returned},
             {"bytes_read", returned_bytes},
             {"bytes_returned", metadata_only ? 0 : returned_bytes},
             {"bytes_by_tier", tier_bytes},
             {"storage_read_latency_us", storage_read_latency_us},
             {"representation_latency_us", 0},
             {"stream_latency_us", static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - stream_started).count())}}));
      }
    }
    ~HistoryState() { Release(); }
  };
  auto state = std::make_shared<HistoryState>(
      std::move(cursor), backend, is_bulk ? bulk_gate_ : nullptr, audit_,
      query.request_id,
      query.resource, negotiated.format, ContentType(resource, negotiated.format),
      query.options.max_bytes, query.options.max_records, query.options.sampling,
      negotiated.format == "metadata");
  DataStream stream(
      [state]() -> std::optional<DataSample> {
        try {
          std::optional<BackendRecord> record;
          for (;;) {
            if (state->records_returned >= state->max_records) {
              state->Release();
              return std::nullopt;
            }
            record = state->cursor->Next();
            if (!record) {
              state->Release();
              return std::nullopt;
            }
            const bool every_n =
                state->records_seen++ % state->sampling.every_n == 0;
            const bool frequency = state->minimum_period_ns == 0 ||
                state->last_timestamp_ns == 0 ||
                (record->timestamp_ns >= state->last_timestamp_ns &&
                 record->timestamp_ns - state->last_timestamp_ns >=
                     state->minimum_period_ns);
            if (every_n && frequency) break;
          }
          if (!state->metadata_only &&
              (state->returned_bytes > state->max_bytes ||
               record->payload_size > state->max_bytes - state->returned_bytes)) {
            state->Release();
            return std::nullopt;
          }
          std::shared_ptr<const std::vector<std::uint8_t>> payload =
              std::make_shared<const std::vector<std::uint8_t>>();
          if (!state->metadata_only) {
            const auto read_started = std::chrono::steady_clock::now();
            payload = state->backend->ReadPayload(*record);
            state->storage_read_latency_us += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - read_started).count());
            if (!payload || payload->size() != record->payload_size) {
              throw PdalError(ErrorClass::kPartialRead,
                              "historical backend returned an incomplete record",
                              {{"resource_id", state->resource_id},
                               {"timestamp_ns", record->timestamp_ns}});
            }
            state->returned_bytes += payload->size();
            const auto* existing = state->tier_bytes.if_contains(record->storage_tier);
            state->tier_bytes[record->storage_tier] =
                (existing ? existing->to_number<std::uint64_t>() : 0) +
                payload->size();
          }
          ++state->records_returned;
          state->last_timestamp_ns = record->timestamp_ns;
          return DataSample{state->resource_id, record->timestamp_ns,
                            state->representation, state->content_type,
                            std::move(payload),
                            {{"payload_size", record->payload_size}}};
        } catch (const PdalError&) {
          state->Release();
          throw;
        } catch (const std::exception&) {
          state->Release();
          throw PdalError(ErrorClass::kBackendUnavailable,
                          "historical stream failed");
        }
      },
      [state] { state->Release(); });
  return {query.request_id, query.resource, query.operation, negotiated.format,
          std::nullopt, std::move(stream)};
}

}  // namespace pdal
