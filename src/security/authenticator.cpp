#include "pdal/security/authenticator.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <string_view>

#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

#include "pdal/model/error.h"

namespace pdal {
namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

[[noreturn]] void Reject(const std::string& why) {
  // The public message stays generic; `why` is a safe, non-secret detail.
  throw PdalError(ErrorClass::kUnauthenticated, "credential is missing or invalid",
                  {{"reason", why}});
}

std::string Base64UrlDecode(std::string_view input) {
  std::array<int, 256> table{};
  table.fill(-1);
  for (int i = 0; i < 64; ++i) {
    table[static_cast<unsigned char>(kBase64Alphabet[i])] = i;
  }
  std::string out;
  out.reserve(input.size() * 3 / 4 + 2);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const unsigned char ch : input) {
    if (ch == '=') break;
    const int value = table[ch];
    if (value < 0) Reject("token is not valid base64url");
    buffer = (buffer << 6U) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xffU));
    }
  }
  return out;
}

std::array<unsigned char, 32> HmacSha256(const std::string& secret,
                                         std::string_view message) {
  std::array<unsigned char, 32> digest{};
  unsigned int length = 0;
  HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
       reinterpret_cast<const unsigned char*>(message.data()), message.size(),
       digest.data(), &length);
  if (length != digest.size()) {
    throw PdalError(ErrorClass::kInternalError, "token signature check failed");
  }
  return digest;
}

std::string ClaimString(const boost::json::object& claims, const char* name) {
  const auto* value = claims.if_contains(name);
  if (!value || !value->is_string()) return {};
  return std::string(value->as_string());
}

std::uint64_t NowSeconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

double ClaimNumber(const boost::json::object& claims, const char* name,
                   bool& present) {
  const auto* value = claims.if_contains(name);
  present = value != nullptr;
  if (!value) return 0.0;
  if (value->is_double()) return value->as_double();
  if (value->is_int64()) return static_cast<double>(value->as_int64());
  if (value->is_uint64()) return static_cast<double>(value->as_uint64());
  Reject(std::string(name) + " claim is not numeric");
}

bool AudienceMatches(const boost::json::object& claims,
                     const std::string& expected) {
  const auto* value = claims.if_contains("aud");
  if (!value) return false;
  if (value->is_string()) return value->as_string() == expected;
  if (value->is_array()) {
    for (const auto& entry : value->as_array()) {
      if (entry.is_string() && entry.as_string() == expected) return true;
    }
  }
  return false;
}

std::string HeaderValue(const RequestHeaders& headers, const std::string& name) {
  const auto found = headers.find(name);
  return found == headers.end() ? std::string{} : found->second;
}

}  // namespace

BearerTokenAuthenticator::BearerTokenAuthenticator(AuthConfig config)
    : config_(std::move(config)) {
  if (config_.algorithm != "HS256") {
    throw std::invalid_argument("only the HS256 token algorithm is supported");
  }
  if (config_.hmac_secret.size() < 16) {
    throw std::invalid_argument(
        "auth hmac_secret must resolve to at least 16 bytes");
  }
  if (config_.issuer.empty() || config_.audience.empty()) {
    throw std::invalid_argument("auth issuer and audience are required");
  }
}

Principal BearerTokenAuthenticator::Authenticate(
    const RequestHeaders& headers) const {
  const std::string authorization = HeaderValue(headers, "authorization");
  if (authorization.empty()) Reject("missing Authorization header");

  constexpr std::string_view kBearer = "Bearer ";
  if (authorization.size() <= kBearer.size() ||
      !std::equal(kBearer.begin(), kBearer.end(), authorization.begin(),
                  [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                  })) {
    Reject("Authorization scheme is not Bearer");
  }
  std::string token = authorization.substr(kBearer.size());
  while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
    token.pop_back();
  }

  const auto first_dot = token.find('.');
  const auto second_dot =
      first_dot == std::string::npos ? std::string::npos : token.find('.', first_dot + 1);
  if (first_dot == std::string::npos || second_dot == std::string::npos ||
      token.find('.', second_dot + 1) != std::string::npos) {
    Reject("token is not a compact JWS with three segments");
  }
  const std::string_view signing_input(token.data(), second_dot);
  const std::string header_json = Base64UrlDecode(token.substr(0, first_dot));
  const std::string payload_json =
      Base64UrlDecode(token.substr(first_dot + 1, second_dot - first_dot - 1));
  const std::string provided_signature = Base64UrlDecode(token.substr(second_dot + 1));

  boost::json::value header;
  try {
    header = boost::json::parse(header_json);
  } catch (const std::exception&) {
    Reject("token header is not valid JSON");
  }
  if (!header.is_object()) Reject("token header is not a JSON object");
  if (ClaimString(header.as_object(), "alg") != "HS256") {
    Reject("token algorithm is not HS256");
  }

  const auto expected_signature = HmacSha256(config_.hmac_secret, signing_input);
  if (provided_signature.size() != expected_signature.size() ||
      CRYPTO_memcmp(provided_signature.data(), expected_signature.data(),
                    expected_signature.size()) != 0) {
    Reject("token signature does not verify");
  }

  boost::json::value payload;
  try {
    payload = boost::json::parse(payload_json);
  } catch (const std::exception&) {
    Reject("token payload is not valid JSON");
  }
  if (!payload.is_object()) Reject("token payload is not a JSON object");
  const auto& claims = payload.as_object();

  if (ClaimString(claims, "iss") != config_.issuer) Reject("issuer mismatch");
  if (!AudienceMatches(claims, config_.audience)) Reject("audience mismatch");

  const auto now = NowSeconds();
  const auto skew = config_.clock_skew_s;

  bool has_exp = false;
  const double exp = ClaimNumber(claims, "exp", has_exp);
  if (!has_exp) Reject("token has no exp claim");
  if (exp + static_cast<double>(skew) < static_cast<double>(now)) {
    Reject("token has expired");
  }
  bool has_nbf = false;
  const double nbf = ClaimNumber(claims, "nbf", has_nbf);
  if (has_nbf && nbf - static_cast<double>(skew) > static_cast<double>(now)) {
    Reject("token is not yet valid");
  }

  const std::string subject = ClaimString(claims, "sub");
  if (subject.empty()) Reject("token has no sub claim");

  Principal principal;
  principal.principal_id = subject;
  principal.role = ClaimString(claims, "role");
  principal.organization = ClaimString(claims, "org");
  const std::string token_id = ClaimString(claims, "jti");
  if (!token_id.empty()) principal.attributes["jti"] = token_id;
  principal.attributes["iss"] = config_.issuer;
  return principal;
}

Principal DevelopmentAuthenticator::Authenticate(
    const RequestHeaders& headers) const {
  Principal principal;
  principal.principal_id = HeaderValue(headers, "x-pdal-principal");
  principal.organization = HeaderValue(headers, "x-pdal-organization");
  principal.role = HeaderValue(headers, "x-pdal-role");
  return principal;
}

}  // namespace pdal
