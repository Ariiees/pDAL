#include "pdal/transport/http_server.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/chunk_encode.hpp>
#include <boost/json/array.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

#include "pdal/adapter/request_translator.h"
#include "pdal/model/error.h"
#include "pdal/model/json.h"

namespace pdal {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

std::string ReadTextFile(const std::string& path) {
  if (path.empty()) return {};
  std::ifstream input(path);
  if (!input) return {};
  std::ostringstream out;
  out << input.rdbuf();
  return out.str();
}

ExternalHeaders ExtractHeaders(const http::request<http::string_body>& request) {
  ExternalHeaders out;
  for (const auto& field : request) {
    std::string name(field.name_string());
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    out[std::move(name)] = std::string(field.value());
  }
  return out;
}

template <typename Body>
void WriteResponse(tcp::socket& socket, http::response<Body>& response) {
  response.set(http::field::server, "pdal/1");
  response.keep_alive(false);
  beast::error_code error;
  http::write(socket, response, error);
}

void WriteJson(tcp::socket& socket, unsigned version, http::status status,
               const boost::json::value& body, const std::string& request_id = {}) {
  http::response<http::string_body> response{status, version};
  response.set(http::field::content_type, "application/json");
  if (!request_id.empty()) response.set("X-PDAL-Request-ID", request_id);
  response.body() = boost::json::serialize(body);
  response.prepare_payload();
  WriteResponse(socket, response);
}

std::string TargetPath(beast::string_view target) {
  const auto query = target.find('?');
  return std::string(target.substr(0, query));
}

std::string ContentTypeFor(const PdalPipeline& pipeline,
                           const std::string& resource_id,
                           const std::string& format) {
  if (format == "metadata") return "application/json";
  for (const auto& representation : pipeline.catalog().Get(resource_id).representations) {
    if (representation.format == format) return representation.content_type;
  }
  return "application/octet-stream";
}

void PutBigEndian(std::uint8_t* output, std::uint64_t value, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) {
    output[bytes - i - 1] = static_cast<std::uint8_t>(value & 0xffU);
    value >>= 8U;
  }
}

bool WriteChunk(tcp::socket& socket, const void* data, std::size_t size) {
  beast::error_code error;
  asio::write(socket, http::make_chunk(asio::buffer(data, size)), error);
  return !error;
}

void WriteStream(tcp::socket& socket, unsigned version,
                 const PdalPipeline& pipeline, const PreparedQuery& prepared) {
  http::response<http::empty_body> response{http::status::ok, version};
  response.set(http::field::server, "pdal/1");
  response.set(http::field::content_type,
               "application/vnd.pdal.record-stream; version=1");
  response.set("X-PDAL-Request-ID", prepared.request.request_id);
  response.set("X-PDAL-Metadata",
               boost::json::serialize(ResponseMetadataToJson(prepared.metadata)));
  response.chunked(true);
  response.keep_alive(false);
  http::response_serializer<http::empty_body> serializer{response};
  beast::error_code error;
  http::write_header(socket, serializer, error);
  if (error) return;

  static constexpr char magic[] = "PDALSTR1";
  if (!WriteChunk(socket, magic, sizeof(magic) - 1)) return;
  bool connected = true;
  try {
    pipeline.Stream(prepared,
                    {[&](const BackendRecord& record) {
                       const auto format = prepared.metadata.representations.at(record.resource_id);
                       const auto metadata = boost::json::serialize(boost::json::object{
                           {"resource_id", record.resource_id},
                           {"timestamp_ns", record.timestamp_ns},
                           {"representation", format},
                           {"content_type", ContentTypeFor(pipeline, record.resource_id, format)},
                           {"payload_size", record.payload_size}});
                       std::array<std::uint8_t, 12> header{};
                       PutBigEndian(header.data(), metadata.size(), 4);
                       PutBigEndian(header.data() + 4, record.payload_size, 8);
                       connected = WriteChunk(socket, header.data(), header.size()) &&
                                   WriteChunk(socket, metadata.data(), metadata.size());
                       return connected;
                     },
                     [&](const std::uint8_t* data, std::size_t size) {
                       connected = connected && WriteChunk(socket, data, size);
                       return connected;
                     },
                     [&]() { return connected; }});
  } catch (const std::exception&) {
    connected = false;
  }
  if (connected) asio::write(socket, http::make_chunk_last(), error);
}

boost::json::object MetadataBody(const PreparedQuery& prepared) {
  auto body = ResponseMetadataToJson(prepared.metadata);
  boost::json::array records;
  for (const auto& record : prepared.result.records) {
    records.emplace_back(boost::json::object{{"resource_id", record.resource_id},
                                             {"timestamp_ns", record.timestamp_ns},
                                             {"payload_size", record.payload_size}});
  }
  body["records"] = std::move(records);
  return body;
}

void HandleSession(tcp::socket socket, const HttpServerConfig& config,
                   const std::shared_ptr<const PdalPipeline>& pipeline) {
  beast::flat_buffer buffer;
  http::request_parser<http::string_body> parser;
  parser.body_limit(config.max_body_bytes);
  beast::error_code error;
  http::read(socket, buffer, parser, error);
  if (error == http::error::body_limit) {
    const auto request_id = GenerateRequestId();
    const PdalError too_large(
        ErrorClass::kInvalidRequest, "request body exceeds the configured limit",
        {{"max_body_bytes", config.max_body_bytes}});
    WriteJson(socket, 11, http::status::payload_too_large,
              ErrorEnvelope(too_large, request_id), request_id);
    return;
  }
  if (error) return;
  auto request = parser.release();
  const auto path = TargetPath(request.target());
  std::string request_id = GenerateRequestId();

  try {
    if (request.method() == http::verb::get && path == "/pdal/v1") {
      WriteJson(socket, request.version(), http::status::ok,
                boost::json::object{{"api_version", kApiVersion},
                                    {"name", "Protected Data Access Layer"},
                                    {"capabilities", "/pdal/v1/capabilities"}});
    } else if (request.method() == http::verb::get &&
               path == "/pdal/v1/capabilities") {
      WriteJson(socket, request.version(), http::status::ok,
                pipeline->CapabilitiesJson());
    } else if (request.method() == http::verb::get &&
               path == "/pdal/v1/resources") {
      boost::json::array resources;
      for (const auto& resource : pipeline->catalog().List()) {
        resources.emplace_back(ResourceToJson(resource));
      }
      WriteJson(socket, request.version(), http::status::ok,
                boost::json::object{{"api_version", kApiVersion},
                                    {"resources", std::move(resources)}});
    } else if (request.method() == http::verb::get &&
               path.starts_with("/pdal/v1/resources/")) {
      const auto resource_id = path.substr(std::string("/pdal/v1/resources/").size());
      if (resource_id.empty() || resource_id.find('/') != std::string::npos) {
        throw PdalError(ErrorClass::kResourceNotFound, "unknown logical resource");
      }
      WriteJson(socket, request.version(), http::status::ok,
                ResourceToJson(pipeline->catalog().Get(resource_id)));
    } else if (request.method() == http::verb::get &&
               path.starts_with("/pdal/v1/requests/")) {
      const auto id = path.substr(std::string("/pdal/v1/requests/").size());
      const auto status = pipeline->registry().Get(id);
      if (!status) {
        throw PdalError(ErrorClass::kResourceNotFound, "request status not found");
      }
      WriteJson(socket, request.version(), http::status::ok,
                boost::json::object{{"api_version", kApiVersion},
                                    {"request_id", status->request_id},
                                    {"state", RequestStateName(status->state)},
                                    {"records_selected", status->records_selected},
                                    {"bytes_returned", status->bytes_returned},
                                    {"error_code", status->error_code}}, id);
    } else if (request.method() == http::verb::get && path == "/pdal/v1/demo") {
      const auto html = ReadTextFile(config.demo_html_path);
      if (html.empty()) throw PdalError(ErrorClass::kResourceNotFound, "demo page unavailable");
      http::response<http::string_body> response{http::status::ok, request.version()};
      response.set(http::field::content_type, "text/html; charset=utf-8");
      response.body() = html;
      response.prepare_payload();
      WriteResponse(socket, response);
    } else if (request.method() == http::verb::post &&
               (path == "/pdal/v1/query" || path == "/sovd/v1/bulk-data/query")) {
      boost::json::value body;
      try {
        body = boost::json::parse(request.body());
      } catch (const std::exception&) {
        throw PdalError(ErrorClass::kInvalidRequest, "request body is not valid JSON");
      }
      const auto headers = ExtractHeaders(request);
      auto canonical = path.starts_with("/sovd/")
                           ? SovdAdapter().ToCanonicalRequest(body, headers)
                           : NativeHttpAdapter().ToCanonicalRequest(body, headers);
      request_id = canonical.request_id;
      auto prepared = pipeline->Prepare(std::move(canonical));
      if (prepared.request.delivery.mode == DeliveryMode::kMetadata ||
          prepared.request.representation.format == "metadata" ||
          prepared.request.representation.format == "metadata-only") {
        pipeline->Stream(prepared, {});
        WriteJson(socket, request.version(), http::status::ok,
                  MetadataBody(prepared), request_id);
      } else {
        WriteStream(socket, request.version(), *pipeline, prepared);
      }
    } else {
      throw PdalError(ErrorClass::kResourceNotFound, "endpoint not found");
    }
  } catch (const PdalError& pdal_error) {
    WriteJson(socket, request.version(),
              static_cast<http::status>(HttpStatus(pdal_error.error_class())),
              ErrorEnvelope(pdal_error, request_id), request_id);
  } catch (const std::exception&) {
    const PdalError internal(ErrorClass::kInternalError,
                             "an internal error occurred");
    WriteJson(socket, request.version(), http::status::internal_server_error,
              ErrorEnvelope(internal, request_id), request_id);
  }
  socket.shutdown(tcp::socket::shutdown_send, error);
}

}  // namespace

HttpServer::HttpServer(HttpServerConfig config,
                       std::shared_ptr<const PdalPipeline> pipeline)
    : config_(std::move(config)), pipeline_(std::move(pipeline)) {
  if (!pipeline_) throw std::invalid_argument("HTTP server requires a pipeline");
}

void HttpServer::Run() {
  asio::io_context context(1);
  const auto address = asio::ip::make_address(config_.address);
  tcp::acceptor acceptor(context, {address, config_.port});
  std::atomic_size_t active = 0;
  for (;;) {
    tcp::socket socket(context);
    acceptor.accept(socket);
    if (active.load() >= config_.max_connections) {
      WriteJson(socket, 11, http::status::service_unavailable,
                ErrorEnvelope(PdalError(ErrorClass::kBackendUnavailable,
                                        "server connection limit reached"),
                              GenerateRequestId()));
      continue;
    }
    ++active;
    std::thread([socket = std::move(socket), config = config_, pipeline = pipeline_,
                 &active]() mutable {
      HandleSession(std::move(socket), config, pipeline);
      --active;
    }).detach();
  }
}

}  // namespace pdal
