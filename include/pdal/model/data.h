#pragma once

#include <boost/json/object.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pdal/model/types.h"

namespace pdal {

struct Selector {
  std::optional<TimeRange> time_range;
};

struct QueryOptions {
  SamplingSpec sampling;
  std::uint64_t max_records = 100;
  std::uint64_t max_bytes = 64ULL * 1024ULL * 1024ULL;
  std::optional<std::string> continuation_token;
};

struct RequestContext {
  Principal principal;
  std::string purpose;
  boost::json::object extensions;
};

struct DataQuery {
  std::string request_id;
  std::string resource;
  Operation operation = Operation::kHistory;
  Selector selector;
  RepresentationRequest representation;
  QueryOptions options;
  RequestContext context;
};

struct DataSample {
  std::string resource_id;
  std::uint64_t timestamp_ns = 0;
  std::string representation;
  std::string content_type;
  std::shared_ptr<const std::vector<std::uint8_t>> payload;
  boost::json::object metadata;
};

class DataStream {
 public:
  using NextFunction = std::function<std::optional<DataSample>()>;
  using CancelFunction = std::function<void()>;

  DataStream() = default;
  DataStream(NextFunction next, CancelFunction cancel = {});
  DataStream(DataStream&&) noexcept = default;
  DataStream& operator=(DataStream&&) noexcept = default;
  DataStream(const DataStream&) = delete;
  DataStream& operator=(const DataStream&) = delete;
  ~DataStream();

  class Iterator {
   public:
    using iterator_category = std::input_iterator_tag;
    using value_type = DataSample;
    using difference_type = std::ptrdiff_t;

    Iterator() = default;
    explicit Iterator(DataStream* stream);
    const DataSample& operator*() const { return *sample_; }
    const DataSample* operator->() const { return &*sample_; }
    Iterator& operator++();
    void operator++(int) { ++*this; }
    friend bool operator==(const Iterator& iterator,
                           std::default_sentinel_t) {
      return !iterator.sample_.has_value();
    }

   private:
    DataStream* stream_ = nullptr;
    std::optional<DataSample> sample_;
  };

  std::optional<DataSample> Next();
  void Cancel();
  Iterator begin() { return Iterator(this); }
  std::default_sentinel_t end() const { return {}; }
  explicit operator bool() const { return static_cast<bool>(state_); }

 private:
  struct State;
  std::shared_ptr<State> state_;
};

struct DataResult {
  std::string request_id;
  std::string resource_id;
  Operation operation = Operation::kHistory;
  std::string representation;
  std::optional<std::string> continuation_token;
  DataStream stream;
};

void ValidateDataQuery(const DataQuery& query);
PdalRequest ToPdalRequest(const DataQuery& query);
DataQuery ToDataQuery(const PdalRequest& request);
bool SemanticallyEquivalent(const DataQuery& left, const DataQuery& right);

}  // namespace pdal
