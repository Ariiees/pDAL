#include <openssl/hmac.h>

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pdal/audit/audit.h"
#include "pdal/catalog/resource_catalog.h"
#include "pdal/live/live_data_source.h"
#include "pdal/model/data.h"
#include "pdal/model/error.h"
#include "pdal/policy/policy_engine.h"
#include "pdal/query_engine.h"
#include "pdal/security/authenticator.h"
#include "pdal/security/engine_policy_hook.h"
#include "pdal/storage/storage_backend.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: "         \
                << #condition << '\n';                                      \
      ++failures;                                                           \
    }                                                                       \
  } while (false)

template <typename Callable>
void CheckError(pdal::ErrorClass expected, Callable&& callable) {
  try {
    callable();
    std::cerr << "expected PdalError was not thrown\n";
    ++failures;
  } catch (const pdal::PdalError& error) {
    CHECK(error.error_class() == expected);
  }
}

// --- token helpers ---------------------------------------------------------

const std::string kSecret = "test-secret-0123456789-abcdefABCDEF";

std::string Base64Url(const unsigned char* data, std::size_t size) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  for (std::size_t i = 0; i < size; i += 3) {
    const std::uint32_t value =
        static_cast<std::uint32_t>(data[i]) << 16U |
        (i + 1 < size ? static_cast<std::uint32_t>(data[i + 1]) << 8U : 0U) |
        (i + 2 < size ? static_cast<std::uint32_t>(data[i + 2]) : 0U);
    out.push_back(alphabet[(value >> 18U) & 63U]);
    out.push_back(alphabet[(value >> 12U) & 63U]);
    if (i + 1 < size) out.push_back(alphabet[(value >> 6U) & 63U]);
    if (i + 2 < size) out.push_back(alphabet[value & 63U]);
  }
  return out;
}

std::string Base64Url(const std::string& value) {
  return Base64Url(reinterpret_cast<const unsigned char*>(value.data()),
                   value.size());
}

std::string MintToken(const std::string& secret, const boost::json::object& claims,
                      const std::string& alg = "HS256") {
  const boost::json::object header{{"alg", alg}, {"typ", "JWT"}};
  const std::string signing_input = Base64Url(boost::json::serialize(header)) +
                                    "." +
                                    Base64Url(boost::json::serialize(claims));
  std::array<unsigned char, 32> mac{};
  unsigned int length = 0;
  HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
       reinterpret_cast<const unsigned char*>(signing_input.data()),
       signing_input.size(), mac.data(), &length);
  return signing_input + "." + Base64Url(mac.data(), length);
}

std::int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

boost::json::object GoodClaims() {
  return {{"iss", "pdal-test-iss"},
          {"aud", "pdal"},
          {"sub", "svc-1"},
          {"role", "incident_investigator"},
          {"org", "oem"},
          {"iat", NowSeconds()},
          {"exp", NowSeconds() + 900}};
}

pdal::AuthConfig AuthCfg() {
  pdal::AuthConfig config;
  config.required = true;
  config.issuer = "pdal-test-iss";
  config.audience = "pdal";
  config.algorithm = "HS256";
  config.hmac_secret = kSecret;
  config.clock_skew_s = 60;
  return config;
}

pdal::RequestHeaders Bearer(const std::string& token) {
  return {{"authorization", "Bearer " + token}};
}

// --- authentication tests ------------------------------------------------

void TestBearerTokenHappyPath() {
  pdal::BearerTokenAuthenticator auth(AuthCfg());
  const auto principal = auth.Authenticate(Bearer(MintToken(kSecret, GoodClaims())));
  CHECK(principal.principal_id == "svc-1");
  CHECK(principal.role == "incident_investigator");
  CHECK(principal.organization == "oem");
  CHECK(auth.enforced());
}

void TestBearerTokenRejections() {
  pdal::BearerTokenAuthenticator auth(AuthCfg());

  CheckError(pdal::ErrorClass::kUnauthenticated,
             [&] { (void)auth.Authenticate({}); });
  CheckError(pdal::ErrorClass::kUnauthenticated, [&] {
    (void)auth.Authenticate({{"authorization", "Basic Zm9vOmJhcg=="}});
  });
  CheckError(pdal::ErrorClass::kUnauthenticated,
             [&] { (void)auth.Authenticate(Bearer("only.two")); });

  auto forged = MintToken(kSecret, GoodClaims());
  forged.back() = forged.back() == 'A' ? 'B' : 'A';
  CheckError(pdal::ErrorClass::kUnauthenticated,
             [&] { (void)auth.Authenticate(Bearer(forged)); });
  CheckError(pdal::ErrorClass::kUnauthenticated, [&] {
    (void)auth.Authenticate(Bearer(MintToken("a-different-but-long-secret-value",
                                             GoodClaims())));
  });

  auto wrong_issuer = GoodClaims();
  wrong_issuer["iss"] = "someone-else";
  CheckError(pdal::ErrorClass::kUnauthenticated, [&] {
    (void)auth.Authenticate(Bearer(MintToken(kSecret, wrong_issuer)));
  });

  auto wrong_audience = GoodClaims();
  wrong_audience["aud"] = "other-service";
  CheckError(pdal::ErrorClass::kUnauthenticated, [&] {
    (void)auth.Authenticate(Bearer(MintToken(kSecret, wrong_audience)));
  });

  auto expired = GoodClaims();
  expired["exp"] = NowSeconds() - 3600;
  CheckError(pdal::ErrorClass::kUnauthenticated, [&] {
    (void)auth.Authenticate(Bearer(MintToken(kSecret, expired)));
  });

  auto not_yet = GoodClaims();
  not_yet["nbf"] = NowSeconds() + 3600;
  CheckError(pdal::ErrorClass::kUnauthenticated, [&] {
    (void)auth.Authenticate(Bearer(MintToken(kSecret, not_yet)));
  });

  auto no_subject = GoodClaims();
  no_subject.erase("sub");
  CheckError(pdal::ErrorClass::kUnauthenticated, [&] {
    (void)auth.Authenticate(Bearer(MintToken(kSecret, no_subject)));
  });

  CheckError(pdal::ErrorClass::kUnauthenticated, [&] {
    (void)auth.Authenticate(Bearer(MintToken(kSecret, GoodClaims(), "none")));
  });
}

void TestIdentityHeadersAreIgnored() {
  pdal::BearerTokenAuthenticator auth(AuthCfg());
  auto headers = Bearer(MintToken(kSecret, GoodClaims()));
  headers["x-pdal-principal"] = "attacker";
  headers["x-pdal-role"] = "service_technician";
  headers["x-pdal-organization"] = "evil-corp";
  const auto principal = auth.Authenticate(headers);
  CHECK(principal.principal_id == "svc-1");
  CHECK(principal.role == "incident_investigator");
  CHECK(principal.organization == "oem");
}

void TestAuthConfigValidation() {
  bool threw = false;
  try {
    pdal::AuthConfig config = AuthCfg();
    config.hmac_secret = "too-short";
    pdal::BearerTokenAuthenticator auth(config);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

void TestDevelopmentAuthenticatorReadsLegacyHeaders() {
  pdal::DevelopmentAuthenticator auth;
  const auto principal = auth.Authenticate({{"x-pdal-principal", "joe"},
                                            {"x-pdal-organization", "acme"},
                                            {"x-pdal-role", "viewer"}});
  CHECK(!auth.enforced());
  CHECK(principal.principal_id == "joe");
  CHECK(principal.role == "viewer");
  CHECK(principal.organization == "acme");
}

// --- authorization (EnginePolicyHook) tests -----------------------------

std::shared_ptr<pdal::PolicyEngine> DemoPolicy() {
  const std::uint64_t span = 1200000000000ULL;
  pdal::RolePolicy fleet;
  fleet.resources = {"position"};
  fleet.purposes = {"fleet-monitoring"};
  fleet.max_time_span_ns = span;
  fleet.max_records = 50000;
  fleet.max_bytes = 268435456;
  pdal::RolePolicy technician;
  technician.resources = {"position", "camera.front"};
  technician.purposes = {"diagnostics"};
  technician.max_time_span_ns = span;
  technician.max_records = 50000;
  technician.max_bytes = 268435456;
  pdal::RolePolicy incident;
  incident.resources = {"position", "camera.front", "lidar.top"};
  incident.purposes = {"incident-investigation"};
  incident.max_time_span_ns = span;
  incident.max_records = 50000;
  incident.max_bytes = 268435456;
  return std::make_shared<pdal::YamlPolicyEngine>(
      pdal::YamlPolicyEngine::FromPolicies(
          "sec-test", 60000000000ULL,
          {{"fleet_analyst", std::move(fleet)},
           {"service_technician", std::move(technician)},
           {"incident_investigator", std::move(incident)}}));
}

pdal::DataQuery History(const std::string& resource, const std::string& role,
                        const std::string& purpose, std::uint64_t start,
                        std::uint64_t end) {
  pdal::DataQuery query;
  query.request_id = pdal::GenerateRequestId();
  query.resource = resource;
  query.operation = pdal::Operation::kHistory;
  query.selector.time_range = pdal::TimeRange{start, end};
  query.representation.format = "native";
  query.options.max_records = 100;
  query.options.max_bytes = 64ULL * 1024ULL * 1024ULL;
  query.context.principal = {"svc-1", "oem", role, {}};
  query.context.purpose = purpose;
  return query;
}

void TestPolicyHookAllowsAndDenies() {
  const auto catalog = pdal::ResourceCatalog::LoadYaml("config/resources.yaml");
  pdal::EnginePolicyHook hook(catalog, DemoPolicy());

  const auto allowed = hook.Apply(
      History("position", "fleet_analyst", "fleet-monitoring", 1000, 2000),
      catalog.Get("position"));
  CHECK(allowed.options.max_records == 100);
  CHECK(allowed.selector.time_range->start_ns == 1000);
  CHECK(allowed.selector.time_range->end_ns == 2000);

  CHECK(hook.Apply(History("lidar.top", "incident_investigator",
                           "incident-investigation", 1000, 2000),
                   catalog.Get("lidar.top"))
            .resource == "lidar.top");

  CheckError(pdal::ErrorClass::kForbidden, [&] {
    (void)hook.Apply(History("camera.front", "fleet_analyst", "fleet-monitoring",
                             1000, 2000),
                     catalog.Get("camera.front"));
  });
  CheckError(pdal::ErrorClass::kForbidden, [&] {
    (void)hook.Apply(
        History("lidar.top", "service_technician", "diagnostics", 1000, 2000),
        catalog.Get("lidar.top"));
  });
  CheckError(pdal::ErrorClass::kForbidden, [&] {
    (void)hook.Apply(History("position", "fleet_analyst", "diagnostics", 1000,
                             2000),
                     catalog.Get("position"));
  });
  CheckError(pdal::ErrorClass::kForbidden, [&] {
    (void)hook.Apply(
        History("position", "no-such-role", "fleet-monitoring", 1000, 2000),
        catalog.Get("position"));
  });
}

void TestPolicyHookRejectsMissingPrincipal() {
  const auto catalog = pdal::ResourceCatalog::LoadYaml("config/resources.yaml");
  pdal::EnginePolicyHook hook(catalog, DemoPolicy());
  auto query =
      History("position", "fleet_analyst", "fleet-monitoring", 1000, 2000);
  query.context.principal.principal_id.clear();
  CheckError(pdal::ErrorClass::kUnauthenticated,
             [&] { (void)hook.Apply(query, catalog.Get("position")); });
}

void TestPolicyHookNarrowsLimitsAndWindow() {
  const auto catalog = pdal::ResourceCatalog::LoadYaml("config/resources.yaml");
  pdal::RolePolicy tight;
  tight.resources = {"position"};
  tight.purposes = {"p"};
  tight.earliest_time_ns = 1500;
  tight.latest_time_ns = 1800;
  tight.max_records = 5;
  tight.max_bytes = 64;
  auto engine = std::make_shared<pdal::YamlPolicyEngine>(
      pdal::YamlPolicyEngine::FromPolicies("tight", 60000000000ULL,
                                           {{"tight", std::move(tight)}}));
  pdal::EnginePolicyHook hook(catalog, engine);

  const auto narrowed = hook.Apply(History("position", "tight", "p", 1000, 3000),
                                   catalog.Get("position"));
  CHECK(narrowed.options.max_records == 5);
  CHECK(narrowed.options.max_bytes == 64);
  CHECK(narrowed.selector.time_range->start_ns == 1500);
  CHECK(narrowed.selector.time_range->end_ns == 1800);
}

void TestPolicyHookLiveOperations() {
  const auto catalog = pdal::ResourceCatalog::LoadYaml("config/resources.yaml");
  pdal::EnginePolicyHook hook(catalog, DemoPolicy());

  pdal::DataQuery latest;
  latest.request_id = pdal::GenerateRequestId();
  latest.resource = "camera.front";
  latest.operation = pdal::Operation::kLatest;
  latest.representation.format = "native";
  latest.options.max_records = 1;
  latest.options.max_bytes = 8ULL * 1024ULL * 1024ULL;
  latest.context.principal = {"svc-1", "oem", "incident_investigator", {}};
  latest.context.purpose = "incident-investigation";

  const auto allowed = hook.Apply(latest, catalog.Get("camera.front"));
  CHECK(!allowed.selector.time_range.has_value());

  auto denied = latest;
  denied.context.principal.role = "fleet_analyst";
  denied.context.purpose = "fleet-monitoring";
  CheckError(pdal::ErrorClass::kForbidden,
             [&] { (void)hook.Apply(denied, catalog.Get("camera.front")); });
}

// --- QueryEngine integration -------------------------------------------

class CountingLocator final : public pdal::RecordLocator {
 public:
  explicit CountingLocator(std::vector<std::uint8_t> bytes)
      : payload(std::move(bytes)) {}
  std::vector<std::uint8_t> payload;
};

class CountingBackend final : public pdal::StorageBackend {
 public:
  pdal::BackendCapabilities GetCapabilities() const override {
    return {"avs", true, true, true, 0};
  }
  void Resolve(const pdal::CatalogResource&) const override {}
  std::vector<pdal::BackendRecord> Query(
      const pdal::ExecutionTask& task,
      const pdal::ResourceCatalog&) const override {
    ++query_calls;
    std::vector<pdal::BackendRecord> records;
    for (const std::uint64_t timestamp : {std::uint64_t{1200}, std::uint64_t{1400},
                                          std::uint64_t{1600}}) {
      if (timestamp < task.time.start_ns || timestamp > task.time.end_ns) continue;
      std::vector<std::uint8_t> payload{1, 2, 3, 4};
      records.push_back({task.resource_id, "avs", timestamp, payload.size(),
                        "memory",
                        std::make_shared<CountingLocator>(std::move(payload))});
    }
    return records;
  }
  std::uint64_t Read(const pdal::BackendRecord& record,
                     const pdal::ByteSink& sink) const override {
    ++read_calls;
    const auto locator =
        std::dynamic_pointer_cast<const CountingLocator>(record.locator);
    if (!locator) throw std::runtime_error("bad locator");
    sink(locator->payload.data(), locator->payload.size());
    return locator->payload.size();
  }
  mutable std::uint64_t query_calls = 0;
  mutable std::uint64_t read_calls = 0;
};

std::shared_ptr<pdal::QueryEngine> EngineWith(
    const std::shared_ptr<CountingBackend>& backend) {
  auto backends = std::make_shared<pdal::BackendRegistry>();
  backends->Register(backend);
  auto catalog = pdal::ResourceCatalog::LoadYaml("config/resources.yaml");
  auto hook = std::make_shared<pdal::EnginePolicyHook>(catalog, DemoPolicy());
  return std::make_shared<pdal::QueryEngine>(
      std::move(catalog), backends,
      std::make_shared<pdal::LiveSourceRegistry>(), hook,
      std::make_shared<pdal::NoOpPrivacy>(),
      std::make_shared<pdal::NullAuditSink>(), 1);
}

void TestQueryEngineAllowsAuthorizedHistory() {
  auto backend = std::make_shared<CountingBackend>();
  auto engine = EngineWith(backend);
  auto result = engine->Execute(
      History("position", "fleet_analyst", "fleet-monitoring", 1000, 2000));
  const auto sample = result.stream.Next();
  CHECK(sample.has_value());
  CHECK(backend->query_calls == 1);
  CHECK(backend->read_calls >= 1);
  result.stream.Cancel();
}

void TestQueryEngineDeniesBeforeBackendAccess() {
  auto backend = std::make_shared<CountingBackend>();
  auto engine = EngineWith(backend);
  CheckError(pdal::ErrorClass::kForbidden, [&] {
    (void)engine->Execute(
        History("camera.front", "fleet_analyst", "fleet-monitoring", 1000, 2000));
  });
  CHECK(backend->query_calls == 0);
  CHECK(backend->read_calls == 0);

  CheckError(pdal::ErrorClass::kForbidden, [&] {
    (void)engine->Execute(
        History("position", "fleet_analyst", "diagnostics", 1000, 2000));
  });
  CHECK(backend->query_calls == 0);
}

// --- shared-decision parity -------------------------------------------

void TestBulkAndEnginePathsAgree() {
  const auto catalog = pdal::ResourceCatalog::LoadYaml("config/resources.yaml");
  auto engine = DemoPolicy();

  pdal::PdalRequest request;
  request.request_id = pdal::GenerateRequestId();
  request.principal = {"svc-1", "oem", "incident_investigator", {}};
  request.purpose = "incident-investigation";
  request.resources = {"camera.front"};
  request.time = {1000, 2000};
  request.representation.format = "jpeg";
  request.delivery.max_records = 100;
  request.delivery.max_bytes = 64ULL * 1024ULL * 1024ULL;

  const auto bulk_plan = engine->Authorize(request, catalog);
  CHECK(bulk_plan.resources().size() == 1);

  pdal::EnginePolicyHook hook(catalog, engine);
  const auto narrowed = hook.Apply(
      History("camera.front", "incident_investigator", "incident-investigation",
              1000, 2000),
      catalog.Get("camera.front"));
  CHECK(narrowed.options.max_records ==
        (bulk_plan.limits().max_records == 0 ? narrowed.options.max_records
                                             : bulk_plan.limits().max_records));

  pdal::PdalRequest denied = request;
  denied.principal.role = "fleet_analyst";
  denied.purpose = "fleet-monitoring";
  CheckError(pdal::ErrorClass::kForbidden,
             [&] { (void)engine->Authorize(denied, catalog); });
  CheckError(pdal::ErrorClass::kForbidden, [&] {
    (void)hook.Apply(History("camera.front", "fleet_analyst", "fleet-monitoring",
                             1000, 2000),
                     catalog.Get("camera.front"));
  });
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"bearer token happy path", TestBearerTokenHappyPath},
      {"bearer token rejections", TestBearerTokenRejections},
      {"identity headers are ignored", TestIdentityHeadersAreIgnored},
      {"auth config validation", TestAuthConfigValidation},
      {"development authenticator reads legacy headers",
       TestDevelopmentAuthenticatorReadsLegacyHeaders},
      {"policy hook allows and denies", TestPolicyHookAllowsAndDenies},
      {"policy hook rejects missing principal",
       TestPolicyHookRejectsMissingPrincipal},
      {"policy hook narrows limits and window",
       TestPolicyHookNarrowsLimitsAndWindow},
      {"policy hook live operations", TestPolicyHookLiveOperations},
      {"query engine allows authorized history",
       TestQueryEngineAllowsAuthorizedHistory},
      {"query engine denies before backend access",
       TestQueryEngineDeniesBeforeBackendAccess},
      {"bulk and engine paths agree", TestBulkAndEnginePathsAgree},
  };
  for (const auto& [name, test] : tests) {
    try {
      test();
      if (failures == 0) std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
      ++failures;
    }
  }
  if (failures != 0) {
    std::cerr << failures << " security test assertion(s) failed\n";
    return 1;
  }
  std::cout << "all pDAL security tests passed\n";
  return 0;
}
