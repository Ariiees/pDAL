# Performance methodology

`pdal_avs_benchmark` compares the AVS-owned `HistoricalRetrieveAPI` directly with the identical request executed through `DataQuery -> QueryEngine -> AvsStorageBackend -> DataStream`. It also measures cached resource discovery and describe. Output is JSON containing p50/p95/p99 latency, user/system CPU, process peak RSS, records, bytes, throughput, internal direct-AVS hot/cold bytes, and p50 framework overhead.

Build and run:

```bash
cmake -S . -B build -DPDAL_BUILD_BENCHMARKS=ON -DPDAL_WITH_AVS=ON
cmake --build build -j2

./build/pdal_avs_benchmark \
  --resource camera.front \
  --start START_NS --end END_NS \
  --max-records 100 --iterations 30 --clients 4 > camera-short.json
```

The executable warms both paths and reverses their order for the second half of the samples. Pin the process and collect system-level CPU/RSS where research-grade repeatability is needed. Report hardware, governor, temperature, build type, background ROS load, payload sizes, cache state, tier roots, and raw JSON. A negative framework delta can still occur from filesystem-cache and scheduler noise.

## Required matrix

Run the executable with real indexed windows for:

| Workload | Resource / setup |
|---|---|
| Resource discovery and describe | emitted by every run; use at least 1,000 iterations with a one-record range |
| Small GPS history | `position`, small record count |
| Short camera history | `camera.front`, short range |
| Large camera history | `camera.front`, maximum permitted range/records/bytes |
| LiDAR history | `lidar.top`, representative short and large ranges |
| SSD history | point `--hdd-root` at an empty test root and select a hot interval |
| HDD history | point `--ssd-root` at an empty test root and select an archived interval |
| Concurrent history clients | `concurrent_pdal_history` runs `--clients` callers against the bounded bulk gate; repeat with 2, 4, and 8 |
| Metadata during bulk LiDAR | `metadata_history_while_bulk` holds a payload stream and measures metadata history; also measure discovery/describe over HTTP |

For live data, compare direct ROS subscription/take latency with `LATEST` and `SUBSCRIBE` on the same topic and QoS while the vehicle or replay publishes. Measure at 1, 2, 4, and the configured maximum subscriptions. Record setup latency, sample rate, delivery latency, delivered/dropped samples, CPU, RSS, and bytes. Live performance is environment-dependent and is intentionally not fabricated by the offline benchmark.

When ROS support and benchmarks are enabled, `pdal_ros_live_benchmark` performs that live-source comparison through the actual configured topic. Start the vehicle publisher/replay first, then run, for example:

```bash
./build-ros/pdal_ros_live_benchmark \
  --resource position --iterations 30 --subscriptions 4 --timeout-ms 10000
```

It reports direct `RosLiveDataSource` versus `QueryEngine` latest/subscription latency, CPU, RSS, bytes/throughput, pDAL latest overhead, and multiple-subscription setup. Repeat with camera and LiDAR publishers to include their actual JPEG/LAZ representation work.

The architecture tests verify bounded cursors and queues; performance runs determine whether configured limits are suitable for Raspberry Pi 5-class hardware. Optimize only measured bottlenecks.

## Development snapshot (2026-08-20)

One smoke run used this host's four-core Cortex-A76 (aarch64, 1.5–2.4 GHz, `ondemand` governor), `RelWithDebInfo`, 50 measured iterations, two camera frames per query, and isolated SSD/HDD roots. This is a regression snapshot, not a publication-quality result.

| Path | p50 | p95 | p99 | pDAL p50 overhead | Bytes (50 runs) |
|---|---:|---:|---:|---:|---:|
| Direct AVS, SSD | 0.522 ms | 0.607 ms | 0.711 ms | — | 53,277,050 |
| pDAL + AVS, SSD | 0.556 ms | 0.592 ms | 0.599 ms | 0.034 ms (6.5%) | 53,277,050 |
| Direct AVS, HDD | 1.550 ms | 106.813 ms | 107.969 ms | — | 59,557,650 |
| pDAL + AVS, HDD | 1.578 ms | 95.599 ms | 96.484 ms | 0.028 ms (1.8%) | 59,557,650 |

Cached discovery p50 was 0.003–0.006 ms and describe p50 was 0.00007–0.00011 ms. Process peak RSS during these small runs was about 6.2 MB. HDD tail latency had large I/O/scheduler outliers in both direct and pDAL paths; repeat under controlled cache, thermal, and scheduling conditions before drawing conclusions. The benchmark JSON also records CPU and throughput.

A five-sample live smoke run used the same host and a 50 Hz ROS `GPSFix` publisher. Direct live-source latest p50 was 0.00017 ms; pDAL latest p50 was 0.00474 ms (0.00458 ms framework delta). Direct and pDAL subscription `Next()` p50 values were 20.03 ms and 19.95 ms respectively, dominated by the 20 ms publish period. Creating and cancelling three pDAL subscriptions took 0.0318 ms p50 per batch. ROS/OpenCV/PCL process peak RSS was about 65.5 MB. These small figures validate the harness and routing only; use longer runs and real camera/LiDAR publishers for conclusions.
