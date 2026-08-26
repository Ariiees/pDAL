# pDAL — Open Vehicle Data Access Layer

pDAL is an in-process C++ modular monolith that gives applications stable logical vehicle-data resources while keeping AVS and ROS details private.

```text
PdalClient / REST / SOVD binding
                |
             DataQuery
                |
            QueryEngine
           /           \
      HISTORY       LATEST/SUBSCRIBE
         |                 |
 IStorageBackend     ILiveDataSource
         |                 |
 AvsStorageBackend   RosLiveDataSource
         |                 |
  AVS SSD + HDD          ROS 2
```

The canonical resources supplied with the repository are `camera.front`, `lidar.top`, and `position`. The former `vehicle.*` identifiers remain private compatibility aliases. Public descriptors and samples never contain ROS topics, AVS references, trips, files, offsets, or storage tiers.

## Developer API

```cpp
pdal::PdalClient client(query_engine);

for (const auto& resource : client.resources()) {
  // DISCOVER
}

auto camera = client.camera("front");
auto descriptor = camera.describe();
auto history = camera.history(start_ns, end_ns);
for (const auto& frame : history) {
  // HISTORY: finite DataStream
}

auto current = camera.latest();              // LATEST
auto subscription = camera.subscribe();      // SUBSCRIBE
camera.subscribe([](const pdal::DataSample& sample) {
  // callback form uses the same continuous DataStream
});
```

Typed helpers use the same generic resource handle and `QueryEngine` as `open("camera.front")`.

## Build and test

The AVS source root defaults to `/home/avs/AVS-PI/src/avs`. pDAL uses the small AVS-owned unified history API there; AVS remains the owner of SQLite, append-log, and cold archive interpretation.

```bash
cmake -S . -B build \
  -DPDAL_AVS_SOURCE_ROOT=/home/avs/AVS-PI/src/avs \
  -DPDAL_WITH_ROS_LIVE=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

For the ROS 2 live source:

```bash
source /opt/ros/jazzy/setup.bash
cmake -S . -B build-ros -DPDAL_WITH_ROS_LIVE=ON
cmake --build build-ros -j2
```

ROS support is optional at configuration time. If its dependencies are absent, the core and historical backend still build.

## Run

```bash
./build/pdal serve --config config/pdal.yaml
```

Discovery and operation-oriented bindings:

```bash
curl http://127.0.0.1:8080/pdal/v1/resources
curl http://127.0.0.1:8080/pdal/v1/resources/camera.front
curl http://127.0.0.1:8080/pdal/v1/resources/camera.front/latest
curl http://127.0.0.1:8080/pdal/v1/resources/camera.front/subscribe

curl -X POST http://127.0.0.1:8080/pdal/v1/resources/camera.front/history \
  -H 'Content-Type: application/json' \
  --data '{
    "time": {"start_ns": 1770307702634771758, "end_ns": 1770307703634771758},
    "purpose": "development",
    "representation": {"format": "jpeg"},
    "delivery": {"max_records": 2, "max_bytes": 67108864}
  }'
```

`POST /pdal/v1/query` and `POST /sovd/v1/bulk-data/query` remain compatible finite-history bindings. The CLI remains available for physical-service workflows.

## Development security status

The operation-oriented `QueryEngine` contains explicit `PolicyHook` and `PrivacyHook` positions, currently wired to development-only `PassThroughPolicy` and `NoOpPrivacy`. The legacy protected history endpoint retains the existing YAML role policy. Neither header claims nor these placeholders are production authentication. See [TODO.md](TODO.md) before deployment.

Configuration lives in [`config/`](config), the detailed design is in [docs/architecture.md](docs/architecture.md), the wire format is in [docs/api-v1.md](docs/api-v1.md), and performance methodology is in [docs/performance.md](docs/performance.md).
