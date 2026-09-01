# Host-side security work (implement on the `host` branch)

This file lives on the `pDAL` (device) branch for reference. The tasks below are
implemented on the **`host`** branch, which contains the browser viewer and the
`:8088` proxy. Nothing here changes the Pi or pDAL.

## Context — what already changed on the device side

The Pi gateway (`demo/pi/gateway.py`, port `8090`) now enforces a **per-role
password login** and no longer accepts `?role=` on data endpoints.

- New: `POST /api/login`, `POST /api/logout`, `GET /api/session`, `GET /api/access`.
- Changed: `GET /api/trips|/api/timeline|/api/history|/api/closest|/api/denial-proof`
  now take the role from a **session token**, not a query parameter.
- Unchanged and public: `GET /api/health`, `GET /api/roles`.
- Optional coarse network gate: if the Pi is started with
  `DEMO_PROTECT_GATEWAY=1`, every `/api/*` call except `/api/health` also needs
  header `X-Demo-Key: <key>`.

The host viewer currently opens `http://127.0.0.1:8088/?role=incident_investigator`
and the proxy forwards `?role=` to the gateway. **That path now returns 401.**
The host must obtain a session by logging in.

## Gateway API contract the host must speak

Base URL: the Pi gateway, e.g. `http://<pi>:8090` (the `:8088` proxy forwards to it).

### `POST /api/login`
Request JSON: `{"role": "<role id>", "password": "<password>", "ttl_seconds": <optional int>}`
Roles: `fleet_analyst`, `service_technician`, `incident_investigator` (also from `GET /api/roles`).
Responses:
- `200` → `{"token": "<session>", "role", "label", "purpose", "expires_in": <seconds>}`
- `401` → `{"error": "invalid role or password"}`
- `429` → `{"error": "too many failed attempts", "retry_after_seconds": <int>}` (5 failures lock a role for 60 s)
- `503` → login not configured on the gateway (treat as "demo misconfigured")

### Authenticated calls
Send the session token on every other `/api/*` request:
```
Authorization: Bearer <session token>
```
If the gateway was started with a key, also send `X-Demo-Key: <key>`.
A missing/expired/invalid session → `401 {"error": "..."}` on any data endpoint,
and `401 {"authenticated": false}` on `GET /api/session`.

### `GET /api/session`
`200` → `{"authenticated": true, "role", "label", "purpose", "expires_in"}` or `401`.
Use it to restore UI state on reload and to drive an expiry countdown.

### `GET /api/access[?trip=<trip id>]`
`200` → per-resource allow/deny matrix for the session's role, every cell decided
by a real pDAL call:
```json
{
  "role": "fleet_analyst", "label": "Fleet Analyst",
  "purpose": "fleet-monitoring", "principal": "oem-fleet-analyst",
  "trip_id": "2026-02-05-trip_05",
  "resources": {
    "position":     {"label": "GPS", "authorized": true,  "policy": "oem-demo-v1"},
    "camera.front": {"label": "Front camera", "authorized": false, "status": 403,
                     "code": "PDAL_FORBIDDEN", "reason": "Denied by pDAL policy"},
    "lidar.top":    {"label": "LiDAR", "authorized": false, "status": 403,
                     "code": "PDAL_FORBIDDEN", "reason": "Denied by pDAL policy"}
  }
}
```

### `POST /api/logout`
Send the session token; gateway revokes it. Response `200 {"ok": true}`.

### `POST /api/login` TTL demo
When the Pi runs with `DEMO_SHORT_TTL=1`, `expires_in` comes back as `60`. The UI
should surface the countdown and, on `401`, drop back to the login screen.

## Tasks

### 1. `:8088` proxy (`scripts/start_host.sh` / whatever serves `:8088`)
- [ ] Forward the `Authorization` request header to the gateway on every `/api/*`
      proxied call (do **not** strip it). Same for `X-Demo-Key`.
- [ ] Forward `POST` (currently the proxy may only handle `GET`): pass `/api/login`
      and `/api/logout` through with body and method intact.
- [ ] Relay the gateway's status code and JSON body verbatim (esp. `401`, `429`, `503`).
- [ ] New flag / env: `--pi-key` / `PI_GATEWAY_KEY` → if set, the proxy adds
      `X-Demo-Key: <key>` to every forwarded `/api/*` request. (Only needed when
      the Pi runs `DEMO_PROTECT_GATEWAY=1`.)
- [ ] The proxy stays stateless about the session — the **browser** holds the token.

### 2. Viewer (browser)
- [ ] Replace the role dropdown / `?role=` entry with a **login form**: role
      `<select>` (populate from `GET /api/roles`) + password `<input type=password>`
      + submit.
- [ ] On submit → `POST /api/login`. On `200`, store `token` and `expires_in`
      (in memory; `sessionStorage` is acceptable, `localStorage` is not). On `401`
      show "invalid role or password"; on `429` show the retry countdown; on `503`
      show "login not configured on the Pi".
- [ ] Attach `Authorization: Bearer <token>` to every `/api/*` fetch. Centralise
      this in the existing API helper.
- [ ] On any `401` from a data call → clear the token, return to the login form
      ("session expired, sign in again").
- [ ] On load, call `GET /api/session`; if `200`, skip the form and resume.
- [ ] "Sign out" button → `POST /api/logout`, clear token, show the form.
- [ ] Show the session countdown from `expires_in` somewhere unobtrusive; when it
      hits ~0, pre-emptively return to the form.
- [ ] Keep `?role=` working only as a *prefill* for the form's select, never as
      auth.

### 3. Access & Policy panel (new, in the viewer)
- [ ] After login, call `GET /api/access` and render a small table: one row per
      resource (`GPS`, `Front camera`, `LiDAR`) with a green "authorized" or a red
      badge showing `status` + `code` + `reason`.
- [ ] Refresh it when the selected trip changes (`?trip=` on `/api/access`).
- [ ] This panel is the headline "what security do we have" visual — make it
      visible on the main screen, not buried in a menu.

### 4. `scripts/check_connectivity.sh` (host copy)
- [ ] No change required — it only calls `GET /api/health`, which stays public.
- [ ] If `PI_GATEWAY_KEY` is set, still don't send it to `/api/health` (health is
      intentionally open); document that.

### 5. `tests/smoke_test.py` (host)
- [ ] Add a login step: `POST /api/login` with a demo password
      (`fleet-demo` / `service-demo` / `incident-demo`), capture the token, use it
      for all subsequent `/api/*` calls.
- [ ] New assertions:
  - `GET /api/trips` with **no** token → `401`.
  - `POST /api/login` with a wrong password → `401`.
  - `POST /api/login` with the right password → `200` and a non-empty `token`.
  - `GET /api/access` with the session → `200`, and for `fleet_analyst`:
    `position.authorized == true`, `camera.front.authorized == false` with
    `status == 403`.
- [ ] Keep the existing "real pDAL 403" assertion — now reached as
      `denial-proof` while logged in as a role that can't see the resource.
- [ ] Accept a `--pi-key` argument and pass `X-Demo-Key` when given.

### 6. `scripts/start_host.sh`
- [ ] Add `--pi-key` / `PI_GATEWAY_KEY` passthrough to the proxy.
- [ ] Print the demo passwords (or a pointer to them) in the startup banner so the
      presenter knows what to type.

### 7. Docs (host `README` / host docs)
- [ ] Document the login step in the run instructions: open `:8088`, pick a role,
      enter its password (`fleet-demo` etc.), and note that decoding/rendering are
      unchanged.
- [ ] Note that camera frames are expected to arrive already blurred once the
      device-side privacy stage lands (separate workstream; not this task).
- [ ] Point to `docs/security.md` on the `pDAL` branch for the full model.

## Acceptance checklist

- [ ] Opening `:8088` shows a login form; no data loads until a valid role +
      password is entered.
- [ ] Wrong password → clear error, no data. 5 wrong → lockout message with countdown.
- [ ] After login, GPS/camera/LiDAR views work exactly as before for a role that
      is allowed them; disallowed modalities show the `403` reason, not a crash.
- [ ] The Access & Policy panel shows the correct allow/deny per role
      (`fleet_analyst` = GPS only; `incident_investigator` = all three).
- [ ] With `DEMO_SHORT_TTL=1` on the Pi, the session visibly expires after ~60 s
      and the UI returns to the login form; logging in again resumes.
- [ ] `python3 tests/smoke_test.py --pi-url http://<pi>:8090` passes, including
      the new login / no-token / access-matrix assertions.
- [ ] With `DEMO_PROTECT_GATEWAY=1` on the Pi and `--pi-key` on the host,
      everything still works; without the key, `/api/*` (except health) is `401`.
- [ ] `scripts/check_connectivity.sh` still prints the OK line unchanged.
