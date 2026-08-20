# pDAL Phase-1 Implementation Plan

## AVS findings

- `AVS-PI/src/avs/include/avs/retrieve_api.h` is the reusable hot-tier read API. It resolves a ROS topic and inclusive nanosecond range to `avs::DataRef` values and loads one encoded payload at a time.
- `AVS-PI/src/avs/src/retrieve/retrieve_api.cpp` uses the SSD `global.sqlite3` catalog and append-log chunk indexes. pDAL will compile this existing implementation into its AVS-only adapter instead of copying its indexing logic.
- `AVS-PI/src/avs/include/avs/append_logger.h` defines the on-disk record/index structures consumed by `RetrieveAPI`; these remain confined to the AVS adapter and external AVS source build.
- `AVS-PI/src/avs/config/topics.yaml` maps the current camera, LiDAR, and GPS topics to AVS folders. pDAL adds a separate logical-resource catalog, so none of these topics or folders enter the public API.
- `AVS-PI/src/avs/config/avs_config.yaml` identifies `/home/avs/DATA/SSD` and `/home/avs/DATA/HDD`. Only the pDAL AVS adapter receives these locations.
- `AVS-PI/src/avs/src/archive/archive.cpp` and `cold_retrieve_report.py` show indexed HDD retrieval, but AVS currently exposes no reusable C++ cold-tier API. Phase 1 therefore uses the reusable SSD `RetrieveAPI`; cold support is left behind the same `StorageBackend` contract rather than duplicating archive/index logic.
- `AVS-PI/src/avs/src/offload/offload.cpp` calls `RetrieveAPI` directly and implements a separate wire protocol. pDAL does not reuse that transport because clients must pass through canonical pDAL policy and planning.
- `AVS-PI/src/avs/CMakeLists.txt` does not export a retrieval library. This project accepts `PDAL_AVS_SOURCE_ROOT` and compiles the existing retrieval source as an adapter-only dependency. No AVS file is modified.

## Mapping

| Required abstraction | pDAL implementation |
|---|---|
| Logical resources | `ResourceDescriptor` plus YAML-backed `ResourceCatalog` |
| One request model | strongly typed `PdalRequest`, shared validation and normalization |
| Interface adapters | native HTTP, local CLI, and explicitly scoped SOVD-style translators |
| Identity and policy | header/CLI principal extraction and replaceable YAML `PolicyEngine` |
| Authorized access | immutable `AuthorizedAccessPlan`; storage never receives the external request |
| Planning | `QueryPlanner` emits backend-neutral tasks and operations after policy narrowing |
| Storage | `StorageBackend` interface; `AvsStorageBackend` contains every AVS type/topic interaction |
| Representation | `RepresentationProvider` negotiates catalog formats and preserves AVS payload encoding |
| Delivery | bounded metadata or chunked binary record streaming with signed continuation tokens |
| Audit/status | structured JSONL audit events, bounded request-status registry, timing/byte counters |
| HTTP API | Boost.Beast server implementing the required `/pdal/v1` endpoints |

## Build and verification

1. Build the transport-independent core and AVS adapter as separate CMake targets.
2. Build a `pdal` executable whose `serve` and `query` commands invoke the same pipeline.
3. Add unit tests for catalog mapping, validation, policy narrowing/denial, planning, errors, and representation negotiation.
4. Add adapter-equivalence and authorization-boundary integration tests using a fake backend.
5. Run CMake/CTest locally and exercise discovery and query endpoints against the test backend where hardware data is unavailable.

## Explicit compatibility boundary

All new and modified files live in `/home/avs/pDAL`. `AVS-PI` remains unchanged. A future cold-tier vertical slice needs a reusable AVS cold-query API (parallel to `RetrieveAPI`) or an exported unified hot/cold retrieval library; implementing the archive index again inside pDAL would violate the storage ownership boundary.
