#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "pdal/pipeline.h"

namespace pdal {

struct HttpServerConfig {
  std::string address = "127.0.0.1";
  std::uint16_t port = 8080;
  std::size_t max_body_bytes = 1024 * 1024;
  std::size_t max_connections = 8;
  std::string demo_html_path;
};

class HttpServer {
 public:
  HttpServer(HttpServerConfig config, std::shared_ptr<const PdalPipeline> pipeline);
  void Run();

 private:
  HttpServerConfig config_;
  std::shared_ptr<const PdalPipeline> pipeline_;
};

}  // namespace pdal
