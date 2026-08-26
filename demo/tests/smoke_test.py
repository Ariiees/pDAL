#!/usr/bin/env python3
"""End-to-end demo smoke test; run from the host against the Pi gateway."""

from __future__ import annotations

import argparse
import json
import math
import struct
import urllib.error
import urllib.parse
import urllib.request


def get(base: str, path: str, **query):
    url = base.rstrip("/") + path
    if query:
        url += "?" + urllib.parse.urlencode(query)
    try:
        with urllib.request.urlopen(url, timeout=60) as response:
            return response.status, dict(response.headers.items()), response.read()
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers.items()), error.read()


def json_get(base: str, path: str, **query):
    status, headers, body = get(base, path, **query)
    return status, headers, json.loads(body)


def records(body: bytes):
    assert body[:8] == b"PDALSTR1"
    offset = 8
    result = []
    while offset < len(body):
        metadata_length, payload_length = struct.unpack_from(">IQ", body, offset)
        offset += 12
        metadata = json.loads(body[offset: offset + metadata_length])
        offset += metadata_length
        payload = body[offset: offset + payload_length]
        offset += payload_length
        result.append((metadata, payload))
    assert offset == len(body)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pi-url", default="http://127.0.0.1:8090")
    args = parser.parse_args()
    base = args.pi_url

    status, _, health = json_get(base, "/api/health")
    assert status == 200 and health["status"] == "ready"
    assert health["decode_location"] == "host"

    status, _, catalog = json_get(base, "/api/trips", role="fleet_analyst")
    assert status == 200 and catalog["recordings"]
    assert catalog["access"]["position"]["authorized"] is True
    assert catalog["access"]["camera.front"]["authorized"] is False
    assert catalog["access"]["lidar.top"]["authorized"] is False
    trip = catalog["recordings"][0]
    assert trip["events"] == []
    assert trip["candidate_bytes"] > 0

    status, _, denied = json_get(
        base,
        "/api/denial-proof",
        role="fleet_analyst",
        trip=trip["id"],
        resource="camera.front",
    )
    assert status == 403 and denied["allowed"] is False
    assert denied["pdal_status"] == 403

    modalities = {item["resource"]: item for item in trip["modalities"]}
    gps = modalities["position"]
    status, headers, body = get(
        base,
        "/api/history",
        role="fleet_analyst",
        trip=trip["id"],
        resource="position",
        start_ns=gps["start_ns"],
        end_ns=gps["end_ns"],
        every_n=max(1, gps["record_count"] // 30),
        max_records=100,
    )
    assert status == 200
    gps_records = records(body)
    assert 1 <= len(gps_records) <= 100
    latitude, longitude, altitude, _, _, _ = struct.unpack("<6d", gps_records[0][1])
    assert math.isfinite(latitude) and -90 <= latitude <= 90
    assert math.isfinite(longitude) and -180 <= longitude <= 180
    assert math.isfinite(altitude)
    assert int(headers["X-Demo-Network-Bytes"]) == len(body)

    requested = str((int(trip["start_ns"]) + int(trip["end_ns"])) // 2)
    status, headers, body = get(
        base,
        "/api/closest",
        role="service_technician",
        trip=trip["id"],
        resource="camera.front",
        t_ns=requested,
    )
    assert status == 200
    camera_records = records(body)
    assert len(camera_records) == 1 and camera_records[0][1][:2] == b"\xff\xd8"
    assert int(headers["X-Demo-Record-Timestamp"]) - int(requested) == int(headers["X-Demo-Delta-Ns"])

    status, headers, body = get(
        base,
        "/api/closest",
        role="incident_investigator",
        trip=trip["id"],
        resource="lidar.top",
        t_ns=requested,
    )
    assert status == 200
    lidar_records = records(body)
    assert len(lidar_records) == 1 and lidar_records[0][1][:4] == b"LASF"
    assert int(headers["X-Demo-Network-Bytes"]) == len(body)

    status, _, timeline = json_get(
        base,
        "/api/timeline",
        role="incident_investigator",
        trip=trip["id"],
    )
    assert status == 200
    assert timeline["tracks"]["camera.front"]
    assert timeline["tracks"]["lidar.top"]

    print(
        "PASS OEM demo smoke: discovery, real 403, raw GPS/JPEG/LAZ, "
        "timeline, and measured bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
