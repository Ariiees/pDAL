# pDAL Raspberry Pi demo service

This directory contains the two-device demo components that run on the
Raspberry Pi. The OEM viewer and all host-side decoding code live separately
on the repository's [`host`](https://github.com/Ariiees/pDAL/tree/host) branch.

## Pi responsibilities

- Run the current pDAL protected data-access service.
- Discover AVS recordings and expose authorized metadata.
- Enforce the YAML resource and role policy before reading payloads.
- Return only selected raw `PDALSTR1` GPS, JPEG, or LAZ records.
- Measure transferred response-body bytes.
- Never decode JPEG/LAZ data or reconstruct point clouds.

The host performs all GPS parsing, camera display, LAZ decoding, point-cloud
rendering, timeline visualization, and bandwidth presentation.

## Layout

```text
pi/gateway.py             Thin raw-record gateway over pDAL
pi/config/pdal.yaml       Demo pDAL endpoint configuration
pi/config/policy.yaml     Demo role/resource policy
scripts/start_pi.sh       Build/start pDAL and the gateway
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
