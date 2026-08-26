#include "pdal/model/data.h"

#include <atomic>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "pdal/adapter/request_translator.h"
#include "pdal/model/error.h"

namespace pdal {

struct DataStream::State {
  State(NextFunction next_function, CancelFunction cancel_function)
      : next(std::move(next_function)), cancel(std::move(cancel_function)) {}
  NextFunction next;
  CancelFunction cancel;
  std::atomic_bool cancelled{false};
};

DataStream::DataStream(NextFunction next, CancelFunction cancel)
    : state_(std::make_shared<State>(std::move(next), std::move(cancel))) {}

DataStream::~DataStream() { Cancel(); }

DataStream::Iterator::Iterator(DataStream* stream) : stream_(stream) {
  if (stream_) sample_ = stream_->Next();
}

DataStream::Iterator& DataStream::Iterator::operator++() {
  sample_ = stream_ ? stream_->Next() : std::nullopt;
  return *this;
}

std::optional<DataSample> DataStream::Next() {
  if (!state_ || state_->cancelled.load()) return std::nullopt;
  if (!state_->next) return std::nullopt;
  return state_->next();
}

void DataStream::Cancel() {
  if (!state_ || state_->cancelled.exchange(true)) return;
  if (state_->cancel) state_->cancel();
}

void ValidateDataQuery(const DataQuery& query) {
  if (query.request_id.empty() || query.request_id.size() > 128) {
    throw PdalError(ErrorClass::kInvalidQuery,
                    "request_id is empty or too long");
  }
  if (query.resource.empty() || query.resource.size() > 128) {
    throw PdalError(ErrorClass::kInvalidQuery,
                    "resource identifier is empty or too long");
  }
  if (query.operation == Operation::kHistory) {
    if (!query.selector.time_range || query.selector.time_range->start_ns == 0 ||
        query.selector.time_range->end_ns == 0 ||
        query.selector.time_range->end_ns < query.selector.time_range->start_ns) {
      throw PdalError(ErrorClass::kInvalidQuery,
                      "HISTORY requires a valid inclusive time range");
    }
  } else if (query.selector.time_range) {
    throw PdalError(ErrorClass::kInvalidQuery,
                    "only HISTORY accepts a time-range selector");
  }
  if (query.operation == Operation::kDiscover ||
      query.operation == Operation::kDescribe) {
    throw PdalError(ErrorClass::kInvalidQuery,
                    "DISCOVER and DESCRIBE use catalog operations, not data execution");
  }
  if (query.options.max_records == 0 || query.options.max_records > 100000) {
    throw PdalError(ErrorClass::kQueryTooLarge,
                    "max_records is outside the supported range");
  }
  if (query.options.max_bytes == 0 ||
      query.options.max_bytes > 1024ULL * 1024ULL * 1024ULL) {
    throw PdalError(ErrorClass::kQueryTooLarge,
                    "max_bytes is outside the supported range");
  }
  if (query.options.continuation_token &&
      (query.operation != Operation::kHistory ||
       query.options.continuation_token->size() > 4096)) {
    throw PdalError(ErrorClass::kInvalidQuery,
                    "continuation is invalid for this data query");
  }
  if (query.options.sampling.every_n == 0 ||
      (query.options.sampling.max_frequency_hz &&
       (!std::isfinite(*query.options.sampling.max_frequency_hz) ||
        *query.options.sampling.max_frequency_hz <= 0.0))) {
    throw PdalError(ErrorClass::kInvalidQuery,
                    "sampling values must be finite and positive");
  }
  if (query.representation.format.empty()) {
    throw PdalError(ErrorClass::kInvalidQuery,
                    "representation format is required");
  }
}

PdalRequest ToPdalRequest(const DataQuery& query) {
  ValidateDataQuery(query);
  PdalRequest request;
  request.request_id = query.request_id;
  request.principal = query.context.principal;
  request.purpose = query.context.purpose.empty() ? "development" : query.context.purpose;
  request.resources = {query.resource};
  if (query.selector.time_range) request.time = *query.selector.time_range;
  request.representation = query.representation;
  request.representation.sampling = query.options.sampling;
  request.delivery.max_records = query.options.max_records;
  request.delivery.max_bytes = query.options.max_bytes;
  request.delivery.continuation_token = query.options.continuation_token;
  request.context = query.context.extensions;
  return request;
}

DataQuery ToDataQuery(const PdalRequest& request) {
  if (request.resources.size() != 1) {
    throw PdalError(ErrorClass::kInvalidRequest,
                    "a DataQuery addresses exactly one logical resource");
  }
  DataQuery query;
  query.request_id = request.request_id;
  query.resource = request.resources.front();
  query.operation = Operation::kHistory;
  query.selector.time_range = request.time;
  query.representation = request.representation;
  query.options.sampling = request.representation.sampling;
  query.options.max_records = request.delivery.max_records;
  query.options.max_bytes = request.delivery.max_bytes;
  query.options.continuation_token = request.delivery.continuation_token;
  query.context = {request.principal, request.purpose, request.context};
  ValidateDataQuery(query);
  return query;
}

bool SemanticallyEquivalent(const DataQuery& left, const DataQuery& right) {
  return left.resource == right.resource && left.operation == right.operation &&
         left.selector.time_range == right.selector.time_range &&
         left.representation == right.representation &&
         left.options.sampling == right.options.sampling &&
         left.options.max_records == right.options.max_records &&
         left.options.max_bytes == right.options.max_bytes &&
         left.options.continuation_token == right.options.continuation_token &&
         left.context.principal == right.context.principal &&
         left.context.purpose == right.context.purpose &&
         left.context.extensions == right.context.extensions;
}

}  // namespace pdal
