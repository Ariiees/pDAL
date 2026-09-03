# pDAL security: authentication, authorization, and camera privacy

This document describes the security methods implemented on the device (Pi)
side: **authentication** (verify the caller), **authorization** (restrict the
verified caller), and **camera privacy** (blur every person in every returned
camera frame). TLS, storage encryption, consent, and retention are **out of
scope** and not implemented here.

## Summary

| Concern | Before | Now |
|---|---|---|
| Identity | `X-PDAL-Principal` / `X-PDAL-Organization` / `X-PDAL-Role` headers, trusted as sent | Signed **bearer token**; `X-PDAL-*` identity headers ignored |
| Unauthenticated request | Reached policy / backend | Rejected with **401** before any catalog or backend access |
| Authorization on `/pdal/v1/query` | Enforced (YAML role policy) | Same policy, now keyed on the **verified** principal |
| Authorization on `/pdal/v1/resources/{id}/…` (SDK / operation API) | **Not enforced** (`PassThroughPolicy`) | Same YAML policy, via `EnginePolicyHook`, enforced **before** history open, payload read, or live subscribe |
| Policy decision | Two code paths | **One** `PolicyEngine`, shared by both paths |
| Camera frames | Raw JPEG returned as stored | Every detected person **blurred** in-process before the frame leaves pDAL; fail closed |

## Authentication

### Credential

Every protected endpoint requires:

```
Authorization: Bearer <token>
```

The token is a **compact JWS (JWT), signed with HMAC-SHA256 (`HS256`)**. pDAL
verifies, in order, before trusting any claim:

1. Exactly three `base64url` segments; header `alg` is `HS256` (`none` is rejected).
2. Signature recomputed with the configured secret and compared in constant time.
3. `iss` equals the configured issuer.
4. `aud` contains the configured audience (string or array).
5. `exp` is present and not in the past (allowing `clock_skew_s`).
6. `nbf`, if present, is not in the future (allowing `clock_skew_s`).
7. `sub` is present and non-empty.

The `Principal` is then built **only** from verified claims:

| Claim | Principal field |
|---|---|
| `sub` | `principal_id` |
| `role` | `role` (selects the policy) |
| `org` | `organization` |
| `jti` | recorded in audit if present |

Any failure raises `PDAL_UNAUTHENTICATED` → **HTTP 401**, before the request
reaches the catalog, policy, or a backend.

### Public endpoints

Only these stay open (no token):

- `GET /pdal/v1` (service / health document)
- `GET /pdal/v1/capabilities`

Everything else — discovery, describe, history, latest, subscribe, `/pdal/v1/query`,
`/sovd/v1/bulk-data/query`, and request status — requires a valid token.

### Configuration

`config/pdal.yaml` (and `demo/pi/config/pdal.yaml`) carry an `auth:` block:

```yaml
auth:
  required: true                 # false = disabled (dev only), falls back to X-PDAL-* headers
  issuer: pdal-local-issuer
  audience: pdal
  algorithm: HS256
  hmac_secret_env: PDAL_AUTH_SECRET   # checked first
  hmac_secret_file: auth-secret       # then this file (path relative to the config)
  hmac_secret: inline-dev-secret      # then this inline value
  clock_skew_s: 60
```

Secret resolution order: **env var → file → inline**. The resolved secret must
be at least 16 bytes or the server refuses to start. See
[`demo/pi/config/auth.example.yaml`](../demo/pi/config/auth.example.yaml).

If the `auth:` block is missing or `required: false`, pDAL prints a warning and
runs the **development authenticator**: identity is read from the unverified
`X-PDAL-*` headers. Never do this in production.

### Minting a token (local / demo)

```bash
demo/scripts/mint_token.py \
  --secret-file demo/pi/config/auth-secret \
  --issuer pdal-local-issuer --audience pdal \
  --subject oem-incident-investigator \
  --role incident_investigator --org oem-demo --ttl 900
```

A production deployment replaces this with tokens from a real identity provider
that signs with the same shared secret (or, as a later upgrade, an asymmetric
key pDAL verifies with a public key).

## Authorization

### One decision, both paths

`YamlPolicyEngine` is the single authorizer:

- `POST /pdal/v1/query` and `/sovd/...` — `PdalPipeline::Prepare` calls it directly.
- `GET/POST /pdal/v1/resources/{id}/…` and the C++ SDK — `QueryEngine` calls it
  through `EnginePolicyHook`, which runs **after authentication and resource
  resolution but before** `OpenHistory`, `ReadPayload`, or a live `Subscribe`.

A hook may only **narrow** a query. The `QueryEngine` re-checks this and rejects
any widening with `PDAL_FORBIDDEN`.

### What the policy enforces

Per role (`role` claim → policy entry):

- **Resources** — the allowed logical resource IDs (`*` = all).
- **Purpose** — the request `purpose` must be in the role's allowed set.
- **Time window** — `earliest_time_ns` / `latest_time_ns` clamp the range;
  `max_time_span_ns` caps its length. (History only; `latest` / `subscribe`
  skip time checks.)
- **Record and byte caps** — `max_records` / `max_bytes` lower the request's
  own limits.
- **Transformations** — anything not in the role's allowlist is dropped.

A denial raises `PDAL_FORBIDDEN` → **HTTP 403** before payload access. A request
with no principal raises `PDAL_UNAUTHENTICATED` → **401**.

### Demo roles (`demo/pi/config/policy.yaml`)

| Role | Purpose | Resources |
|---|---|---|
| `fleet_analyst` | `fleet-monitoring` | `position` |
| `service_technician` | `diagnostics` | `position`, `camera.front` |
| `incident_investigator` | `incident-investigation` | `position`, `camera.front`, `lidar.top` |

Each is capped at 50 000 records / 256 MiB and a 20-minute span.

## Camera privacy (human blur)

Every `camera.front` frame that would leave pDAL is routed through a pixel-domain
stage (`src/privacy/human_blur.cpp`) that runs entirely on the Pi:

1. Decode the JPEG (OpenCV).
2. Detect people — YOLOv8n (COCO `person` class only) via ONNX Runtime on the CPU.
3. Gaussian-blur each detected region (`kernel = max(15, box_width / 3)`).
4. Re-encode as JPEG and emit that.

- **Applies to** `history`, `latest`, and `subscribe`, on both the
  `/pdal/v1/query` bulk path and the operation/SDK path.
- **Never touches** GPS or LiDAR (they stay byte-identical), and
  metadata-only requests skip image processing entirely.
- **Fail closed:** any decode / inference / re-encode failure raises
  `PDAL_BACKEND_UNAVAILABLE` and *no* camera bytes are written for that record.
  If the model or ONNX Runtime is missing, or `privacy.enabled: false`, camera
  requests fail closed the same way.
- **No stored copies, no pixel logging.** The audit log records only a
  `frame_anonymized` event: `regions_blurred`, input/output byte counts, and
  `blur_latency_ms`.
- The re-encoded frame is a different size from the stored one; the record
  framing and `payload_size` metadata are updated to the emitted size.

### Configuration (`privacy:` block)

```yaml
privacy:
  enabled: true
  model_path: ../models/yolov8n.onnx   # relative to the config file
  score_threshold: 0.25
  nms_threshold: 0.45
  jpeg_quality: 90
  blur_sigma: 30
  blur_kernel_divisor: 3
  intra_op_threads: 2
```

### Build dependencies

- **OpenCV** (`core`, `imgproc`, `imgcodecs`) — system package.
- **ONNX Runtime** prebuilt — `scripts/fetch_privacy_deps.sh` places it under
  `third_party/onnxruntime/` (git-ignored). `start_pi.sh` runs it automatically.
- **Model** — `models/yolov8n.onnx` is vendored (AGPL-3.0; see `models/README.md`).
- `-DPDAL_WITH_PRIVACY=OFF` builds without it; camera requests then fail closed.

## Errors

| Code | HTTP | Meaning |
|---|---|---|
| `PDAL_UNAUTHENTICATED` | 401 | Missing / malformed / forged / expired token, or spoofed identity headers only |
| `PDAL_FORBIDDEN` | 403 | Authenticated, but the role / purpose / resource / time is not permitted |
| `PDAL_BACKEND_UNAVAILABLE` | 503 | Camera frame could not be anonymized (fail closed) |

Error messages are generic; a safe `reason` detail is included for 401s. Token
bytes are never logged.

## Demo / gateway wiring

`demo/scripts/start_pi.sh` generates a git-ignored `demo/pi/config/auth-secret`
(`chmod 600`), exports `PDAL_AUTH_SECRET`, and passes it to the gateway.
`demo/pi/gateway.py` sends `Authorization: Bearer …` to pDAL; it no longer sends
`X-PDAL-*`.

### Per-role password login (device side)

The gateway now requires the caller to **sign in to a role** before any data
endpoint. Identity of the role is a signed session token, not a `?role=`
parameter.

| Endpoint | Auth | Purpose |
|---|---|---|
| `POST /api/login` `{role, password[, ttl_seconds]}` | none | returns `{token, role, purpose, expires_in}` |
| `POST /api/logout` | session | revokes the current session |
| `GET /api/session` | session | `{role, purpose, expires_in}` for the current token |
| `GET /api/access[?trip=…]` | session | per-resource allow/deny matrix, each cell decided by a real pDAL call |
| `GET /api/trips` `/api/timeline` `/api/history` `/api/closest` `/api/denial-proof` | session | role comes from the session |
| `GET /api/health` `/api/roles` | none | unchanged |

- Passwords: `demo/pi/config/roles.auth.json` (git-ignored; `start_pi.sh` seeds
  it from `roles.auth.example.json`). Hashes are PBKDF2-HMAC-SHA256; change them
  with `demo/scripts/hash_password.py`. Demo passwords: `fleet-demo`,
  `service-demo`, `incident-demo`.
- A wrong password gets **401**; five failures lock that role for 60 s (**429**).
- Sessions are HMAC-signed and expire (`session_ttl_seconds`, default 900).
  `DEMO_SHORT_TTL=1 ./demo/scripts/start_pi.sh` issues 60 s sessions so expiry is
  easy to show live.
- The client sends the session as `Authorization: Bearer <session-token>` on
  every `/api/*` call. It is **not** the pDAL token — the gateway still mints the
  pDAL token itself, only for the session's role.
- `--allow-legacy-role` re-enables `?role=` without a session (transition aid,
  insecure).

### Live security demo

```bash
demo/scripts/security_demo.sh \
  --secret-file demo/pi/config/auth-secret \
  --gateway-url http://127.0.0.1:8090
```

Prints one line per check: no/forged/tampered/expired/wrong-issuer token → 401;
`X-PDAL-*` spoof → 401; role × resource × purpose → 200/403; a 1-hour request
narrowed to the 20-minute policy cap; and (with `--gateway-url`) wrong password →
401, good password → session → `/api/trips` 200.

## Tests

| Test | Covers |
|---|---|
| `pdal_security_tests` | token verification (happy path, bad signature, wrong issuer/audience, expiry, `nbf`, `alg:none`, malformed, missing `sub`), identity headers ignored, `EnginePolicyHook` allow/deny/narrow for history and live, `QueryEngine` denies before backend access, bulk-vs-engine parity |
| `pdal_http_auth_smoke` | live server: public endpoints open, 401 without/with forged token, 401 for `X-PDAL-*` only, 200 with a valid token, 403 for a policy denial on the bulk path |
| `pdal_privacy_tests` | human blur: fail-closed stage throws, bad model path throws, people are blurred and dimensions retained, no-people frame leaves region count 0, garbage/undecodable input fails closed |
| `demo/scripts/security_demo.sh` | manual, against a running pDAL/gateway: authentication, authorization, quota-narrowing, camera-privacy, and password-login walkthrough |

Run: `ctest --test-dir build --output-on-failure` (privacy tests need
`third_party/onnxruntime/` — run `scripts/fetch_privacy_deps.sh` first).

## Not covered here

TLS/mTLS, secret rotation tooling, storage encryption, consent, and retention.
A permissively licensed detector should replace the AGPL YOLOv8n weights before
shipping. See [TODO.md](../TODO.md).
