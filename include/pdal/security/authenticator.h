#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "pdal/model/types.h"

namespace pdal {

// Case-insensitive is the caller's responsibility: keys are expected lowercased,
// matching pdal::ExternalHeaders produced by the HTTP layer.
using RequestHeaders = std::unordered_map<std::string, std::string>;

struct AuthConfig {
  bool required = true;
  std::string issuer;
  std::string audience;
  std::string algorithm = "HS256";
  std::string hmac_secret;
  std::uint64_t clock_skew_s = 60;
};

// Verifies caller identity. Implementations never trust X-PDAL-* identity
// headers; a Principal is built only from a verified credential.
class Authenticator {
 public:
  virtual ~Authenticator() = default;

  // Returns a verified Principal or throws PdalError(kUnauthenticated, ...).
  virtual Principal Authenticate(const RequestHeaders& headers) const = 0;

  // True when a real credential is enforced. False for the development
  // pass-through authenticator.
  virtual bool enforced() const = 0;
};

// Verifies a signed bearer token supplied as `Authorization: Bearer <token>`.
// The token is a compact JWS (JWT) signed with HMAC-SHA256. The signature,
// issuer, audience, and expiry are checked before any claim is trusted; the
// Principal is populated from the `sub`, `role`, and `org` claims only.
class BearerTokenAuthenticator final : public Authenticator {
 public:
  explicit BearerTokenAuthenticator(AuthConfig config);

  Principal Authenticate(const RequestHeaders& headers) const override;
  bool enforced() const override { return true; }

 private:
  AuthConfig config_;
};

// Development-only. Authentication is disabled: identity is read from the
// legacy X-PDAL-* headers. Never use in production.
class DevelopmentAuthenticator final : public Authenticator {
 public:
  Principal Authenticate(const RequestHeaders& headers) const override;
  bool enforced() const override { return false; }
};

}  // namespace pdal
