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


def get(base: str, path: str, token: str = "", pi_key: str = "", **query):
    url = base.rstrip("/") + path
    if query:
        url += "?" + urllib.parse.urlencode(query)
    headers: dict[str, str] = {}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if pi_key:
        headers["X-Demo-Key"] = pi_key
    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=60) as response:
            return response.status, dict(response.headers.items()), response.read()
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers.items()), error.read()


def post(base: str, path: str, body: dict, token: str = "", pi_key: str = ""):
    url = base.rstrip("/") + path
    data = json.dumps(body).encode()
    headers: dict[str, str] = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if pi_key:
        headers["X-Demo-Key"] = pi_key
    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            return response.status, dict(response.headers.items()), response.read()
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers.items()), error.read()


def json_get(base: str, path: str, token: str = "", pi_key: str = "", **query):
    status, headers, body = get(base, path, token=token, pi_key=pi_key, **query)
    return status, headers, json.loads(body)


def json_post(base: str, path: str, body: dict, token: str = "", pi_key: str = ""):
    status, headers, resp = post(base, path, body, token=token, pi_key=pi_key)
    return status, headers, json.loads(resp)


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
    parser.add_argument("--pi-key", default="", help="Gateway key (PI_GATEWAY_KEY / DEMO_PROTECT_GATEWAY)")
    args = parser.parse_args()
    base = args.pi_url
    key = args.pi_key

    # ── Health (always public, no key required) ───────────────────────────────
    status, _, health = json_get(base, "/api/health")
    assert status == 200 and health["status"] == "ready", f"health failed: {health}"
    assert health["decode_location"] == "host"

    # ── Auth: unauthenticated requests must be rejected ───────────────────────
    status, _, _ = get(base, "/api/trips", pi_key=key)
    assert status == 401, f"Expected 401 without token on /api/trips, got {status}"

    # ── Auth: wrong password must be rejected ─────────────────────────────────
    status, _, _ = json_post(base, "/api/login",
                             {"role": "fleet_analyst", "password": "wrong-password"}, pi_key=key)
    assert status == 401, f"Expected 401 for wrong password, got {status}"

    # ── Login as each demo role ───────────────────────────────────────────────
    status, _, login_data = json_post(base, "/api/login",
                                      {"role": "fleet_analyst", "password": "fleet-demo"}, pi_key=key)
    assert status == 200 and login_data.get("token"), f"fleet_analyst login failed: {login_data}"
    fleet_token = login_data["token"]

    status, _, login_data = json_post(base, "/api/login",
                                      {"role": "service_technician", "password": "service-demo"}, pi_key=key)
    assert status == 200 and login_data.get("token"), f"service_technician login failed: {login_data}"
    service_token = login_data["token"]

    status, _, login_data = json_post(base, "/api/login",
                                      {"role": "incident_investigator", "password": "incident-demo"}, pi_key=key)
    assert status == 200 and login_data.get("token"), f"incident_investigator login failed: {login_data}"
    investigator_token = login_data["token"]

    # ── Trip catalog (fleet_analyst) ──────────────────────────────────────────
    status, _, catalog = json_get(base, "/api/trips", token=fleet_token, pi_key=key)
    assert status == 200 and catalog["recordings"], f"trips failed: {catalog}"
    assert catalog["access"]["position"]["authorized"] is True
    assert catalog["access"]["camera.front"]["authorized"] is False
    assert catalog["access"]["lidar.top"]["authorized"] is False
    trip = catalog["recordings"][0]
    assert trip["events"] == []
    assert trip["candidate_bytes"] > 0

    # ── Access matrix (fleet_analyst) ─────────────────────────────────────────
    status, _, access_data = json_get(base, "/api/access", token=fleet_token, pi_key=key,
                                      trip=trip["id"])
    assert status == 200, f"access failed: {access_data}"
    res = access_data["resources"]
    assert res["position"]["authorized"] is True, "fleet_analyst should have GPS"
    assert res["camera.front"]["authorized"] is False, "fleet_analyst should not have camera"
    assert res["camera.front"]["status"] == 403, f"expected 403, got {res['camera.front']['status']}"

    # ── Real pDAL denial proof (logged in as fleet_analyst) ───────────────────
    status, _, denied = json_get(base, "/api/denial-proof", token=fleet_token, pi_key=key,
                                 trip=trip["id"], resource="camera.front")
    assert status == 403 and denied["allowed"] is False, f"denial-proof unexpected: {denied}"
    assert denied["pdal_status"] == 403

    # ── GPS history ───────────────────────────────────────────────────────────
    modalities = {item["resource"]: item for item in trip["modalities"]}
    gps = modalities["position"]
    status, headers, body = get(base, "/api/history", token=fleet_token, pi_key=key,
                                trip=trip["id"], resource="position",
                                start_ns=gps["start_ns"], end_ns=gps["end_ns"],
                                every_n=max(1, gps["record_count"] // 30), max_records=100)
    assert status == 200
    gps_records = records(body)
    assert 1 <= len(gps_records) <= 100
    latitude, longitude, altitude, _, _, _ = struct.unpack("<6d", gps_records[0][1])
    assert math.isfinite(latitude) and -90 <= latitude <= 90
    assert math.isfinite(longitude) and -180 <= longitude <= 180
    assert math.isfinite(altitude)
    assert int(headers["X-Demo-Network-Bytes"]) == len(body)

    # ── Camera closest (service_technician) ───────────────────────────────────
    requested = str((int(trip["start_ns"]) + int(trip["end_ns"])) // 2)
    status, headers, body = get(base, "/api/closest", token=service_token, pi_key=key,
                                trip=trip["id"], resource="camera.front", t_ns=requested)
    assert status == 200
    camera_records = records(body)
    assert len(camera_records) == 1 and camera_records[0][1][:2] == b"\xff\xd8"
    assert int(headers["X-Demo-Record-Timestamp"]) - int(requested) == int(headers["X-Demo-Delta-Ns"])

    # ── LiDAR closest (incident_investigator) ────────────────────────────────
    status, headers, body = get(base, "/api/closest", token=investigator_token, pi_key=key,
                                trip=trip["id"], resource="lidar.top", t_ns=requested)
    assert status == 200
    lidar_records = records(body)
    assert len(lidar_records) == 1 and lidar_records[0][1][:4] == b"LASF"
    assert int(headers["X-Demo-Network-Bytes"]) == len(body)

    # ── Timeline (incident_investigator) ─────────────────────────────────────
    status, _, timeline = json_get(base, "/api/timeline", token=investigator_token, pi_key=key,
                                   trip=trip["id"])
    assert status == 200
    assert timeline["tracks"]["camera.front"]
    assert timeline["tracks"]["lidar.top"]

    # ── Logout ────────────────────────────────────────────────────────────────
    status, _, logout_data = json_post(base, "/api/logout", {}, token=fleet_token, pi_key=key)
    assert status == 200 and logout_data.get("ok") is True, f"logout failed: {logout_data}"

    print(
        "PASS OEM demo smoke: health, login/logout, auth enforcement, "
        "access matrix, real 403, raw GPS/JPEG/LAZ, timeline, measured bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
