# pDAL — Protected Data Access Layer

pDAL gives applications a stable, policy-controlled API for recorded and live
vehicle data while keeping storage tiers and live vehicle data private.

For this Pi's private-5G demo, run `./pDAL.sh` and wait for `READY`, then run
`./scripts/start_host.sh` in `/home/yuxw/demo` on the host. See the
[one-command launch instructions](demo/README.md#one-command-private-5g-launch).

The repository has two deployment branches:

| Machine | Git branch | Responsibility |
|---|---|---|
| Raspberry Pi 5 | [`pDAL`](https://github.com/Ariiees/pDAL/tree/pDAL) | pDAL core, AVS history adapter, policy enforcement, optional ROS live adapter, and the raw-record demo gateway |
| OEM host computer | [`host`](https://github.com/Ariiees/pDAL/tree/host) | Host proxy, browser viewer, GPS/JPEG/LAZ decoding, point-cloud rendering, and end-to-end validation |

Branch names are case-sensitive. Clone `pDAL` on the Pi and `host` on the host
computer. The host branch intentionally does not contain the Pi/core source,
and the latest pDAL branch does not contain the host viewer.

## Architecture

### pDAL core on the Pi

pDAL is a C++20 modular monolith. Its modules are logical boundaries inside
one process; there is no internal HTTP, RPC, broker, or service mesh.

```text
Application / C++ SDK / REST / SOVD-style binding
                         |
              stable DataQuery semantics
                         |
                    QueryEngine
             +-----------+-----------+
             |                       |
       ResourceCatalog       PolicyHook -> PrivacyHook
             |                       |
             +-----------+-----------+
                         |
            +------------+-------------+
            |                          |
         HISTORY                LATEST / SUBSCRIBE
            |                          |
     IStorageBackend             ILiveDataSource
            |                          |
    AvsStorageBackend          RosLiveDataSource
            |                          |
 AVS SSD hot + HDD cold              ROS 2
```

The public semantic model consists of `ResourceDescriptor`, `DataQuery`,
`QueryOptions`, `RepresentationRequest`, `DataSample`, `DataResult`, and
`DataStream`. These types contain no AVS or ROS implementation details.

The supplied canonical resources are:

- `camera.front`
- `lidar.top`
- `position`

The compatibility names `vehicle.camera.front`, `vehicle.lidar.top`, and
`vehicle.position` resolve internally but are not returned by discovery.

### Two-device OEM demo

```text
Raspberry Pi 5                                      OEM host

AVS SSD/HDD recordings                              Browser viewer
        |                                                |
        v                                                | local decode/render
pDAL :8080 (loopback)                                    | GPS / JPEG / LAZ
  catalog -> policy -> AVS query                         |
        |                                                |
        v                                                v
thin demo gateway :8090  <----------------------  host proxy :8088
 metadata + selected raw PDALSTR1 records          HTTP and static UI
```

The Pi performs discovery, policy enforcement, time/range selection, and raw
record transfer. It does not decode JPEG or LAZ data and does not reconstruct
point clouds. The host performs all sensor decoding and visualization.

The demo gateway reads AVS trip metadata and sends protected requests to pDAL.
It is a presentation adapter, not a replacement for the stable pDAL API.

## APIs available to users

pDAL exposes the same resource/operation model through its C++ SDK and REST
binding. Detailed contracts are in [docs/api-v1.md](docs/api-v1.md).

### C++ SDK

Include [`include/pdal/sdk/client.h`](include/pdal/sdk/client.h) and construct a
`PdalClient` with the application's `QueryEngine`:

```cpp
pdal::PdalClient client(query_engine);

// DISCOVER
for (const auto& resource : client.resources()) {
  // resource.id, operations, representations, availability, limits
}

auto camera = client.camera("front");       // camera.front
auto descriptor = camera.describe();        // DESCRIBE

// HISTORY: finite stream over an inclusive nanosecond range
auto history = camera.history(start_ns, end_ns);
while (auto frame = history.Next()) {
  // frame->timestamp_ns, representation, content_type, payload
}

auto current = camera.latest();              // LATEST
auto subscription = camera.subscribe();      // SUBSCRIBE stream

camera.subscribe([](const pdal::DataSample& sample) {
  // callback subscription form
});
```

Generic access uses `client.open("camera.front")`. Typed helpers
`camera("front")`, `lidar("top")`, and `position()` use the same engine and
policy/privacy hook path.

### REST API

The default base URL is `http://127.0.0.1:8080/pdal/v1`.

| Method | Endpoint | Operation |
|---|---|---|
| `GET` | `/pdal/v1/resources` | Discover public resources |
| `GET` | `/pdal/v1/resources/{id}` | Describe one resource |
| `POST` | `/pdal/v1/resources/{id}/history` | Finite historical stream |
| `GET` | `/pdal/v1/resources/{id}/latest` | Latest live sample |
| `GET` | `/pdal/v1/resources/{id}/subscribe` | Continuous live stream |
| `POST` | `/pdal/v1/query` | Compatible protected bulk-history query |
| `POST` | `/sovd/v1/bulk-data/query` | Experimental SOVD-style history binding |
| `GET` | `/pdal/v1/capabilities` | Capability document |
| `GET` | `/pdal/v1/requests/{request_id}` | Compatible bulk-request status |

All endpoints below need `--header "Authorization: Bearer ${TOKEN}"` (see the
bulk example for how to mint one); only `GET /pdal/v1` and
`GET /pdal/v1/capabilities` are public.

Discovery and description:

```bash
curl --fail -H "Authorization: Bearer ${TOKEN}" http://127.0.0.1:8080/pdal/v1/resources
curl --fail -H "Authorization: Bearer ${TOKEN}" http://127.0.0.1:8080/pdal/v1/resources/camera.front
```

Operation-oriented history query:

```bash
curl --fail --request POST \
  http://127.0.0.1:8080/pdal/v1/resources/camera.front/history \
  --header 'Content-Type: application/json' \
  --header "Authorization: Bearer ${TOKEN}" \
  --data '{
    "time": {
      "start_ns": 1770307702634771758,
      "end_ns": 1770307703634771758
    },
    "purpose": "development",
    "representation": {
      "format": "native",
      "sampling": {"every_n": 1},
      "transformations": []
    },
    "delivery": {"max_records": 2, "max_bytes": 67108864}
  }' \
  --output camera.pdalstream
```

Protected bulk query used by the OEM gateway:

```bash
TOKEN=$(demo/scripts/mint_token.py \
  --secret-file demo/pi/config/auth-secret \
  --issuer pdal-local-issuer --audience pdal \
  --subject oem-incident-investigator --role incident_investigator --org oem-demo)

curl --fail --request POST http://127.0.0.1:8080/pdal/v1/query \
  --header 'Content-Type: application/json' \
  --header "Authorization: Bearer ${TOKEN}" \
  --data '{
    "purpose": "incident-investigation",
    "resources": ["lidar.top"],
    "time": {
      "start_ns": "1770307702634771758",
      "end_ns": "1770307703634771758"
    },
    "representation": {
      "format": "laz",
      "sampling": {"every_n": 1},
      "transformations": []
    },
    "delivery": {
      "mode": "stream",
      "max_records": 1,
      "max_bytes": 268435456
    }
  }' \
  --output lidar.pdalstream
```

The `role`, `sub`, and `org` come from the verified token, not from headers. See
[docs/security.md](docs/security.md).

### Record-stream wire format

Data endpoints return
`application/vnd.pdal.record-stream; version=1`. HTTP chunks are transport
chunks, not record boundaries. Concatenate them and parse:

```text
8 bytes   ASCII "PDALSTR1"

repeated frame:
4 bytes   metadata JSON length, unsigned big-endian
8 bytes   payload length, unsigned big-endian
N bytes   UTF-8 public metadata
M bytes   stored or negotiated payload
```

Public metadata includes the resource ID, timestamp, representation, content
type, and payload size. It does not expose storage paths, tiers, offsets, AVS
references, or ROS topics.

### OEM demo gateway API

The Pi gateway listens on port `8090` for the host viewer. These `/api/*`
routes are demo-specific rather than stable pDAL v1 endpoints:

| Endpoint | Purpose |
|---|---|
| `GET /api/health` | Pi, pDAL, and decode-boundary health |
| `GET /api/roles` | Presentation roles |
| `GET /api/trips` | Real AVS recording catalog and authorized modalities |
| `GET /api/timeline` | Authorized modality timestamps |
| `GET /api/history` | Sampled raw historical records |
| `GET /api/closest` | Closest retained raw record to a requested time |
| `GET /api/denial-proof` | Demonstrate a real pDAL policy denial |

## Install the correct branch on each machine

### 1. Raspberry Pi 5: clone `pDAL`

```bash
git clone --branch pDAL --single-branch \
  https://github.com/Ariiees/pDAL.git ~/pDAL
cd ~/pDAL
```

Required Pi build/runtime dependencies are:

- CMake 3.20 or newer and a C++20 compiler
- Boost 1.81 or newer with `json` and `system`
- OpenSSL, SQLite3, yaml-cpp, and POSIX threads development packages
- Python 3 and `curl`
- AVS source at `/home/avs/AVS-PI/src/avs`
- AVS data at `/home/avs/DATA/SSD` and/or `/home/avs/DATA/HDD`

The paths above match the supplied Pi launcher and configuration. If the AVS
or data paths differ, update `PDAL_AVS_SOURCE_ROOT` and
[`demo/pi/config/pdal.yaml`](demo/pi/config/pdal.yaml) before starting.

Build and run unit tests explicitly:

```bash
cmake -S . -B build \
  -DPDAL_AVS_SOURCE_ROOT=/home/avs/AVS-PI/src/avs \
  -DPDAL_WITH_ROS_LIVE=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

ROS 2 live data is optional. For a history-only demo, keep
`PDAL_WITH_ROS_LIVE=OFF`.

### 2. OEM host: clone `host`

On the host computer, use a different directory:

```bash
git clone --branch host --single-branch \
  https://github.com/Ariiees/pDAL.git ~/pDAL-host
cd ~/pDAL-host
```

The host requires Python 3, `curl`, and a modern WebGL browser such as Firefox
or Chromium. The browser also needs internet access for MapLibre, Three.js,
loaders.gl, and map tiles. No AVS source, pDAL C++ build, or sensor recording is
copied to the host.

For existing clones, update with:

```bash
# On the Pi
git switch pDAL && git pull --ff-only origin pDAL

# On the host
git switch host && git pull --ff-only origin host
```

## Run the two-device demo

Use this order: start the Pi first, verify port `8090` from the host, and then
start the host viewer.

### Step 1: start the Pi services

On the Raspberry Pi, from the `pDAL` branch checkout:

```bash
cd ~/pDAL
./demo/scripts/start_pi.sh
```

The launcher:

1. Builds `build/pdal` if it is absent.
2. Starts pDAL on `127.0.0.1:8080` using the demo YAML policy.
3. Waits for pDAL health.
4. Starts the thin raw-record gateway on `0.0.0.0:8090`.

Keep this terminal running. Verify locally on the Pi:

```bash
curl --fail http://127.0.0.1:8090/api/health
```

The response must contain `"status":"ready"` and
`"decode_location":"host"`.

If the Pi must use non-default data roots:

```bash
./demo/scripts/start_pi.sh \
  --ssd-root /path/to/SSD \
  --hdd-root /path/to/HDD
```

### Step 2: verify Pi-to-host connectivity

The supplied configuration uses Pi address `128.175.213.254`. Replace it below
if the Pi has another address. On the host:

```bash
cd ~/pDAL-host
PI_URL=http://128.175.213.254:8090 \
  ./scripts/check_connectivity.sh
```

Expected output:

```text
Pi ↔ host connectivity OK; decode location: host
```

If this fails, confirm both machines are on the same reachable network and
that TCP port `8090` is allowed from the host to the Pi. Port `8080` should
remain loopback-only on the Pi.

### Step 3: start the host viewer

```bash
cd ~/pDAL-host
./scripts/start_host.sh --pi-url http://128.175.213.254:8090
```

Open this URL in the host browser:

```text
http://127.0.0.1:8088/?role=incident_investigator
```

The viewer should show the real GPS route, complete retained camera frame,
centered top-down LiDAR point cloud, multimodality timeline, and measured
Pi-to-host transfer evidence on one screen.

### Step 4: run the end-to-end check

In another host terminal:

```bash
cd ~/pDAL-host
python3 tests/smoke_test.py \
  --pi-url http://128.175.213.254:8090
```

This verifies discovery, a real pDAL HTTP 403, raw GPS/JPEG/LAZ signatures,
closest-record timing, timeline records, and measured network bytes.

## Ports and data ownership

| Port | Machine | Binding | Purpose |
|---|---|---|---|
| `8080` | Pi | `127.0.0.1` | Stable pDAL API; gateway access only in the demo |
| `8090` | Pi | `0.0.0.0` | Demo gateway used by the host |
| `8088` | Host | `0.0.0.0` | Host viewer and transparent gateway proxy |

Recordings remain on the Pi. Only policy-authorized, query-selected raw records
cross port `8090`; decoding and rendering occur on the host.

## Security status

**Authentication and authorization are enforced.** Every endpoint except
`GET /pdal/v1` and `GET /pdal/v1/capabilities` requires a signed bearer token
(`Authorization: Bearer <token>`); `X-PDAL-*` identity headers are ignored.
Missing, forged, or expired tokens get `401` before any backend access. Both the
compatible `/pdal/v1/query` path and the operation-oriented `QueryEngine` now
apply the **same** YAML role policy — role, purpose, resource, time window, and
record/byte caps — returning `403` before payload access. Full details and setup
are in [docs/security.md](docs/security.md).

**Camera privacy is enforced.** Every returned `camera.front` frame is decoded,
every detected person is blurred (YOLOv8n + Gaussian blur, in-process on the
CPU), and the frame is re-encoded before it leaves pDAL — on history, latest,
and subscribe, on both query paths. GPS/LiDAR stay byte-identical; any failure
returns no camera bytes.

Still outstanding before production: TLS/mTLS, secret rotation, a permissively
licensed detector in place of the AGPL YOLOv8n weights, and the remaining items
in [TODO.md](TODO.md).

## More documentation

- [Architecture details](docs/architecture.md)
- [pDAL API v1 and stable errors](docs/api-v1.md)
- [Performance methodology](docs/performance.md)
- [Pi demo service](demo/README.md)
- [Implementation plan](IMPLEMENTATION_PLAN.md)
- [Architecture review](PDAL_ARCHITECTURE_REVIEW.md)
