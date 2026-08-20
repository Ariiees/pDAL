# pDAL API v1

Base path: `/pdal/v1`

## Endpoints

| Method | Path | Result |
|---|---|---|
| `GET` | `/pdal/v1` | API identity |
| `GET` | `/pdal/v1/capabilities` | resources, representations, query features, modes, limits, extensions |
| `GET` | `/pdal/v1/resources` | public logical-resource catalog |
| `GET` | `/pdal/v1/resources/{resource_id}` | one public resource descriptor |
| `POST` | `/pdal/v1/query` | metadata JSON or streamed records |
| `GET` | `/pdal/v1/requests/{request_id}` | bounded in-memory request status |
| `GET` | `/pdal/v1/demo` | demonstration browser client |

Discovery endpoints reveal logical metadata only. Query identity comes from `X-PDAL-Principal`, `X-PDAL-Organization`, and `X-PDAL-Role`. This header mechanism is a replaceable development boundary, not cryptographic authentication.

## Canonical query JSON

```json
{
  "api_version": "pdal/v1",
  "request_id": "optional-client-correlation-id",
  "resources": ["vehicle.camera.front"],
  "time": {
    "start_ns": 1766013563038953891,
    "end_ns": 1766013568038953891
  },
  "purpose": "incident-investigation",
  "representation": {
    "format": "jpeg",
    "sampling": {"every_n": 1},
    "transformations": []
  },
  "delivery": {
    "mode": "stream",
    "max_records": 100,
    "max_bytes": 67108864,
    "continuation_token": null
  },
  "context": {}
}
```

Unknown optional fields are ignored. Required fields and bounded limits are validated. Time ranges are inclusive. `native` chooses the stored representation for each resource; `metadata`/`metadata-only` suppresses payload reads.

## Metadata response

With `delivery.mode = metadata`, the response is `application/json`. It contains the authorized time range, negotiated representation per resource, selected record count, continuation, policy version, and record timestamps/sizes. It contains no payload or backend locator.

## Binary stream response

Stream queries return `application/vnd.pdal.record-stream; version=1` using HTTP chunked transfer. HTTP chunk boundaries have no record meaning. Concatenate the body chunks and parse:

```text
8 bytes   ASCII "PDALSTR1"

repeated record frame:
4 bytes   metadata JSON length, unsigned big-endian
8 bytes   payload length, unsigned big-endian
N bytes   UTF-8 metadata JSON
M bytes   stored/negotiated payload
```

Frame metadata contains only `resource_id`, `timestamp_ns`, `representation`, `content_type`, and `payload_size`. The response headers include `X-PDAL-Request-ID` and `X-PDAL-Metadata`. Payloads are streamed one AVS record at a time.

## Continuation

When more authorized records remain, response metadata includes an opaque continuation token. Repeat the same semantic request with that token in `delivery.continuation_token`. The HMAC-protected token is bound to principal, purpose, resources, range, representation, and delivery limits and has a short expiry. Modified, expired, or mismatched tokens return `PDAL_INVALID_REQUEST`.

## Stable errors

```json
{
  "error": {
    "code": "PDAL_RESOURCE_NOT_FOUND",
    "class": "resource_not_found",
    "message": "unknown logical resource: vehicle.unknown",
    "request_id": "...",
    "details": {"resource_id": "vehicle.unknown"}
  }
}
```

Stable classes are `invalid_request`, `unauthenticated`, `forbidden`, `resource_not_found`, `representation_not_supported`, `range_not_satisfiable`, `backend_unavailable`, and `internal_error`.

## SOVD-style adapter

`POST /sovd/v1/bulk-data/query` is an experimental compatibility adapter, not a complete ASAM SOVD implementation. Its supported input is:

```json
{
  "resource": "vehicle.camera.front",
  "timeRange": {"from": 1766013563038953891, "to": 1766013568038953891},
  "reason": "incident-investigation",
  "contentFormat": "jpeg",
  "deliveryMode": "stream",
  "limit": 100,
  "maxBytes": 67108864,
  "continuationToken": null
}
```

It translates to `PdalRequest` and then follows the same identity, policy, access-plan, planning, backend, and response path as the native endpoint.
