#include "pdal/adapter/request_translator.h"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <sstream>
#include <string_view>

#include "pdal/model/error.h"

namespace pdal {
namespace {

const boost::json::object& RequireObject(const boost::json::value& value,
                                         std::string_view field = "body") {
  if (!value.is_object()) {
    throw PdalError(ErrorClass::kInvalidRequest,
                    std::string(field) + " must be an object");
  }
  return value.as_object();
}

const boost::json::value& RequireField(const boost::json::object& object,
                                       std::string_view field) {
  const auto* value = object.if_contains(field);
  if (!value) {
    throw PdalError(ErrorClass::kInvalidRequest,
                    "missing required field: " + std::string(field),
                    {{"field", field}});
  }
  return *value;
}

std::string StringValue(const boost::json::value& value,
                        const std::string& field) {
  if (!value.is_string()) {
    throw PdalError(ErrorClass::kInvalidRequest, field + " must be a string");
  }
  return std::string(value.as_string());
}

std::uint64_t UintValue(const boost::json::value& value,
                        const std::string& field) {
  if (value.is_uint64()) return value.as_uint64();
  if (value.is_int64() && value.as_int64() >= 0) {
    return static_cast<std::uint64_t>(value.as_int64());
  }
  if (value.is_string()) {
    const std::string text(value.as_string());
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) return parsed;
  }
  throw PdalError(ErrorClass::kInvalidRequest,
                  field + " must be an unsigned integer");
}

std::optional<std::string> Header(const ExternalHeaders& headers,
                                  const std::string& name) {
  auto found = headers.find(name);
  if (found != headers.end()) return found->second;
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  found = headers.find(lower);
  if (found != headers.end()) return found->second;
  return std::nullopt;
}

Principal PrincipalFromHeaders(const ExternalHeaders& headers) {
  Principal principal;
  principal.principal_id = Header(headers, "x-pdal-principal").value_or("");
  principal.organization = Header(headers, "x-pdal-organization").value_or("");
  principal.role = Header(headers, "x-pdal-role").value_or("");
  return principal;
}

RepresentationRequest ParseRepresentation(const boost::json::object& body) {
  RepresentationRequest representation;
  const auto* value = body.if_contains("representation");
  if (!value) return representation;
  const auto& object = RequireObject(*value, "representation");
  if (const auto* format = object.if_contains("format")) {
    representation.format = StringValue(*format, "representation.format");
  }
  if (const auto* quality = object.if_contains("quality")) {
    const auto parsed = UintValue(*quality, "representation.quality");
    if (parsed > 100) {
      throw PdalError(ErrorClass::kInvalidRequest,
                      "representation.quality must be between 0 and 100");
    }
    representation.quality = static_cast<int>(parsed);
  }
  if (const auto* sampling = object.if_contains("sampling")) {
    const auto& sampling_object = RequireObject(*sampling, "representation.sampling");
    if (const auto* every = sampling_object.if_contains("every_n")) {
      const auto parsed = UintValue(*every, "representation.sampling.every_n");
      if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw PdalError(ErrorClass::kInvalidRequest, "sampling.every_n is too large");
      }
      representation.sampling.every_n = static_cast<std::uint32_t>(parsed);
    }
    if (const auto* frequency = sampling_object.if_contains("max_frequency_hz")) {
      if (!frequency->is_double() && !frequency->is_int64() && !frequency->is_uint64()) {
        throw PdalError(ErrorClass::kInvalidRequest,
                        "sampling.max_frequency_hz must be numeric");
      }
      representation.sampling.max_frequency_hz = frequency->to_number<double>();
    }
  }
  if (const auto* transformations = object.if_contains("transformations")) {
    if (!transformations->is_array()) {
      throw PdalError(ErrorClass::kInvalidRequest,
                      "representation.transformations must be an array");
    }
    for (const auto& transformation : transformations->as_array()) {
      representation.transformations.push_back(
          StringValue(transformation, "representation.transformations[]"));
    }
  }
  return representation;
}

DeliverySpec ParseDelivery(const boost::json::object& body) {
  DeliverySpec delivery;
  const auto* value = body.if_contains("delivery");
  if (!value) return delivery;
  const auto& object = RequireObject(*value, "delivery");
  if (const auto* mode = object.if_contains("mode")) {
    const auto parsed = StringValue(*mode, "delivery.mode");
    if (parsed == "stream") delivery.mode = DeliveryMode::kStream;
    else if (parsed == "metadata") delivery.mode = DeliveryMode::kMetadata;
    else throw PdalError(ErrorClass::kInvalidRequest,
                         "delivery.mode must be stream or metadata");
  }
  if (const auto* records = object.if_contains("max_records")) {
    delivery.max_records = UintValue(*records, "delivery.max_records");
  }
  if (const auto* bytes = object.if_contains("max_bytes")) {
    delivery.max_bytes = UintValue(*bytes, "delivery.max_bytes");
  }
  if (const auto* token = object.if_contains("continuation_token")) {
    delivery.continuation_token = StringValue(*token, "delivery.continuation_token");
  }
  return delivery;
}

std::uint64_t ParseCliUint(const std::string& text, const std::string& option) {
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    throw PdalError(ErrorClass::kInvalidRequest,
                    option + " requires an unsigned integer");
  }
  return value;
}

}  // namespace

PdalRequest NativeHttpAdapter::ToCanonicalRequest(
    const boost::json::value& external, const ExternalHeaders& headers) const {
  const auto& body = RequireObject(external);
  PdalRequest request;
  request.request_id = GenerateRequestId();
  request.principal = PrincipalFromHeaders(headers);
  if (const auto* version = body.if_contains("api_version")) {
    request.api_version = StringValue(*version, "api_version");
  }
  if (const auto* request_id = body.if_contains("request_id")) {
    request.request_id = StringValue(*request_id, "request_id");
  }
  request.purpose = StringValue(RequireField(body, "purpose"), "purpose");
  const auto& resources = RequireField(body, "resources");
  if (!resources.is_array()) {
    throw PdalError(ErrorClass::kInvalidRequest, "resources must be an array");
  }
  for (const auto& resource : resources.as_array()) {
    request.resources.push_back(StringValue(resource, "resources[]"));
  }
  const auto& time = RequireObject(RequireField(body, "time"), "time");
  request.time.start_ns = UintValue(RequireField(time, "start_ns"), "time.start_ns");
  request.time.end_ns = UintValue(RequireField(time, "end_ns"), "time.end_ns");
  request.representation = ParseRepresentation(body);
  request.delivery = ParseDelivery(body);
  if (const auto* context = body.if_contains("context")) {
    request.context = RequireObject(*context, "context");
  }
  ValidateRequest(request);
  return request;
}

DataQuery NativeHttpAdapter::ToDataQuery(
    const boost::json::value& external, Operation operation,
    const ExternalHeaders& headers) const {
  if (operation == Operation::kHistory) {
    auto query = pdal::ToDataQuery(ToCanonicalRequest(external, headers));
    query.operation = operation;
    return query;
  }
  const auto& input = RequireObject(external);
  boost::json::object compatibility;
  if (const auto* resource = input.if_contains("resource")) {
    compatibility["resources"] = boost::json::array{*resource};
  } else if (const auto* resources = input.if_contains("resources")) {
    compatibility["resources"] = *resources;
  } else {
    throw PdalError(ErrorClass::kInvalidRequest,
                    "resource is required");
  }
  compatibility["time"] = boost::json::object{{"start_ns", 1}, {"end_ns", 1}};
  compatibility["purpose"] = input.if_contains("purpose")
                                  ? *input.if_contains("purpose")
                                  : boost::json::value("development");
  if (const auto* representation = input.if_contains("representation")) {
    compatibility["representation"] = *representation;
  }
  if (const auto* options = input.if_contains("options")) {
    compatibility["delivery"] = *options;
  } else if (const auto* delivery = input.if_contains("delivery")) {
    compatibility["delivery"] = *delivery;
  }
  auto query = pdal::ToDataQuery(ToCanonicalRequest(compatibility, headers));
  query.operation = operation;
  query.selector.time_range.reset();
  ValidateDataQuery(query);
  return query;
}

PdalRequest SovdAdapter::ToCanonicalRequest(
    const boost::json::value& external, const ExternalHeaders& headers) const {
  const auto& body = RequireObject(external);
  boost::json::array resources;
  if (const auto* value = body.if_contains("resource")) {
    resources.emplace_back(StringValue(*value, "resource"));
  } else if (const auto* values = body.if_contains("resources")) {
    if (!values->is_array()) {
      throw PdalError(ErrorClass::kInvalidRequest, "resources must be an array");
    }
    resources = values->as_array();
  } else {
    throw PdalError(ErrorClass::kInvalidRequest, "missing required field: resource");
  }
  const auto& range = RequireObject(RequireField(body, "timeRange"), "timeRange");
  boost::json::object native{
      {"resources", std::move(resources)},
      {"time", boost::json::object{{"start_ns", RequireField(range, "from")},
                                    {"end_ns", RequireField(range, "to")}}},
      {"purpose", RequireField(body, "reason")}};
  boost::json::object representation;
  if (const auto* format = body.if_contains("contentFormat")) representation["format"] = *format;
  native["representation"] = std::move(representation);
  boost::json::object delivery;
  if (const auto* limit = body.if_contains("limit")) delivery["max_records"] = *limit;
  if (const auto* bytes = body.if_contains("maxBytes")) delivery["max_bytes"] = *bytes;
  if (const auto* mode = body.if_contains("deliveryMode")) delivery["mode"] = *mode;
  if (const auto* token = body.if_contains("continuationToken")) {
    delivery["continuation_token"] = *token;
  }
  native["delivery"] = std::move(delivery);
  return NativeHttpAdapter().ToCanonicalRequest(native, headers);
}

DataQuery SovdAdapter::ToDataQuery(
    const boost::json::value& external, const ExternalHeaders& headers) const {
  return pdal::ToDataQuery(ToCanonicalRequest(external, headers));
}

PdalRequest LocalCliAdapter::ToCanonicalRequest(
    const std::vector<std::string>& arguments) const {
  PdalRequest request;
  request.request_id = GenerateRequestId();
  request.principal = {"local-service-tool", "local", "service", {}};
  std::size_t i = (!arguments.empty() && arguments.front() == "query") ? 1 : 0;
  auto value = [&](const std::string& option) -> const std::string& {
    if (++i >= arguments.size()) {
      throw PdalError(ErrorClass::kInvalidRequest, option + " requires a value");
    }
    return arguments[i];
  };
  for (; i < arguments.size(); ++i) {
    const auto& argument = arguments[i];
    if (argument == "--resource") request.resources.push_back(value(argument));
    else if (argument == "--start") request.time.start_ns = ParseCliUint(value(argument), argument);
    else if (argument == "--end") request.time.end_ns = ParseCliUint(value(argument), argument);
    else if (argument == "--purpose") request.purpose = value(argument);
    else if (argument == "--format") request.representation.format = value(argument);
    else if (argument == "--max-records") request.delivery.max_records = ParseCliUint(value(argument), argument);
    else if (argument == "--max-bytes") request.delivery.max_bytes = ParseCliUint(value(argument), argument);
    else if (argument == "--continuation") request.delivery.continuation_token = value(argument);
    else if (argument == "--principal") request.principal.principal_id = value(argument);
    else if (argument == "--organization") request.principal.organization = value(argument);
    else if (argument == "--role") request.principal.role = value(argument);
    else if (argument == "--metadata") request.delivery.mode = DeliveryMode::kMetadata;
    else throw PdalError(ErrorClass::kInvalidRequest, "unknown query option: " + argument);
  }
  ValidateRequest(request);
  return request;
}

DataQuery LocalCliAdapter::ToDataQuery(
    const std::vector<std::string>& arguments, Operation operation) const {
  auto query = pdal::ToDataQuery(ToCanonicalRequest(arguments));
  query.operation = operation;
  if (operation != Operation::kHistory) query.selector.time_range.reset();
  ValidateDataQuery(query);
  return query;
}

void ValidateRequest(const PdalRequest& request) {
  if (request.api_version != kApiVersion) {
    throw PdalError(ErrorClass::kInvalidRequest, "unsupported api_version",
                    {{"supported", kApiVersion}});
  }
  if (request.request_id.empty() || request.request_id.size() > 128) {
    throw PdalError(ErrorClass::kInvalidRequest, "request_id is empty or too long");
  }
  if (request.resources.empty() || request.resources.size() > 16) {
    throw PdalError(ErrorClass::kInvalidRequest,
                    "resources must contain between 1 and 16 entries");
  }
  std::set<std::string> unique;
  for (const auto& resource : request.resources) {
    if (resource.empty() || resource.size() > 128 || !unique.insert(resource).second) {
      throw PdalError(ErrorClass::kInvalidRequest,
                      "resource identifiers must be non-empty and unique");
    }
  }
  if (request.time.start_ns == 0 || request.time.end_ns == 0 ||
      request.time.end_ns < request.time.start_ns) {
    throw PdalError(ErrorClass::kInvalidRequest, "invalid inclusive time range");
  }
  if (request.purpose.empty() || request.purpose.size() > 128) {
    throw PdalError(ErrorClass::kInvalidRequest, "purpose is empty or too long");
  }
  if (request.representation.format.empty() ||
      request.representation.format.size() > 64) {
    throw PdalError(ErrorClass::kInvalidRequest, "invalid representation format");
  }
  if (request.representation.sampling.every_n == 0) {
    throw PdalError(ErrorClass::kInvalidRequest, "sampling.every_n must be positive");
  }
  if (request.representation.sampling.max_frequency_hz &&
      (!std::isfinite(*request.representation.sampling.max_frequency_hz) ||
       *request.representation.sampling.max_frequency_hz <= 0.0)) {
    throw PdalError(ErrorClass::kInvalidRequest,
                    "sampling.max_frequency_hz must be finite and positive");
  }
  if (request.delivery.max_records == 0 || request.delivery.max_records > 100000) {
    throw PdalError(ErrorClass::kInvalidRequest,
                    "delivery.max_records must be between 1 and 100000");
  }
  if (request.delivery.max_bytes == 0 ||
      request.delivery.max_bytes > 1024ULL * 1024ULL * 1024ULL) {
    throw PdalError(ErrorClass::kInvalidRequest,
                    "delivery.max_bytes must be between 1 and 1073741824");
  }
  if (request.delivery.continuation_token &&
      request.delivery.continuation_token->size() > 4096) {
    throw PdalError(ErrorClass::kInvalidRequest, "continuation token is too long");
  }
}

bool SemanticallyEquivalent(const PdalRequest& left,
                            const PdalRequest& right) {
  return left.api_version == right.api_version &&
         left.principal == right.principal && left.purpose == right.purpose &&
         left.resources == right.resources && left.time == right.time &&
         left.representation == right.representation &&
         left.delivery == right.delivery && left.context == right.context;
}

}  // namespace pdal
