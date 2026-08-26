# pDAL two-device OEM history demo

This demo shows an OEM engineer selecting an existing onboard AVS recording,
choosing time visually, and receiving only role-authorized records. It uses the
current pDAL protected bulk query path; sensor access is not implemented as
frontend-only hiding.

## Two-device boundary

```text
Raspberry Pi 5                                      OEM host computer
┌──────────────────────────────────────┐            ┌──────────────────────────┐
│ AVS SSD/HDD metadata + recordings    │            │ Python static/proxy host │
│        ↓                             │ raw pDAL   │        ↓                 │
│ pDAL YAML authorization + AVS query  ├───────────►│ Browser parses PDALSTR1  │
│        ↓                             │ records    │ GPS binary / JPEG / LAZ  │
│ thin gateway (no sensor decoding)    │            │ MapLibre + Three.js      │
└──────────────────────────────────────┘            └──────────────────────────┘
```

The Pi never decodes GPS, JPEG, or LAZ and never reconstructs a point cloud.
For camera and LiDAR it first asks pDAL for authorized metadata near `T`,
selects the closest timestamp, then asks pDAL for exactly that one timestamp.
The host receives the original `PDALSTR1` framed stored payload and performs all
sensor decoding and rendering.

The browser uses MapLibre with OpenFreeMap's key-free OpenStreetMap-derived
vector style. It uses Three.js for interaction and the loaders.gl LAS loader (LASzip-compatible LAZ,
up through LAS 1.3) for AVS point clouds. Browser internet access is required
for the map tiles and the pinned web-library modules.

## Roles enforced by pDAL

| Viewer identity | pDAL role | Purpose | Authorized resources |
|---|---|---|---|
| Fleet Analyst | `fleet_analyst` | `fleet-monitoring` | GPS |
| Service Technician | `service_technician` | `diagnostics` | GPS, front camera |
| Incident Investigator | `incident_investigator` | `incident-investigation` | GPS, front camera, LiDAR |

The role picker represents a demo identity provider. It is intentionally easy
to switch for the presentation, but every request still crosses the pDAL YAML
policy. Header claims are not production authentication. The “Run policy denial
proof” button sends a protected Fleet Analyst camera query and displays pDAL's
real HTTP 403 response; no camera payload is read or transferred.

## Start the demo

Prerequisites on the Pi are the current pDAL build dependencies, AVS source at
`/home/avs/AVS-PI/src/avs`, and recordings under `/home/avs/DATA/SSD` and/or
`/home/avs/DATA/HDD`. The host needs Python 3 and a modern WebGL browser.

On the Raspberry Pi, from the pDAL repository:

```bash
./demo/scripts/start_pi.sh
```

This starts protected pDAL on loopback port `8080`, waits for it, then starts
the raw-data gateway on `0.0.0.0:8090`. It builds pDAL first only when
`build/pdal` is absent.

Deploy the host files without copying recordings:

```bash
./demo/scripts/deploy_host.sh
```

The deployment defaults to `yuxw@128.175.213.233:/home/yuxw/demo`. SSH may prompt for
the host account password; no password is stored in this repository or passed
on a command line. Override `HOST_TARGET` or `REMOTE_ROOT` when needed.

On the OEM host:

```bash
/home/yuxw/demo/scripts/start_host.sh
```

The host command automatically checks `http://128.175.213.254:8090/api/health`
before serving the viewer. Open `http://127.0.0.1:8088`. To use a different Pi:

```bash
/home/yuxw/demo/scripts/start_host.sh --pi-url http://PI_ADDRESS:8090
```

The standalone connectivity check is:

```bash
/home/yuxw/demo/scripts/check_connectivity.sh
```

## Demo sequence

1. Open the viewer. The sidebar is populated from real AVS global metadata; no
   timestamp entry is required.
2. Select a recording. Inspect its modality ranges, record counts, stored bytes,
   and empty event list.
3. Choose Fleet Analyst. The full-time-span sampled GPS trajectory is drawn on
   the real map; camera and LiDAR are visibly locked.
4. Click a route point or drag the timeline. The closest retained GPS record is
   independently queried and its exact `Δt` is shown.
5. Choose Service Technician. Move through time to inspect the closest retained,
   deduplicated JPEG and its requested time, stored timestamp, and `Δt`.
6. Choose Incident Investigator. Rotate and zoom the closest host-decoded AVS
   LAZ snapshot; it has its own timestamp and `Δt`.
7. Click “Run policy denial proof” and show the onboard pDAL HTTP 403 response.
8. Show the bandwidth strip. Candidate bytes are real selected-trip AVS append
   log sizes. Network bytes are measured HTTP response-body bytes received from
   the Pi during this selection, including metadata. Records returned counts
   actual raw records parsed by the host. The avoided percentage is calculated,
   never hardcoded.

Camera timeline dots correspond to retained, deduplicated frames. LiDAR dots
correspond to downsampled/compressed records. The GPS line represents continuous
trip coverage while the map route uses sampled real GPS records. This is a
timestamp-oriented multi-modal history inspector, not sensor fusion or 3D scene
reconstruction.

## Verification

Run the current pDAL tests on the Pi:

```bash
cmake -S . -B build -DPDAL_AVS_SOURCE_ROOT=/home/avs/AVS-PI/src/avs -DPDAL_WITH_ROS_LIVE=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

With the Pi side running, run the end-to-end smoke test from the host (so its GPS
test decode also respects the production boundary):

```bash
python3 demo/tests/smoke_test.py --pi-url http://128.175.213.254:8090
```

It checks discovery, `events: []`, the real pDAL 403, raw GPS/JPEG/LAZ signatures,
closest-record `Δt`, timeline records, and measured transfer bytes.

An optional decoder-specific host check is also included for environments with
Deno installed:

```bash
deno run --allow-net demo/tests/laz_decode_test.js http://128.175.213.254:8090
```

It retrieves one authorized raw LiDAR record and requires the same pinned
loaders.gl decoder used by the viewer to produce a non-empty XYZ array.

## Data and event truthfulness

Discovery reads AVS's global trip index and reports append-log sizes when the
hot log is present. HDD-only stored byte size may be reported as unavailable
rather than estimated. Payload reads always go through pDAL and the AVS unified
hot/cold history backend. No recording is copied to the host.

Some pre-existing trip-summary files contain GPS-derived event experiments.
They are deliberately ignored because there is no real brake recorder. The API
always returns `events: []`. See [TODO.md](TODO.md) for the required real
Lincoln MKZ / Dataspeed brake extension.
