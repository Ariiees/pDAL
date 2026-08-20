#include "pdal/audit/audit.h"

#include <boost/json/serialize.hpp>

#include <stdexcept>

#include "pdal/model/types.h"

namespace pdal {

JsonLinesAuditSink::JsonLinesAuditSink(const std::filesystem::path& path)
    : output_(path, std::ios::app) {
  if (!output_) throw std::runtime_error("cannot open audit log: " + path.string());
}

void JsonLinesAuditSink::Write(const boost::json::object& event) {
  std::lock_guard lock(mutex_);
  output_ << boost::json::serialize(event) << '\n';
  output_.flush();
}

boost::json::object AuditEvent(const std::string& name,
                               const std::string& request_id,
                               boost::json::object details) {
  return {{"event", name},
          {"request_id", request_id},
          {"timestamp_ns", SystemNowNs()},
          {"details", std::move(details)}};
}

}  // namespace pdal
