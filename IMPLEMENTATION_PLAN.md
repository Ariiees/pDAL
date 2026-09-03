# pDAL implementation map

This file records the implemented module mapping. The pre-change gap analysis and CRITICAL/IMPORTANT/LATER classification are in `PDAL_ARCHITECTURE_REVIEW.md`.

| Responsibility | Implementation |
|---|---|
| Stable application API | `PdalClient`, generic `ResourceHandle`, range/callback stream helpers |
| Semantic contract | physical-detail-free `ResourceDescriptor`, `DataQuery`, `QueryOptions`, `DataSample`, `DataResult`, `DataStream` |
| Private mappings | immutable YAML `ResourceCatalog` and internal `CatalogResource` |
| Runtime routing | `QueryEngine`: HISTORY to storage; LATEST/SUBSCRIBE to live source |
| Historical boundary | `IStorageBackend` (`StorageBackend` compatibility name) and bounded `HistoryCursor` |
| Historical implementation | `AvsStorageBackend` using AVS-owned `HistoricalRetrieveAPI` for unified SSD/HDD lookup |
| Live boundary | `ILiveDataSource` and registry |
| Live implementation | optional `RosLiveDataSource` with typed callbacks and byte-bounded queues |
| Bindings | native REST, local CLI, and experimental SOVD translator converge on semantic query types |
| Authentication | `Authenticator` in the HTTP layer; `BearerTokenAuthenticator` (HS256 JWT: signature, `iss`, `aud`, `exp`; principal from `sub`/`role`/`org`); 401 before dispatch; `X-PDAL-*` ignored |
| Authorization | one `YamlPolicyEngine` shared by `PdalPipeline` and `QueryEngine` via `EnginePolicyHook` (narrow-only, enforced before backend access) |
| Security extension | `PolicyHook` production `EnginePolicyHook`; `PrivacyHook` still development-only `NoOpPrivacy` (narrows `DataQuery` only) |
| Camera privacy | in-process `PrivacyStage` (`src/privacy/human_blur.cpp`): YOLOv8n person detection via ONNX Runtime + Gaussian blur + JPEG re-encode, after `ReadPayload` / before `DataSample` on both paths; fail closed; GPS/LiDAR untouched |
| Compatibility | retained multi-resource `PdalRequest`/`PdalPipeline`, YAML policy, continuation, status, and binary framing |
| Verification | semantic/routing/isolation tests, synthetic hot/cold test, sanitizers, ROS/AVS builds, direct-AVS benchmark |

The only AVS source changes are the minimal bounded hot query, unified historical read API, and exported retrieval library target. Existing hot-only `RetrieveAPI::QueryRefs` callers retain their API and were rebuilt successfully with the full AVS package.

Future storage backends, production security/privacy, standards-complete protocol bindings, and derived computation remain intentionally unimplemented.
