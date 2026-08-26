#!/usr/bin/env python3
"""Thin OEM demo gateway: AVS discovery, pDAL queries, and raw record transfer.

This process intentionally does not decode GPS, JPEG, or LAZ payloads. Sensor
record parsing and rendering belong to the host viewer.
"""

from __future__ import annotations

import argparse
import json
import os
import sqlite3
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


RESOURCE_INFO = {
    "position": {"folder": "gps_right", "label": "GPS", "format": "position-binary-v1"},
    "camera.front": {"folder": "camera_front", "label": "Front camera", "format": "jpeg"},
    "lidar.top": {"folder": "lidar_top", "label": "LiDAR", "format": "laz"},
}
FOLDER_TO_RESOURCE = {value["folder"]: key for key, value in RESOURCE_INFO.items()}
TOPIC_TO_RESOURCE = {
    "/novatel/oem7/gps": "position",
    "/my_camera/pylon_ros2_camera_node/image_raw": "camera.front",
    "/sensing/lidar/top/pointcloud": "lidar.top",
}
ROLE_INFO = {
    "fleet_analyst": {
        "label": "Fleet Analyst",
        "purpose": "fleet-monitoring",
        "principal": "oem-fleet-analyst",
        "description": "Fleet operations · GPS only",
    },
    "service_technician": {
        "label": "Service Technician",
        "purpose": "diagnostics",
        "principal": "oem-service-technician",
        "description": "Diagnostics · GPS + retained camera",
    },
    "incident_investigator": {
        "label": "Incident Investigator",
        "purpose": "incident-investigation",
        "principal": "oem-incident-investigator",
        "description": "Investigation · GPS + camera + LiDAR",
    },
}


class UpstreamError(Exception):
    def __init__(self, status: int, body: bytes, headers: dict[str, str] | None = None):
        super().__init__(f"pDAL returned HTTP {status}")
        self.status = status
        self.body = body
        self.headers = headers or {}


class PdalClient:
    def __init__(self, base_url: str, timeout: float = 30.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def health(self) -> dict[str, Any]:
        body, _, _ = self._request("GET", "/pdal/v1")
        return json.loads(body)

    def query(
        self,
        role: str,
        resource: str,
        start_ns: int,
        end_ns: int,
        *,
        metadata: bool,
        every_n: int = 1,
        max_records: int = 50000,
        max_bytes: int = 268435456,
    ) -> tuple[bytes, dict[str, str], int]:
        profile = role_profile(role)
        representation: dict[str, Any] = {
            "format": "metadata" if metadata else RESOURCE_INFO[resource]["format"],
            "sampling": {"every_n": max(1, every_n)},
            "transformations": [],
        }
        request = {
            "purpose": profile["purpose"],
            "resources": [resource],
            "time": {"start_ns": str(start_ns), "end_ns": str(end_ns)},
            "representation": representation,
            "delivery": {
                "mode": "metadata" if metadata else "stream",
                "max_records": max_records,
                "max_bytes": max_bytes,
            },
        }
        headers = {
            "Content-Type": "application/json",
            "X-PDAL-Principal": profile["principal"],
            "X-PDAL-Organization": "oem-demo",
            "X-PDAL-Role": role,
        }
        return self._request(
            "POST", "/pdal/v1/query", json.dumps(request).encode(), headers
        )

    def _request(
        self,
        method: str,
        path: str,
        body: bytes | None = None,
        headers: dict[str, str] | None = None,
    ) -> tuple[bytes, dict[str, str], int]:
        request = urllib.request.Request(
            self.base_url + path, data=body, headers=headers or {}, method=method
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                return response.read(), dict(response.headers.items()), response.status
        except urllib.error.HTTPError as error:
            raise UpstreamError(
                error.code, error.read(), dict(error.headers.items())
            ) from error


def role_profile(role: str) -> dict[str, str]:
    try:
        return ROLE_INFO[role]
    except KeyError as error:
        raise ValueError("unknown OEM role") from error


@dataclass(frozen=True)
class ModalitySummary:
    resource: str
    start_ns: int
    end_ns: int
    record_count: int | None
    stored_bytes: int | None


class TripCatalog:
    def __init__(self, ssd_root: Path, hdd_root: Path):
        self.ssd_root = ssd_root
        self.hdd_root = hdd_root

    def scan(self) -> list[dict[str, Any]]:
        trips: dict[tuple[str, int], dict[str, ModalitySummary]] = {}
        self._read_global(self.ssd_root / "global.sqlite3", trips, prefer=True)
        self._read_global(self.hdd_root / "global.sqlite3", trips, prefer=False)
        result: list[dict[str, Any]] = []
        for (day, trip_number), modalities in sorted(trips.items(), reverse=True):
            if not modalities:
                continue
            start_ns = min(item.start_ns for item in modalities.values())
            end_ns = max(item.end_ns for item in modalities.values())
            encoded_id = f"{day}-trip_{trip_number:02d}"
            items = []
            for resource in RESOURCE_INFO:
                item = modalities.get(resource)
                if not item:
                    continue
                items.append(
                    {
                        "resource": resource,
                        "label": RESOURCE_INFO[resource]["label"],
                        "start_ns": str(item.start_ns),
                        "end_ns": str(item.end_ns),
                        "record_count": item.record_count,
                        "stored_bytes": item.stored_bytes,
                    }
                )
            result.append(
                {
                    "id": encoded_id,
                    "day": day,
                    "trip_number": trip_number,
                    "label": f"{day} · Trip {trip_number + 1:02d}",
                    "start_ns": str(start_ns),
                    "end_ns": str(end_ns),
                    "duration_ns": str(end_ns - start_ns),
                    "modalities": items,
                    "candidate_bytes": sum(
                        item.stored_bytes or 0 for item in modalities.values()
                    ),
                    # Real brake records are not available. Never forward legacy
                    # GPS-derived event summaries into this OEM demo.
                    "events": [],
                }
            )
        return result

    def by_id(self, trip_id: str) -> dict[str, Any]:
        for trip in self.scan():
            if trip["id"] == trip_id:
                return trip
        raise ValueError("recording not found")

    def _read_global(
        self,
        database: Path,
        trips: dict[tuple[str, int], dict[str, ModalitySummary]],
        *,
        prefer: bool,
    ) -> None:
        if not database.exists():
            return
        connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
        try:
            columns = {
                row[1] for row in connection.execute("PRAGMA table_info(global)")
            }
            has_count = "number_of_records" in columns
            fields = (
                "sensor_topic, "
                + ("topic_folder, " if "topic_folder" in columns else "")
                + ("number_of_records, " if has_count else "")
                + "day, trip_id, start_ts_ns, end_ts_ns"
            )
            for row in connection.execute(f"SELECT {fields} FROM global"):
                position = 0
                topic = row[position]
                position += 1
                folder = None
                if "topic_folder" in columns:
                    folder = row[position]
                    position += 1
                count = None
                if has_count:
                    count = int(row[position])
                    position += 1
                day, trip_number, start_ns, end_ns = row[position: position + 4]
                resource = TOPIC_TO_RESOURCE.get(topic) or FOLDER_TO_RESOURCE.get(folder)
                if not resource:
                    continue
                key = (str(day), int(trip_number))
                current = trips.setdefault(key, {})
                if resource in current and not prefer:
                    continue
                stored_bytes = self._stored_bytes(
                    RESOURCE_INFO[resource]["folder"], str(day), int(trip_number)
                )
                current[resource] = ModalitySummary(
                    resource,
                    int(start_ns),
                    int(end_ns),
                    count,
                    stored_bytes,
                )
        finally:
            connection.close()

    def _stored_bytes(self, folder: str, day: str, trip_number: int) -> int | None:
        path = self.ssd_root / folder / day / f"trip_{trip_number:02d}.log"
        try:
            return path.stat().st_size
        except FileNotFoundError:
            return None


def parse_metadata(body: bytes) -> dict[str, Any]:
    data = json.loads(body)
    for record in data.get("records", []):
        if "timestamp_ns" in record:
            record["timestamp_ns"] = str(record["timestamp_ns"])
    for field in ("time_range",):
        value = data.get(field)
        if isinstance(value, dict):
            for key in ("start_ns", "end_ns"):
                if key in value:
                    value[key] = str(value[key])
    return data


class DemoService:
    def __init__(self, pdal: PdalClient, catalog: TripCatalog):
        self.pdal = pdal
        self.catalog = catalog
        self.started_at = time.time()

    def trips(self, role: str) -> dict[str, Any]:
        role_profile(role)
        trips = self.catalog.scan()
        if not trips:
            return {"role": role, "recordings": []}
        probe = trips[0]
        access = self.access_matrix(role, probe)
        return {"role": role, "access": access, "recordings": trips}

    def access_matrix(self, role: str, trip: dict[str, Any]) -> dict[str, Any]:
        output = {}
        available = {item["resource"]: item for item in trip["modalities"]}
        for resource in RESOURCE_INFO:
            if resource not in available:
                output[resource] = {"authorized": False, "reason": "not-recorded"}
                continue
            try:
                self.pdal.query(
                    role,
                    resource,
                    int(available[resource]["start_ns"]),
                    int(available[resource]["end_ns"]),
                    metadata=True,
                    max_records=1,
                    max_bytes=1,
                )
                output[resource] = {"authorized": True, "policy": "oem-demo-v1"}
            except UpstreamError as error:
                details = safe_error(error.body)
                output[resource] = {
                    "authorized": False,
                    "status": error.status,
                    "code": details.get("code", "PDAL_FORBIDDEN"),
                    "reason": details.get("message", "Denied by pDAL policy"),
                }
        return output

    def timeline(self, role: str, trip_id: str) -> dict[str, Any]:
        trip = self.catalog.by_id(trip_id)
        ranges = {item["resource"]: item for item in trip["modalities"]}
        access = self.access_matrix(role, trip)
        tracks: dict[str, Any] = {}
        for resource in ("camera.front", "lidar.top"):
            if resource not in ranges or not access[resource]["authorized"]:
                continue
            item = ranges[resource]
            body, _, _ = self.pdal.query(
                role,
                resource,
                int(item["start_ns"]),
                int(item["end_ns"]),
                metadata=True,
                max_records=50000,
                max_bytes=1,
            )
            metadata = parse_metadata(body)
            tracks[resource] = [
                record["timestamp_ns"] for record in metadata.get("records", [])
            ]
        return {
            "trip_id": trip_id,
            "start_ns": trip["start_ns"],
            "end_ns": trip["end_ns"],
            "tracks": tracks,
            "access": access,
        }

    def history(
        self,
        role: str,
        trip_id: str,
        resource: str,
        start_ns: int,
        end_ns: int,
        every_n: int,
        max_records: int,
    ) -> tuple[bytes, dict[str, str], int]:
        trip, modality = self._validate_selection(trip_id, resource, start_ns, end_ns)
        body, headers, status = self.pdal.query(
            role,
            resource,
            start_ns,
            end_ns,
            metadata=False,
            every_n=every_n,
            max_records=max_records,
        )
        output_headers = self._transfer_headers(trip, modality, body, headers)
        output_headers["X-Demo-Query-Kind"] = "history-window"
        return body, output_headers, status

    def closest(
        self, role: str, trip_id: str, resource: str, requested_ns: int
    ) -> tuple[bytes, dict[str, str], int]:
        trip, modality = self._validate_selection(
            trip_id, resource, requested_ns, requested_ns
        )
        trip_start = int(trip["start_ns"])
        trip_end = int(trip["end_ns"])
        radius = 2_000_000_000
        start_ns = max(trip_start, requested_ns - radius)
        end_ns = min(trip_end, requested_ns + radius)
        metadata_body, _, _ = self.pdal.query(
            role,
            resource,
            start_ns,
            end_ns,
            metadata=True,
            max_records=5000,
            max_bytes=1,
        )
        metadata = parse_metadata(metadata_body)
        records = metadata.get("records", [])
        if not records:
            raise ValueError("no retained record near the selected time")
        closest = min(
            records,
            key=lambda record: abs(int(record["timestamp_ns"]) - requested_ns),
        )
        selected_ns = int(closest["timestamp_ns"])
        body, headers, status = self.pdal.query(
            role,
            resource,
            selected_ns,
            selected_ns,
            metadata=False,
            max_records=1,
        )
        output_headers = self._transfer_headers(trip, modality, body, headers)
        output_headers.update(
            {
                "X-Demo-Query-Kind": "closest-record",
                "X-Demo-Requested-T": str(requested_ns),
                "X-Demo-Record-Timestamp": str(selected_ns),
                "X-Demo-Delta-Ns": str(selected_ns - requested_ns),
            }
        )
        return body, output_headers, status

    def denial_probe(
        self, role: str, trip_id: str, resource: str
    ) -> tuple[int, dict[str, Any]]:
        trip = self.catalog.by_id(trip_id)
        modality = next(
            (item for item in trip["modalities"] if item["resource"] == resource), None
        )
        if not modality:
            raise ValueError("modality is not present in this recording")
        try:
            self.pdal.query(
                role,
                resource,
                int(modality["start_ns"]),
                int(modality["end_ns"]),
                metadata=True,
                max_records=1,
                max_bytes=1,
            )
            return 200, {"allowed": True, "role": role, "resource": resource}
        except UpstreamError as error:
            return error.status, {
                "allowed": False,
                "role": role,
                "resource": resource,
                "pdal_status": error.status,
                "pdal_error": safe_error(error.body),
            }

    def _validate_selection(
        self, trip_id: str, resource: str, start_ns: int, end_ns: int
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        if resource not in RESOURCE_INFO:
            raise ValueError("unknown resource")
        trip = self.catalog.by_id(trip_id)
        if start_ns > end_ns:
            raise ValueError("invalid time range")
        if start_ns < int(trip["start_ns"]) or end_ns > int(trip["end_ns"]):
            raise ValueError("requested time is outside the selected recording")
        modality = next(
            (item for item in trip["modalities"] if item["resource"] == resource), None
        )
        if not modality:
            raise ValueError("modality is not present in this recording")
        return trip, modality

    @staticmethod
    def _transfer_headers(
        trip: dict[str, Any],
        modality: dict[str, Any],
        body: bytes,
        upstream: dict[str, str],
    ) -> dict[str, str]:
        return {
            "Content-Type": upstream.get(
                "Content-Type", "application/vnd.pdal.record-stream; version=1"
            ),
            "X-PDAL-Request-ID": upstream.get("X-PDAL-Request-ID", ""),
            "X-Demo-Network-Bytes": str(len(body)),
            "X-Demo-Onboard-Candidate-Bytes": str(trip["candidate_bytes"]),
            "X-Demo-Modality-Candidate-Bytes": str(modality.get("stored_bytes") or 0),
        }


def safe_error(body: bytes) -> dict[str, Any]:
    try:
        value = json.loads(body)
        return value if isinstance(value, dict) else {"message": str(value)}
    except Exception:
        return {"message": "pDAL rejected the request"}


class GatewayHandler(BaseHTTPRequestHandler):
    server_version = "pdal-oem-gateway/1"

    @property
    def service(self) -> DemoService:
        return self.server.service  # type: ignore[attr-defined]

    def do_GET(self) -> None:
        parsed = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        try:
            if parsed.path == "/api/health":
                pdal_health = self.service.pdal.health()
                self._json(
                    200,
                    {
                        "status": "ready",
                        "component": "pi-gateway",
                        "pdal": pdal_health,
                        "decode_location": "host",
                        "uptime_seconds": round(time.time() - self.service.started_at, 1),
                    },
                )
            elif parsed.path == "/api/roles":
                self._json(
                    200,
                    {
                        "roles": [dict({"id": key}, **value) for key, value in ROLE_INFO.items()]
                    },
                )
            elif parsed.path == "/api/trips":
                self._json(200, self.service.trips(required(query, "role")))
            elif parsed.path == "/api/timeline":
                self._json(
                    200,
                    self.service.timeline(required(query, "role"), required(query, "trip")),
                )
            elif parsed.path == "/api/history":
                body, headers, status = self.service.history(
                    required(query, "role"),
                    required(query, "trip"),
                    required(query, "resource"),
                    int(required(query, "start_ns")),
                    int(required(query, "end_ns")),
                    int(optional(query, "every_n", "1")),
                    min(50000, int(optional(query, "max_records", "50000"))),
                )
                self._bytes(status, body, headers)
            elif parsed.path == "/api/closest":
                body, headers, status = self.service.closest(
                    required(query, "role"),
                    required(query, "trip"),
                    required(query, "resource"),
                    int(required(query, "t_ns")),
                )
                self._bytes(status, body, headers)
            elif parsed.path == "/api/denial-proof":
                status, result = self.service.denial_probe(
                    required(query, "role"),
                    required(query, "trip"),
                    required(query, "resource"),
                )
                self._json(status, result)
            else:
                self._json(404, {"error": "endpoint not found"})
        except UpstreamError as error:
            headers = {"X-Demo-Policy-Decision": "denied"}
            self._bytes(
                error.status,
                error.body,
                {"Content-Type": "application/json", **headers},
            )
        except (ValueError, KeyError) as error:
            self._json(400, {"error": str(error)})
        except Exception as error:
            print(f"gateway error: {error}", file=sys.stderr)
            self._json(502, {"error": "Pi gateway could not complete the request"})

    def _json(self, status: int, value: Any) -> None:
        body = json.dumps(value, separators=(",", ":")).encode()
        self._bytes(
            status,
            body,
            {
                "Content-Type": "application/json",
                "X-Demo-Network-Bytes": str(len(body)),
            },
        )

    def _bytes(self, status: int, body: bytes, headers: dict[str, str]) -> None:
        self.send_response(status)
        for name, value in headers.items():
            if value:
                self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        print(f"{self.address_string()} {format % args}", file=sys.stderr)


def required(query: dict[str, list[str]], name: str) -> str:
    if name not in query or not query[name] or not query[name][0]:
        raise ValueError(f"missing query parameter: {name}")
    return query[name][0]


def optional(query: dict[str, list[str]], name: str, default: str) -> str:
    return query.get(name, [default])[0]


def main() -> int:
    parser = argparse.ArgumentParser(description="pDAL OEM demo Pi gateway")
    parser.add_argument("--address", default=os.environ.get("DEMO_PI_ADDRESS", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("DEMO_PI_PORT", "8090")))
    parser.add_argument("--pdal-url", default=os.environ.get("PDAL_URL", "http://127.0.0.1:8080"))
    parser.add_argument("--ssd-root", type=Path, default=Path("/home/avs/DATA/SSD"))
    parser.add_argument("--hdd-root", type=Path, default=Path("/home/avs/DATA/HDD"))
    args = parser.parse_args()
    service = DemoService(PdalClient(args.pdal_url), TripCatalog(args.ssd_root, args.hdd_root))
    try:
        health = service.pdal.health()
    except Exception as error:
        print(f"pDAL connectivity check failed: {error}", file=sys.stderr)
        return 1
    server = ThreadingHTTPServer((args.address, args.port), GatewayHandler)
    server.service = service  # type: ignore[attr-defined]
    print(
        f"Pi gateway listening on http://{args.address}:{args.port} "
        f"(pDAL {health.get('api_version', 'unknown')}; decoding disabled)",
        file=sys.stderr,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
