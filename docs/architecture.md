# Architecture

pDAL is a modular monolith. Its modules are logical C++ boundaries in one process; there is no internal HTTP, RPC, IPC, broker, or service mesh.

```text
Application
    |
PdalClient / REST / SOVD-style binding
    |
DataQuery (resource + operation + selector + representation + limits + context)
    |
QueryEngine
    +-- immutable, cached ResourceCatalog
    +-- PolicyHook -> PrivacyHook
    +-- deterministic representation negotiation and routing
    |
    +-- HISTORY ----------> IStorageBackend
    |                           |
    |                     AvsStorageBackend
    |                           |
    |               AVS HistoricalRetrieveAPI
    |                      /             \
    |                 SSD hot          HDD cold
    |
    +-- LATEST/SUBSCRIBE -> ILiveDataSource
                                |
                         RosLiveDataSource
                                |
                              ROS 2
```

## Stable contracts

The application contract is `PdalClient` and `ResourceHandle`. The semantic contract is `ResourceDescriptor`, `DataQuery`, `QueryOptions`, `RepresentationRequest`, `DataSample`, `DataResult`, and move-only `DataStream`. None contains AVS or ROS implementation types or physical binding fields. `CatalogResource` is a separate internal record that adds historical/live bindings and compatibility aliases.

The physical contracts are `IStorageBackend` and `ILiveDataSource`. `AvsStorageBackend` is the only implemented historical backend. Another storage backend is intentionally not included. `RosLiveDataSource` is an optional target so the semantic core has no ROS dependency.

The older multi-resource `PdalRequest`/`PdalPipeline` remains as a compatible protected bulk-history binding. New SDK and operation-oriented REST paths converge on `DataQuery` and `QueryEngine`.

## Resource identity and discovery

`camera.front`, `lidar.top`, and `position` remain identical across history, latest, and subscription operations. Representation is independent of identity. `native` selects an already-stored representation; metadata suppresses payload reads. The public resource serializer exposes schema, semantic references, operations, representations, availability, and limits, while deliberately omitting both bindings and aliases.

The YAML catalog is parsed once at startup and then used immutably. Physical topic/folder/message mappings never enter a query or a public result.

## Historical path

The AVS adapter pushes the inclusive time range and page limit to AVS indexes. A history cursor retains at most a requested reference page. `DataStream::Next()` loads one payload, hands ownership to the caller, and releases it when the sample leaves scope. Metadata queries return timestamps and sizes without calling the payload reader.

AVS `HistoricalRetrieveAPI` composes the existing hot `RetrieveAPI` with AVS-owned cold archive lookup. It merges records in timestamp order, prefers the hot locator when migration temporarily leaves the same timestamp in both tiers, and keeps tier/path/offset state behind the adapter locator. Existing AVS `RetrieveAPI::QueryRefs` remains hot-only and source-compatible.

## Live path

`RosLiveDataSource` owns the ROS node, topics, QoS, typed callbacks, executor, and message conversions. Camera samples are JPEG, LiDAR samples use the AVS LAZ compressor, and position samples use the same compact representation advertised for history.

Each subscription has a byte-bounded queue. Oldest samples are dropped under pressure and the next delivered sample carries aggregate drop accounting. A global subscription cap and a single ROS executor thread prevent unbounded threads and memory. `LATEST` uses a one-sample cache owned by the live adapter.

## Concurrency and reliability

Historical bulk streams pass through a configurable admission gate. Discovery and describe never enter that gate, so control requests remain responsive during bulk reads. HTTP connections, bulk queries, live subscriptions, response bytes, record counts, historical ranges, and live queues all have configured bounds.

Storage and live exceptions are mapped to stable pDAL errors. Short payloads are `PARTIAL_READ`; no-current-sample is `NO_DATA`. Streams support cancellation and release admission/subscription resources on destruction. Catalog locks are not held during storage reads or socket writes.

## Security, observability, and future extensions

The HTTP layer authenticates the caller before dispatch: every endpoint except
`GET /pdal/v1` and `GET /pdal/v1/capabilities` requires an `HS256` bearer token,
and the `Principal` is built only from verified `sub` / `role` / `org` claims.
Policy/privacy hooks execute after semantic validation and resource resolution
but before physical access. The policy hook (`EnginePolicyHook`) delegates to the
same `YamlPolicyEngine` used by the legacy bulk path, so both paths enforce one
role/purpose/resource/time/record/byte decision and deny with `403` before
`OpenHistory`, `ReadPayload`, or a live subscription. The privacy hook is still
the development `NoOpPrivacy`. See [security.md](security.md).

Audit events use a single request ID and record control/setup latency, selected/considered records, read/returned bytes, internal hot/cold attribution, stream duration, CPU/memory, and aggregate live delivery/drop metrics. Sensor payloads and physical locators are never logged.

Future storage backends implement `IStorageBackend`; future bindings compile into `DataQuery`. Controlled derived resources/data functions remain a future concept—pDAL v1 does not execute arbitrary application code.
