# Security and privacy implementation handoff

## Scope

Implement only:

1. **Authentication** — verify the caller; never trust identity/role headers.
2. **Authorization** — restrict verified callers by resource, purpose, time,
   record count, and byte count.
3. **Privacy blur** — blur every detected human in every returned camera frame.

TLS, storage encryption, consent, retention, and other security work are out of
scope.

## Status

- **Authentication — implemented.** Signed HS256 bearer tokens; `X-PDAL-*`
  ignored; 401 before backend access on both query paths. Public: `GET /pdal/v1`,
  `GET /pdal/v1/capabilities`.
- **Authorization — implemented.** One `YamlPolicyEngine` decision shared by
  `PdalPipeline` and `QueryEngine` (via `EnginePolicyHook`); 403 before
  `OpenHistory` / `ReadPayload` / live `Subscribe`; policies only narrow.
- **Privacy blur — implemented.** In-process pixel stage
  (`include/pdal/privacy/`, `src/privacy/human_blur.cpp`): YOLOv8n person
  detection (ONNX Runtime, CPU) + Gaussian blur + JPEG re-encode, run after
  `ReadPayload` / before `DataSample` in `QueryEngine` and before the callbacks
  in `PdalPipeline::Stream`. History, latest, and subscribe. GPS/LiDAR
  byte-identical; metadata-only skips it; any failure returns no camera bytes.
  Not `PrivacyHook` (that still narrows `DataQuery` only, as `NoOpPrivacy`).
- **Demo security surface — implemented (device side).** Gateway per-role
  password login (`POST /api/login` → signed session), lockout, short-TTL mode,
  `GET /api/access` policy matrix, and `demo/scripts/security_demo.sh`.

See [docs/security.md](docs/security.md). Device-side code and tests
(`pdal_security_tests`, `pdal_privacy_tests`, `pdal_http_auth_smoke`,
`demo/scripts/security_demo.sh`) are on `pDAL`. Host-side work (login screen,
access panel, proxy/token handling, smoke test) is specified in
[demo/HOST_SECURITY_TODO.md](demo/HOST_SECURITY_TODO.md) for the `host` branch.

Built and measured on the Raspberry Pi 5 itself (Cortex-A76 @ 2.4 GHz): blur
~0.33 s/frame, peak RSS ~140 MB, non-camera paths unchanged. Follow-ups:
replace the AGPL YOLOv8n weights with a permissively licensed detector; add a
live-path `frame_anonymized` audit event; a governor-pinned publication run.

## Read before coding

Read every `docs/*.md`, then `README.md`, `IMPLEMENTATION_PLAN.md`, and
`PDAL_ARCHITECTURE_REVIEW.md`. Understand both current paths:

```text
HTTP -> adapter -> QueryEngine -> hooks -> AVS backend
HTTP /pdal/v1/query -> PdalPipeline -> YamlPolicyEngine -> AVS backend
AVS bytes -> PDALSTR1 -> Pi gateway -> host decoder/viewer
```

Current gaps: `request_translator.cpp` trusts `X-PDAL-*` headers,
`QueryEngine` uses `PassThroughPolicy`, and `NoOpPrivacy` cannot change payload
bytes. The demo uses `/pdal/v1/query`; both paths need identical enforcement.

## Implementation

### Authentication

- Add code under `include/pdal/security/` and `src/security/`; wire it through
  `src/main.cpp` and `src/transport/http_server.cpp`.
- Verify signed bearer-token signature, issuer, audience, expiry, and subject;
  build `Principal` only from verified claims.
- Reject missing/invalid credentials with `401` before data access. Ignore or
  reject `X-PDAL-Principal`, `X-PDAL-Organization`, and `X-PDAL-Role`.
- Protect discovery, describe, query, history, latest, subscribe, SOVD, and
  request-status endpoints. Health/capabilities may stay public.

### Authorization

- Keep rules in `include/pdal/policy/`, `src/policy/`, and existing YAML files.
  Reuse one policy decision in `QueryEngine` and `PdalPipeline`.
- Replace `PassThroughPolicy`. Authorize after authentication and before
  `OpenHistory`, `ReadPayload`, or live subscription. Policies may only narrow.
- Preserve `demo/pi/config/policy.yaml`: fleet analyst = GPS; service technician
  = GPS + camera; incident investigator = GPS + camera + LiDAR. Also enforce
  purpose and configured limits.

### Human blur

- Add the payload transformer under `include/pdal/privacy/` and `src/privacy/`.
  Do not use `PrivacyHook` for pixels; it only narrows `DataQuery`.
- For every `camera.front` JPEG, decode one frame, detect and blur all humans,
  re-encode it, then emit it. Apply to history, latest, and subscribe.
- Insert it after `ReadPayload` and before `DataSample` in
  `src/query_engine.cpp`, and before callbacks in `PdalPipeline::Stream` in
  `src/pipeline.cpp`. Buffer at most one camera frame.
- GPS/LiDAR stay byte-identical; metadata-only requests skip image processing.
  Any detection/codec failure must return no raw camera bytes (fail closed).

Update `demo/pi/gateway.py` only to forward credentials and stop inventing
identity headers. On `host`, make only the minimal credential change and display
the already-blurred JPEG. All normal decoding/reconstruction stays on the host;
Pi camera decode/re-encode is the privacy-stage exception.

## Do not touch

- `/home/avs/AVS-PI/**`, AVS APIs, `/home/avs/DATA/**`, recordings, or indexes.
- Catalog, planner, storage backends, live adapters, public API shapes, or
  `PDALSTR1` framing outside the integration points above.
- Host GPS/LAZ decoding, point-cloud reconstruction, or viewer layout.
- No cloud detection, hard-coded secrets, payload logging, or stored copies.

## Acceptance

- Valid tokens succeed. Missing, forged, expired, and spoofed-header requests
  return `401` with zero backend reads.
- Test every role/resource/purpose combination; denials return `403` before
  payload access on both query paths.
- JPEG fixtures with zero, one, and multiple people remain valid and retain
  dimensions; every detected person is blurred. Multi-frame queries blur every
  frame, and failures leak zero raw bytes.
- GPS, LiDAR, and metadata-only behavior remain unchanged.
- CTest and the host smoke test pass. Rerun the complete demo on the real host
  machine against the current Pi and verify that GPS, camera, and LiDAR are all
  visualized smoothly without freezes, broken frames, or UI regressions.
- The rerun must also prove that unauthorized access is denied and every person
  shown in the camera view is blurred before the frame reaches the host.
- Re-run the Pi 5 benchmarks from `docs/performance.md`: non-camera p95 may not
  regress by more than 10%; report blur latency, throughput, and peak RSS.

Done means core changes/tests are on `pDAL`, minimal host credential work is on
`host`, related docs are updated, and no protected path returns raw people.
