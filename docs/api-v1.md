# pDAL API v1

Base path: `/pdal/v1`.

| Method | Path | Semantic operation |
|---|---|---|
| `GET` | `/pdal/v1/resources` | `DISCOVER` |
| `GET` | `/pdal/v1/resources/{id}` | `DESCRIBE` |
| `POST` | `/pdal/v1/resources/{id}/history` | `HISTORY` |
| `GET` | `/pdal/v1/resources/{id}/latest` | `LATEST` |
| `GET` | `/pdal/v1/resources/{id}/subscribe` | `SUBSCRIBE` |
| `POST` | `/pdal/v1/query` | compatible protected bulk history |
| `POST` | `/sovd/v1/bulk-data/query` | experimental SOVD-style history binding |
| `GET` | `/pdal/v1/capabilities` | compatible capability document |
| `GET` | `/pdal/v1/requests/{request_id}` | compatible bulk-request status |

The SDK, operation-oriented REST endpoints, and SOVD adapter normalize to the same `DataQuery` semantics. Bindings never call AVS or ROS directly.

## Authentication

Every endpoint except `GET /pdal/v1` and `GET /pdal/v1/capabilities` requires:

```
Authorization: Bearer <token>
```

The token is an `HS256`-signed compact JWT. pDAL verifies the signature,
`iss`, `aud`, and `exp`, then builds the principal from the `sub`, `role`, and
`org` claims. `X-PDAL-Principal` / `X-PDAL-Organization` / `X-PDAL-Role` are
ignored. Failures return `PDAL_UNAUTHENTICATED` (HTTP 401) before any catalog
or backend access. The `role` claim selects the authorization policy; a
disallowed role / purpose / resource / time returns `PDAL_FORBIDDEN` (HTTP 403)
before payload access. See [security.md](security.md).

`LATEST` and `SUBSCRIBE` accept an optional `?purpose=<p>` query parameter
(default `development`) that is checked against the role's allowed purposes.

## History request

The resource comes from the URL and cannot be overridden by the body.

```json
{
  "request_id": "optional-correlation-id",
  "time": {"start_ns": 1770307702634771758, "end_ns": 1770307703634771758},
  "purpose": "development",
  "representation": {
    "format": "native",
    "sampling": {"every_n": 1},
    "transformations": []
  },
  "delivery": {"max_records": 100, "max_bytes": 67108864}
}
```

Ranges are inclusive. `native` selects the stored representation. `metadata` and `metadata-only` return frames with zero payload length and public `payload_size` metadata without reading sensor payloads.

`LATEST` and `SUBSCRIBE` currently accept the default native representation. Latest produces a finite one-element stream or `PDAL_NO_DATA`; subscribe remains open until cancellation/disconnection.

## Record stream

All operation-oriented data endpoints use `application/vnd.pdal.record-stream; version=1` with HTTP chunked transfer. HTTP chunk boundaries are not record boundaries. Concatenate chunks and parse:

```text
8 bytes   ASCII "PDALSTR1"

repeated frame:
4 bytes   metadata JSON length, unsigned big-endian
8 bytes   payload length, unsigned big-endian
N bytes   UTF-8 public sample metadata
M bytes   stored/negotiated payload
```

Frame metadata contains `resource_id`, `timestamp_ns`, `representation`, `content_type`, and `payload_size`, plus source-neutral stream metrics when available. It never contains a backend, topic, tier, trip, path, or offset. Response headers include `X-PDAL-Request-ID`, `X-PDAL-Resource-ID`, and `X-PDAL-Operation`.

The compatible `/pdal/v1/query` endpoint retains its JSON metadata mode, signed continuation tokens, multi-resource request envelope, and YAML role policy.

## Public resource descriptor

A descriptor exposes canonical ID, kind (`SIGNAL`, `STREAM`, or `OBJECT`), display name, description, schema, optional external semantic references, supported operations, representations, availability, and query limits. `historical_available` and `live_available` are booleans; their bindings are private.

The supplied canonical IDs are:

- `camera.front`
- `lidar.top`
- `position`

Legacy `vehicle.camera.front`, `vehicle.lidar.top`, and `vehicle.position` inputs resolve to the canonical identities but are not listed by discovery.

## Stable errors

Errors use a JSON envelope with `code`, `class`, safe message, request ID, and optional safe details. The vocabulary is:

- `PDAL_UNAUTHENTICATED` (missing / invalid bearer token; HTTP 401)
- `PDAL_FORBIDDEN` (authenticated but not authorized; HTTP 403)
- `PDAL_RESOURCE_NOT_FOUND`
- `PDAL_INVALID_QUERY` (semantic `DataQuery` validation)
- `PDAL_INVALID_REQUEST` (malformed binding input and legacy compatibility)
- `PDAL_NO_DATA`
- `PDAL_NOT_SUPPORTED`
- `PDAL_REPRESENTATION_NOT_SUPPORTED`
- `PDAL_BACKEND_UNAVAILABLE`
- `PDAL_QUERY_TOO_LARGE`
- `PDAL_PARTIAL_READ`
- `PDAL_INTERNAL_ERROR`

The compatible protected endpoint also retains authentication/authorization and range-specific errors.

## SOVD-style mapping

`POST /sovd/v1/bulk-data/query` is not a full ASAM SOVD implementation. Its `resource`, `timeRange`, `reason`, `contentFormat`, `limit`, and `maxBytes` fields compile to a history `DataQuery` equivalent to native REST/SDK semantics; it has no direct storage path.
