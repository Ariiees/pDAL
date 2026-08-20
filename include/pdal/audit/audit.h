#pragma once

#include <boost/json/object.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

namespace pdal {

class AuditSink {
 public:
  virtual ~AuditSink() = default;
  virtual void Write(const boost::json::object& event) = 0;
};

class JsonLinesAuditSink final : public AuditSink {
 public:
  explicit JsonLinesAuditSink(const std::filesystem::path& path);
  void Write(const boost::json::object& event) override;

 private:
  std::mutex mutex_;
  std::ofstream output_;
};

class NullAuditSink final : public AuditSink {
 public:
  void Write(const boost::json::object&) override {}
};

boost::json::object AuditEvent(const std::string& name,
                               const std::string& request_id,
                               boost::json::object details = {});

}  // namespace pdal
