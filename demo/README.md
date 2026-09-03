# pDAL Raspberry Pi demo service

This directory contains the two-device demo components that run on the
Raspberry Pi. The OEM viewer and all host-side decoding code live separately
on the repository's [`host`](https://github.com/Ariiees/pDAL/tree/host) branch.

## Pi responsibilities

- Run the current pDAL protected data-access service.
- Authenticate every caller with a signed bearer token (401 on failure).
- Discover AVS recordings and expose authorized metadata.
- Enforce the YAML resource and role policy before reading payloads (403 on denial).
- Blur every person in every returned `camera.front` frame before it leaves the Pi.
- Return only selected `PDALSTR1` GPS, LAZ, or already-blurred JPEG records.
- Measure transferred response-body bytes.
- Never reconstruct point clouds or render on the Pi.

`start_pi.sh` runs `scripts/fetch_privacy_deps.sh` on first use to download the
ONNX Runtime the blur stage needs (`third_party/onnxruntime/`, git-ignored). The
detector model is vendored at `models/yolov8n.onnx`.

## Authentication

Two layers, both set up by `start_pi.sh`:

1. **pDAL ← gateway (bearer token).** `start_pi.sh` creates git-ignored
   `pi/config/auth-secret`, exports `PDAL_AUTH_SECRET`, and hands it to the
   gateway. pDAL then requires `Authorization: Bearer <token>` on every endpoint
   except `GET /pdal/v1` and `GET /pdal/v1/capabilities`. The gateway mints the
   token per role; it no longer sends `X-PDAL-*`. Mint one by hand with
   `demo/scripts/mint_token.py`.
2. **gateway ← client (per-role password).** The gateway requires a signed-in
   role session on every `/api/*` endpoint except `/api/health` and `/api/roles`.
   `POST /api/login {role, password}` returns a session token; send it back as
   `Authorization: Bearer <session>`. Passwords live in git-ignored
   `pi/config/roles.auth.json` (seeded from `roles.auth.example.json`; demo
   values `fleet-demo` / `service-demo` / `incident-demo`; change with
   `demo/scripts/hash_password.py`). Five wrong tries lock a role for 60 s.

`DEMO_SHORT_TTL=1 ./demo/scripts/start_pi.sh` issues 60 s sessions for showing
expiry. `--allow-legacy-role` restores the old unauthenticated `?role=`.

Walk through the whole boundary live:

```bash
demo/scripts/security_demo.sh \
  --secret-file demo/pi/config/auth-secret \
  --gateway-url http://127.0.0.1:8090
```

Full details: [`docs/security.md`](../docs/security.md).

The host performs all GPS parsing, camera display, LAZ decoding, point-cloud
rendering, timeline visualization, and bandwidth presentation.

## Layout

```text
pi/gateway.py                   Thin raw-record gateway over pDAL + per-role login
pi/config/pdal.yaml             Demo pDAL endpoint configuration
pi/config/policy.yaml           Demo role/resource policy
pi/config/auth.example.yaml     Documented auth: block reference
pi/config/roles.auth.example.json  Per-role demo passwords (seed for roles.auth.json)
scripts/start_pi.sh             Build/start pDAL and the gateway
scripts/mint_token.py           Mint an HS256 pDAL bearer token for local use
scripts/hash_password.py        Make a roles.auth.json password entry
scripts/security_demo.sh        Live authn/authz/quota/login walkthrough
```

## Start on the Pi

Prerequisites are the pDAL build dependencies, AVS source at
`/home/avs/AVS-PI/src/avs`, and recordings under `/home/avs/DATA/SSD` and/or
`/home/avs/DATA/HDD`.

From the pDAL repository root:

```bash
./demo/scripts/start_pi.sh
```

The launcher builds pDAL when `build/pdal` is absent, starts protected pDAL on
loopback port `8080`, waits for it to become ready, and then starts the raw-data
gateway on `0.0.0.0:8090`.

Verify the gateway locally:

```bash
curl --fail http://127.0.0.1:8090/api/health
```

The response reports `decode_location: host`, which documents and enforces the
two-device processing boundary.

## Build and test pDAL

```bash
cmake -S . -B build \
  -DPDAL_AVS_SOURCE_ROOT=/home/avs/AVS-PI/src/avs \
  -DPDAL_WITH_ROS_LIVE=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The host branch contains the end-to-end raw GPS/JPEG/LAZ validation because
those payloads must be decoded on the host, not on the Pi.

## Policy roles

| Identity | Purpose | Authorized resources |
|---|---|---|
| Fleet Analyst | `fleet-monitoring` | GPS |
| Service Technician | `diagnostics` | GPS and front camera |
| Incident Investigator | `incident-investigation` | GPS, front camera, and LiDAR |

Every host request crosses this Pi-side policy boundary. The presentation role
picker is not a production identity provider; the policy enforcement and real
HTTP denial occur on the Pi.
