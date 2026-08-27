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

Open `http://127.0.0.1:8088`. To use another Pi address:

```bash
PI_URL=http://PI_ADDRESS:8090 ./scripts/check_connectivity.sh
./scripts/start_host.sh --pi-url http://PI_ADDRESS:8090
```

The default viewer role is Fleet Analyst. To open directly as Incident
Investigator:

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

The smoke test verifies discovery, the real pDAL HTTP 403, raw GPS/JPEG/LAZ
payloads, closest-record timing, timeline data, and measured transfer bytes.

An optional Deno-based LAZ decoder check is available:

```bash
deno run --allow-net tests/laz_decode_test.js http://128.175.213.254:8090
```

See [TODO.md](TODO.md) for the future real brake-event integration boundary.
