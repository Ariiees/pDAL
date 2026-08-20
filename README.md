# pDAL — Protected Data Access Layer

pDAL is a transport-independent, protected vehicle-data abstraction above AVS. Clients name stable logical resources, while the AVS storage adapter alone translates those resources into AVS topics and `RetrieveAPI` calls.

```text
HTTP / SOVD-style / local CLI
             |
      canonical PdalRequest
             |
 identity -> policy -> AuthorizedAccessPlan
             |
        QueryPlanner
             |
        StorageBackend
             |
    AvsStorageBackend -> AVS RetrieveAPI
             |
 representation -> bounded streaming response
```

The initial catalog contains:

- `vehicle.camera.front` (`jpeg`)
- `vehicle.lidar.top` (`laz`)
- `vehicle.position` (`avs-gps-binary`)

ROS topics, trip IDs, log/index paths, offsets, and tier layout are never serialized by a public pDAL endpoint.

## Build

The default build consumes the existing AVS `RetrieveAPI` source read-only from `/home/avs/AVS-PI/src/avs`. Override that location when needed.

```bash
cmake -S . -B build \
  -DPDAL_AVS_SOURCE_ROOT=/home/avs/AVS-PI/src/avs
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The core has no ROS dependency. The only AVS dependency is the `pdal_avs_backend` target. A core-only build can use `-DPDAL_WITH_AVS=OFF` for tests or another storage adapter.

## Run

```bash
./build/pdal serve --config config/pdal.yaml
```

Discovery is unauthenticated:

```bash
curl http://127.0.0.1:8080/pdal/v1/resources
curl http://127.0.0.1:8080/pdal/v1/capabilities
```

Queries require identity headers. In this prototype they represent claims supplied by a trusted local deployment boundary; they are not production authentication.

```bash
curl -sS http://127.0.0.1:8080/pdal/v1/query \
  -H 'Content-Type: application/json' \
  -H 'X-PDAL-Principal: technician-17' \
  -H 'X-PDAL-Organization: workshop' \
  -H 'X-PDAL-Role: service' \
  --data '{
    "resources": ["vehicle.camera.front"],
    "time": {"start_ns": 1766013563038953891, "end_ns": 1766013568038953891},
    "purpose": "incident-investigation",
    "representation": {"format": "jpeg"},
    "delivery": {"mode": "metadata", "max_records": 100}
  }'
```

The physical-service CLI enters the identical policy/planning/backend pipeline:

```bash
./build/pdal query \
  --resource vehicle.camera.front \
  --start 1766013563038953891 \
  --end 1766013568038953891 \
  --purpose diagnostics \
  --format jpeg \
  --metadata
```

The demonstration client is at `http://127.0.0.1:8080/pdal/v1/demo`.

## Configuration and security

- [`config/resources.yaml`](config/resources.yaml) is the private logical-resource-to-backend mapping.
- [`config/policy.yaml`](config/policy.yaml) narrows resources, purposes, time, record counts, byte counts, and transformations per role.
- [`config/pdal.yaml`](config/pdal.yaml) configures storage, audit logging, the server, and continuation signing.

Replace the development continuation secret and the header-based identity boundary before production use. Audit events contain request metadata and metrics, never sensor payloads.

## Phase-1 boundary

The AVS C++ API currently exposes reusable SSD retrieval only. pDAL therefore reports `cold_tier: false` and uses the hot tier for this vertical slice. The HDD archive format is deliberately not reimplemented here. A future AVS-owned unified hot/cold retrieval API can be connected without changing pDAL request, policy, planning, or response semantics.

See [architecture](docs/architecture.md), [API v1](docs/api-v1.md), and the inspected-file mapping in [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md).
