# Architecture

## Protected execution path

```text
External Client
      |
      v
Interface Adapter (native HTTP, local CLI, or SOVD-style)
      |
      v
Canonical PdalRequest
      |
      v
Identity + PolicyEngine
      |
      v
immutable AuthorizedAccessPlan
      |
      v
QueryPlanner + ResourceCatalog
      |
      v
backend-neutral ExecutionPlan
      |
      v
StorageBackend -> AvsStorageBackend -> AVS RetrieveAPI
      |
      v
RepresentationProvider + record streaming
      |
      v
Canonical metadata / binary record-stream response
```

The dependency direction is intentional. A storage backend receives only an authorized execution task. It never receives an HTTP/SOVD request or the original request before policy narrowing.

## Separation of responsibilities

`PdalRequest` is the single semantic request model. HTTP JSON, the local CLI, and the experimental SOVD-style endpoint normalize into it and then call the same pipeline. A new transport implements a translator; it does not add query semantics.

`ResourceCatalog` owns the private binding from `vehicle.camera.front` to an AVS topic. Public catalog serialization omits `backend.source`. A topic, folder, trip, `.log`, `.idx`, SQLite row, or byte offset is never a pDAL resource identity.

`PolicyEngine` computes the intersection of the requested access and the role policy. It can remove resources, intersect time ranges, cap record/byte limits, and remove transformations. The resulting `AuthorizedAccessPlan` has getters only and contains an issue time, expiry, and policy version so a future signer can attest it without changing its meaning.

`QueryPlanner` turns that access plan into backend-neutral operations and resource tasks. Narrowed time ranges reach `StorageBackend::Query`; broad data is not intentionally retrieved and filtered after authorization.

`AvsStorageBackend` is the only pDAL component that includes an AVS header or sees `avs::DataRef`. It converts references immediately to opaque `BackendRecord` locators. The pDAL core is independent of ROS and AVS on-disk structures.

`RepresentationProvider` treats logical identity and representation separately. Phase 1 passes through stored JPEG, LAZ, and compact AVS GPS bytes. Unsupported quality changes and transforms return a stable error rather than silently changing bytes.

## What pDAL is not

- **SOVD is not pDAL.** The SOVD-style route is one translator and makes no claim of complete ASAM SOVD compliance.
- **AVS is not pDAL.** AVS owns storage/indexing; pDAL owns stable resource semantics, protection, planning, and delivery.
- **HTTP transport is not pDAL semantics.** The local CLI uses the same in-process model and protected execution path.

## Streaming and bounded operation

AVS payloads are loaded one record at a time and immediately sent to a sink. Result payload sets are never accumulated. Policy and request limits bound records and bytes. HTTP uses chunked transfer encoding and a self-framing pDAL binary body. Signed, request-bound continuation tokens carry only an opaque result offset and expiry.

The current AVS API returns all matching `DataRef` metadata before pDAL applies record/byte pagination. Payload memory remains bounded, but an AVS API accepting a maximum record count would permit earlier reference pruning. No AVS change was made because all implementation work was required to remain in this pDAL workspace.

## Concurrency and observability

The core contains no mutable global state. The registry and JSONL audit sink synchronize their bounded/shared state. The HTTP server caps simultaneous connections. Structured events cover receipt, normalization, policy, planning, backend selection/query, returned bytes, completion, and failures; they do not include payloads or backend locators.

## Storage tiers

Clients cannot select SSD or HDD. Tier placement is a backend concern. The current reusable AVS C++ API is hot-tier-only, so the AVS adapter truthfully advertises hot support and no cold support. Adding an AVS-owned cold/unified API only changes `AvsStorageBackend`; all adapters and canonical semantics remain unchanged.
