# pDAL OEM host viewer

This branch contains only the code that runs on the OEM host computer. The
pDAL service, AVS storage integration, authorization policy, and raw-data
gateway run on the Raspberry Pi from the `pDAL` branch.

The host does not receive recordings in bulk and does not ask the Pi to decode
sensor data. It requests authorized `PDALSTR1` records, then performs GPS,
JPEG, and LAZ decoding and visualization locally in the browser.

## Layout

```text
host/server.py              Static viewer and transparent Pi API proxy
host/viewer/                Map, camera, LiDAR, timeline, and policy UI
scripts/start_host.sh       Host launcher
scripts/check_connectivity.sh
tests/smoke_test.py         End-to-end Pi-to-host validation
tests/laz_decode_test.js    Optional LAZ decoder validation
```

Generated screenshots, logs, PID files, bytecode, and sensor recordings are
not part of this branch.

## Start

The Pi must already expose its gateway at `http://128.175.213.254:8090`.
From the root of this branch on the host:

```bash
./scripts/check_connectivity.sh
./scripts/start_host.sh
```

Open `http://127.0.0.1:8088`. A login screen appears — pick a role and enter
its demo password:


| Role                  | Password        |
| --------------------- | --------------- |
| Fleet Analyst         | `fleet-demo`    |
| Service Technician    | `service-demo`  |
| Incident Investigator | `incident-demo` |

No data loads until you sign in. Five failed attempts lock a role for 60 s.
The startup banner also prints the passwords as a presenter reminder.

To use another Pi address:

```bash
PI_URL=http://PI_ADDRESS:8090 ./scripts/check_connectivity.sh
./scripts/start_host.sh --pi-url http://PI_ADDRESS:8090
```

To prefill the login form's role select (still requires the password):

```text
http://127.0.0.1:8088/?role=incident_investigator
```

## Host responsibilities

- Decode sampled GPS records and render the retained route.
- Display complete retained JPEG frames with fit, pan, zoom, and full-screen
  controls.
- Decode the authorized LAZ record and render its complete point cloud from a
  centered top-down default view.
- Render the multimodality timeline and measured Pi-to-host transfer evidence.
- Display real pDAL authorization denials returned by the Pi.

The browser loads MapLibre, Three.js, loaders.gl, and map tiles from their
pinned public URLs, so it needs internet access in addition to connectivity to
the Pi.

## Verify

With the Pi gateway running:

```bash
python3 tests/smoke_test.py --pi-url http://128.175.213.254:8090
```

The smoke test verifies: public health endpoint, unauthenticated 401
enforcement, wrong-password 401, login for all three roles, the access matrix
(fleet_analyst sees GPS only, camera/LiDAR denied with 403), the real pDAL
HTTP 403, raw GPS/JPEG/LAZ payloads, closest-record timing, timeline data,
measured transfer bytes, and logout.

An optional Deno-based LAZ decoder check is available:

```bash
deno run --allow-net tests/laz_decode_test.js http://128.175.213.254:8090
```

> **Note:** Camera frames are expected to arrive already blurred once the
> device-side privacy stage lands (separate workstream). See
> `docs/security.md` on the `pDAL` branch for the full security model.

## SSD/HDD and retrieval timing

Trips and their modalities show `SSD`, `HDD`, or `SSD + HDD`. The storage filter
shows recordings available on that medium; a trip stored in both appears once.
The Pi gateway must provide `storage_locations` metadata. Timestamp selection
uses the existing pDAL/AVS retrieval path, which reads archived HDD records
without extracting whole archives. If both copies exist, AVS selects its usual
preferred copy; labels indicate availability, not a forced source selection.

The bottom-right **Request → full response** panel measures each data request
through arrival of its complete body. This includes browser/host proxy, Pi
query processing and transport; it excludes sign-in interaction and browser
sensor decoding/rendering. Expand the panel for the latest 50 retrievals,
including requested timestamps, GPS/camera/LiDAR, duration and HTTP status.
Failed transfers are labeled. Entries reset when selecting another trip.

Red lines mark pedal-based hard-braking candidates supplied by the Pi gateway.
Click a line or choose its timestamp to retrieve permitted sensor data. Marker
access follows the existing `vehicle.brake` policy. The current Pi threshold is
raw `pedal_output >= 0.30` sustained for 0.20 s; change it in the Pi's
`demo/pi/config/hard_brake.json` and restart pDAL. No speed is used.

Optional browser checks (Playwright and Chromium required):
`node tests/storage_latency_browser.cjs` and
`node tests/brake_timeline_browser.cjs`. They use isolated browser fixtures.

For the deployed SSH tunnel, use
`./scripts/start_host.sh --pi-url http://127.0.0.1:18090`.

See [TODO.md](TODO.md) for remaining brake calibration work.

Trip/timestamp switching cancels obsolete requests and clears old sensor samples.
Access is checked for the selected trip before requesting payloads; temporary
backend failures show UNAVAILABLE rather than LOCKED. The Pi gateway serializes
historical reads and searches beyond two seconds when camera deduplication leaves
a gap, always within the selected trip. CAMERA TIME and Δt identify the actual
retained frame. AVS recording and role permissions are unchanged.
Regression check: `node tests/viewer_selection_test.cjs` (Playwright required).
After updating the gateway, restart Pi pDAL and hard-refresh the host browser.
