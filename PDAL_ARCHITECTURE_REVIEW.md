# pDAL Open Vehicle Data Platform — Architecture Review

Review date: 2026-08-20

This review compares the current `pDAL` branch with the final modular-monolith architecture. It deliberately preserves the working request validation, YAML catalog loading, policy narrowing, deterministic planning, AVS hot-tier pass-through, HTTP streaming, continuation tokens, stable error envelopes, audit sink, and adapter-equivalence tests.

## CRITICAL

### Stable semantic and developer contracts are incomplete

The implementation has `PdalRequest`, `BackendRecord`, and callback streaming, but it does not expose the requested single-resource `DataQuery`, universal `Operation` vocabulary, public `DataSample`/`DataStream`, or an in-process `PdalClient`/resource handle. REST and CLI currently target the older batch request directly.

Targeted change: add these contracts and a `QueryEngine`; keep `PdalRequest` as the compatible REST/policy envelope and compile bindings/SDK calls into the new semantic query before execution.

### Resource descriptors mix semantic and physical concerns internally

Public JSON correctly omits `BackendBinding`, but `ResourceDescriptor` lacks `kind`, description/schema, semantic references, supported operations, separate historical/live availability, and per-resource limits. Current canonical IDs (`vehicle.camera.front`, `vehicle.lidar.top`, `vehicle.position`) do not match the final stable examples.

Targeted change: make `camera.front`, `lidar.top`, and `position` canonical; support old IDs only as non-public aliases. Add the missing semantic fields and separate private historical/live bindings without exposing either binding.

### There is no live-data boundary or routing

`LATEST` and `SUBSCRIBE`, `ILiveDataSource`, and `RosLiveDataSource` do not exist. The current pipeline can only execute historical AVS reads.

Targeted change: introduce `ILiveDataSource`, a bounded subscription abstraction, and a ROS implementation isolated in an optional ROS target. Route `HISTORY` only to `IStorageBackend`; route `LATEST`/`SUBSCRIBE` only to the live source.

### AVS history is hot-tier-only

`AvsStorageBackend` uses `avs::RetrieveAPI`, which only reads SSD append logs. The HDD archive has benchmark retrieval code but no reusable C++ API. Reimplementing that archive/index logic in pDAL would violate AVS ownership.

Targeted change: add one new AVS-owned `HistoricalRetrieveAPI` beside the unchanged `RetrieveAPI`. It will provide bounded, unified SSD/HDD references and payload reads. Existing AVS callers retain hot-only behavior and source compatibility.

### Historical reference materialization is not fully bounded

Payloads stream one record at a time, but `RetrieveAPI::QueryRefs` and `PdalPipeline::Prepare` can materialize every matching reference before applying record/byte limits. Large histories therefore have bounded payload memory but potentially unbounded metadata memory.

Targeted change: push page limits/continuation into the AVS historical lookup and retain at most the authorized page plus one look-ahead record in pDAL.

## IMPORTANT

### Physical storage interface naming and scope

`StorageBackend` is already backend-neutral and AVS types are confined to its adapter implementation. Rename/alias the contract as `IStorageBackend`, extend it with bounded history lookup, and retain compatibility where useful. Do not add a second backend.

### Explicit security and privacy hooks

The YAML policy engine is stronger than the requested development placeholder, but there is no separately named privacy hook. Preserve policy narrowing and add explicit `PolicyHook`/`PrivacyHook` extension points with clearly labeled development implementations. Production claims remain prohibited.

### Operation-oriented REST and binding normalization

Existing discovery/describe endpoints are sound, and `/pdal/v1/query` is a working history binding. Add operation-oriented `history`, `latest`, and subscription bindings that compile to `DataQuery`; keep the current endpoint as a compatibility binding. SOVD remains explicitly partial and must use the same compiler.

### Reliability/error vocabulary

The existing errors are stable but do not distinguish `NO_DATA`, `QUERY_TOO_LARGE`, or `PARTIAL_READ`. Add those stable classes while mapping raw AVS/SQLite/ROS failures to generic application-safe messages.

### Concurrency classes and backpressure

HTTP has a global connection cap, so it is not unbounded, but bulk history and small metadata/live work share the same admission limit. Add a separate bounded bulk-query gate and bounded live-subscription queues with drop accounting. Do not hold catalog locks during I/O; the catalog is immutable after startup already.

### Observability and performance comparison

Current JSONL audit events include policy/planning/backend-query latency, bytes, CPU, memory, and hot-tier attribution. Extend them with resource resolution, storage read, representation, stream, live setup/delivery/drop metrics, and hot/cold attribution. Add a benchmark executable for direct AVS versus pDAL framework overhead and discovery/describe/live control paths.

## LATER

- Additional storage backends; only `AvsDataBackend` is implemented now.
- Full VSS/VISS mapping. The descriptor will support semantic references, but custom stream IDs will not be represented as official VSS paths.
- Full SOVD, VISS, or uProtocol implementations.
- Production identity, consent, privacy transformations, TLS/mTLS, key management, retention/deletion, and regulatory evidence.
- Derived resources or controlled near-data `DataFunction` execution.
- General query optimization, distributed caches, microservices, or message brokers.

## Intended final dependency direction

```text
PdalClient / REST / SOVD binding
              |
           DataQuery
              |
          QueryEngine
              |
   cached ResourceCatalog + hooks
              |
       lightweight QueryPlan
          /          \
     HISTORY       LATEST/SUBSCRIBE
        |                 |
 IStorageBackend   ILiveDataSource
        |                 |
 AvsDataBackend   RosLiveDataSource
        |                 |
 AVS SSD/HDD       current vehicle
```

No application-visible object in this path contains a ROS topic, AVS reference, storage tier, trip, file, index, path, or offset.

## Implementation status after review

The CRITICAL items above are implemented: stable semantic/SDK contracts, canonical resource descriptors and aliases, operation routing, optional ROS live access, unified AVS SSD/HDD retrieval, and bounded historical cursors. IMPORTANT items implemented in this change include explicit hooks, operation-oriented REST translators, the expanded stable error vocabulary, separate bulk/live bounds, aggregate observability, architecture tests, a direct-AVS comparison benchmark, and the production security/privacy TODO.

Items in LATER remain intentionally unimplemented. The legacy protected multi-resource history pipeline is retained for compatibility; new SDK and operation endpoints use `DataQuery`/`QueryEngine` directly.
