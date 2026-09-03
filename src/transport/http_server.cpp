#include "pdal/transport/http_server.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/chunk_encode.hpp>
#include <boost/json/array.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
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

std::string UrlDecode(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '+') {
      out.push_back(' ');
    } else if (input[i] == '%' && i + 2 < input.size() &&
               std::isxdigit(static_cast<unsigned char>(input[i + 1])) &&
               std::isxdigit(static_cast<unsigned char>(input[i + 2]))) {
      const auto hex = [](char c) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return c <= '9' ? c - '0' : c - 'a' + 10;
      };
      out.push_back(static_cast<char>(hex(input[i + 1]) * 16 + hex(input[i + 2])));
      i += 2;
    } else {
      out.push_back(input[i]);
    }
  }
  return out;
}

// Returns the first value of a query-string parameter, or an empty string.
std::string QueryParam(beast::string_view target, std::string_view key) {
  const auto start = target.find('?');
  if (start == beast::string_view::npos) return {};
  std::string_view query(target.data() + start + 1, target.size() - start - 1);
  std::size_t pos = 0;
  while (pos < query.size()) {
    const auto amp = query.find('&', pos);
    const auto end = amp == std::string_view::npos ? query.size() : amp;
    const auto pair = query.substr(pos, end - pos);
    const auto eq = pair.find('=');
    if (eq != std::string_view::npos && pair.substr(0, eq) == key) {
      return UrlDecode(pair.substr(eq + 1));
    }
    if (amp == std::string_view::npos) break;
    pos = amp + 1;
  }
  return {};
}

// The verified identity always replaces anything a caller may have sent in the
// legacy X-PDAL-* identity headers, so they are dropped before translation.
void StripLegacyIdentityHeaders(ExternalHeaders& headers) {
  headers.erase("x-pdal-principal");
  headers.erase("x-pdal-organization");
  headers.erase("x-pdal-role");
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

boost::json::object SampleMetadata(const DataSample& sample) {
  boost::json::object metadata{{"resource_id", sample.resource_id},
                               {"timestamp_ns", sample.timestamp_ns},
                               {"representation", sample.representation},
                               {"content_type", sample.content_type}};
  for (const auto& [key, value] : sample.metadata) metadata[key] = value;
  if (!metadata.contains("payload_size")) {
    metadata["payload_size"] = sample.payload ? sample.payload->size() : 0;
  }
  return metadata;
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

void WriteDataStream(tcp::socket& socket, unsigned version,
                     DataResult result) {
  http::response<http::empty_body> response{http::status::ok, version};
  response.set(http::field::server, "pdal/1");
  response.set(http::field::content_type,
               "application/vnd.pdal.record-stream; version=1");
  response.set("X-PDAL-Request-ID", result.request_id);
  response.set("X-PDAL-Resource-ID", result.resource_id);
  response.set("X-PDAL-Operation", OperationName(result.operation));
  response.chunked(true);
  response.keep_alive(false);
  http::response_serializer<http::empty_body> serializer{response};
  beast::error_code error;
  http::write_header(socket, serializer, error);
  if (error) {
    result.stream.Cancel();
    return;
  }

  static constexpr char magic[] = "PDALSTR1";
  bool connected = WriteChunk(socket, magic, sizeof(magic) - 1);
  try {
    while (connected) {
      auto sample = result.stream.Next();
      if (!sample) break;
      const auto metadata = boost::json::serialize(SampleMetadata(*sample));
      const auto payload_size = sample->payload ? sample->payload->size() : 0;
      std::array<std::uint8_t, 12> header{};
      PutBigEndian(header.data(), metadata.size(), 4);
      PutBigEndian(header.data() + 4, payload_size, 8);
      connected = WriteChunk(socket, header.data(), header.size()) &&
                  WriteChunk(socket, metadata.data(), metadata.size());
      if (connected && payload_size > 0) {
        connected = WriteChunk(socket, sample->payload->data(), payload_size);
      }
    }
  } catch (const std::exception&) {
    connected = false;
  }
  result.stream.Cancel();
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

bool IsPublicEndpoint(http::verb method, const std::string& path) {
  return method == http::verb::get &&
         (path == "/pdal/v1" || path == "/pdal/v1/capabilities");
}

void HandleSession(tcp::socket socket, const HttpServerConfig& config,
                   const std::shared_ptr<const PdalPipeline>& pipeline,
                   const std::shared_ptr<const QueryEngine>& query_engine,
                   const std::shared_ptr<const Authenticator>& authenticator) {
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
  auto headers = ExtractHeaders(request);

  try {
    Principal principal;
    if (!IsPublicEndpoint(request.method(), path)) {
      principal = authenticator->Authenticate(headers);
    }
    StripLegacyIdentityHeaders(headers);

    if (request.method() == http::verb::get && path == "/pdal/v1") {
      WriteJson(socket, request.version(), http::status::ok,
                boost::json::object{{"api_version", kApiVersion},
                                    {"name", "pDAL Open Vehicle Data Access Layer"},
                                    {"capabilities", "/pdal/v1/capabilities"}});
    } else if (request.method() == http::verb::get &&
               path == "/pdal/v1/capabilities") {
      WriteJson(socket, request.version(), http::status::ok,
                pipeline->CapabilitiesJson());
    } else if (request.method() == http::verb::get &&
               path == "/pdal/v1/resources") {
      boost::json::array resources;
      for (const auto& resource : query_engine->Discover(request_id)) {
        resources.emplace_back(ResourceToJson(resource));
      }
      WriteJson(socket, request.version(), http::status::ok,
                boost::json::object{{"api_version", kApiVersion},
                                    {"resources", std::move(resources)}},
                request_id);
    } else if (path.starts_with("/pdal/v1/resources/")) {
      const auto suffix = path.substr(std::string("/pdal/v1/resources/").size());
      const auto separator = suffix.find('/');
      const auto resource_id = suffix.substr(0, separator);
      const auto operation = separator == std::string::npos
                                 ? std::string{}
                                 : suffix.substr(separator + 1);
      if (resource_id.empty() ||
          (separator != std::string::npos && operation.find('/') != std::string::npos)) {
        throw PdalError(ErrorClass::kResourceNotFound, "unknown logical resource");
      }
      if (request.method() == http::verb::get && operation.empty()) {
        WriteJson(socket, request.version(), http::status::ok,
                  ResourceToJson(query_engine->Describe(resource_id, request_id)),
                  request_id);
      } else if (request.method() == http::verb::post && operation == "history") {
        boost::json::value body;
        try {
          body = boost::json::parse(request.body());
        } catch (const std::exception&) {
          throw PdalError(ErrorClass::kInvalidRequest,
                          "request body is not valid JSON");
        }
        if (!body.is_object()) {
          throw PdalError(ErrorClass::kInvalidRequest,
                          "request body must be a JSON object");
        }
        auto& object = body.as_object();
        object["resources"] = boost::json::array{resource_id};
        auto query = NativeHttpAdapter().ToDataQuery(
            body, Operation::kHistory, headers);
        query.context.principal = principal;
        request_id = query.request_id;
        WriteDataStream(socket, request.version(),
                        query_engine->Execute(std::move(query)));
      } else if (request.method() == http::verb::get &&
                 (operation == "latest" || operation == "subscribe")) {
        const auto selected_operation = operation == "latest"
                                            ? Operation::kLatest
                                            : Operation::kSubscribe;
        auto purpose = QueryParam(request.target(), "purpose");
        if (purpose.empty()) purpose = "development";
        const boost::json::object body{{"resource", resource_id},
                                       {"purpose", purpose}};
        auto query = NativeHttpAdapter().ToDataQuery(
            body, selected_operation, headers);
        query.context.principal = principal;
        request_id = query.request_id;
        WriteDataStream(socket, request.version(),
                        query_engine->Execute(std::move(query)));
      } else {
        throw PdalError(ErrorClass::kResourceNotFound, "endpoint not found");
      }
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
      auto translated = path.starts_with("/sovd/")
                            ? SovdAdapter().ToCanonicalRequest(body, headers)
                            : NativeHttpAdapter().ToCanonicalRequest(body, headers);
      translated.principal = principal;
      // The compatible bulk envelope can contain several resources, while a
      // DataQuery intentionally addresses one. Every single-resource REST and
      // SOVD request still crosses the stable semantic contract before it
      // enters the retained protected bulk pipeline.
      auto canonical = translated;
      if (translated.resources.size() == 1) {
        canonical = ToPdalRequest(ToDataQuery(translated));
        canonical.api_version = translated.api_version;
        canonical.delivery.mode = translated.delivery.mode;
      }
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
                       std::shared_ptr<const PdalPipeline> pipeline,
                       std::shared_ptr<const QueryEngine> query_engine,
                       std::shared_ptr<const Authenticator> authenticator)
    : config_(std::move(config)),
      pipeline_(std::move(pipeline)),
      query_engine_(std::move(query_engine)),
      authenticator_(std::move(authenticator)) {
  if (!pipeline_ || !query_engine_ || !authenticator_) {
    throw std::invalid_argument(
        "HTTP server requires a pipeline, query engine, and authenticator");
  }
}

void HttpServer::Run() {
  asio::io_context context(1);
  const auto address = asio::ip::make_address(config_.address);
  tcp::acceptor acceptor(context, {address, config_.port});
  auto active = std::make_shared<std::atomic_size_t>(0);
  for (;;) {
    tcp::socket socket(context);
    beast::error_code accept_error;
    acceptor.accept(socket, accept_error);
    if (accept_error == asio::error::operation_aborted ||
        accept_error.value() == EINTR) {
      break;
    }
    if (accept_error) throw boost::system::system_error(accept_error);
    if (active->load() >= config_.max_connections) {
      WriteJson(socket, 11, http::status::service_unavailable,
                ErrorEnvelope(PdalError(ErrorClass::kBackendUnavailable,
                                        "server connection limit reached"),
                              GenerateRequestId()));
      continue;
    }
    ++*active;
    std::thread([socket = std::move(socket), config = config_, pipeline = pipeline_,
                 query_engine = query_engine_, authenticator = authenticator_,
                 active]() mutable {
      HandleSession(std::move(socket), config, pipeline, query_engine,
                    authenticator);
      --*active;
    }).detach();
  }
}

}  // namespace pdal
