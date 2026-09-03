#!/usr/bin/env python3
"""Thin OEM demo gateway: AVS discovery, pDAL queries, and raw record transfer.

This process intentionally does not decode GPS, JPEG, or LAZ payloads. Sensor
record parsing and rendering belong to the host viewer.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import os
import secrets
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


class Unauthorized(Exception):
    """Raised when a request has no valid role session."""


def _b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def _b64url_decode(text: str) -> bytes:
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


class PasswordStore:
    """Per-role demo passwords, checked against PBKDF2-HMAC-SHA256 hashes."""

    def __init__(self, entries: dict[str, str]):
        self._entries = entries

    @classmethod
    def from_file(cls, path: Path) -> "PasswordStore":
        data = json.loads(path.read_text(encoding="utf-8"))
        roles = data.get("roles", {})
        if not isinstance(roles, dict) or not roles:
            raise ValueError(f"{path} has no non-empty 'roles' map")
        return cls({str(key): str(value) for key, value in roles.items()})

    def known_role(self, role: str) -> bool:
        return role in self._entries

    def verify(self, role: str, password: str) -> bool:
        stored = self._entries.get(role)
        if not stored:
            return False
        try:
            scheme, iterations, salt_b64, hash_b64 = stored.split("$", 3)
        except ValueError:
            return False
        if scheme != "pbkdf2_sha256":
            return False
        derived = hashlib.pbkdf2_hmac(
            "sha256", password.encode(), _b64url_decode(salt_b64), int(iterations)
        )
        return hmac.compare_digest(derived, _b64url_decode(hash_b64))


class LoginThrottle:
    """Locks a role for a cooldown window after repeated failed logins."""

    def __init__(self, max_attempts: int = 5, cooldown_seconds: int = 60):
        self.max_attempts = max_attempts
        self.cooldown_seconds = cooldown_seconds
        self._state: dict[str, tuple[int, float]] = {}
        self._lock = threading.Lock()

    def locked_for(self, role: str) -> float:
        with self._lock:
            count, until = self._state.get(role, (0, 0.0))
            remaining = until - time.time()
            if count >= self.max_attempts and remaining > 0:
                return remaining
            return 0.0

    def record_failure(self, role: str) -> None:
        with self._lock:
            count, _ = self._state.get(role, (0, 0.0))
            self._state[role] = (count + 1, time.time() + self.cooldown_seconds)

    def record_success(self, role: str) -> None:
        with self._lock:
            self._state.pop(role, None)


class SessionStore:
    """Issues and verifies signed, expiring session tokens bound to a role."""

    def __init__(self, secret: bytes):
        self._secret = secret
        self._lock = threading.Lock()
        self._revoked: set[str] = set()

    def issue(self, role: str, ttl_seconds: int) -> tuple[str, int]:
        issued = int(time.time())
        payload = {
            "role": role,
            "iat": issued,
            "exp": issued + ttl_seconds,
            "jti": _b64url(secrets.token_bytes(9)),
        }
        raw = _b64url(json.dumps(payload, separators=(",", ":")).encode())
        signature = hmac.new(self._secret, raw.encode(), hashlib.sha256).digest()
        return raw + "." + _b64url(signature), ttl_seconds

    def verify(self, token: str) -> dict[str, Any] | None:
        try:
            raw, signature_b64 = token.split(".", 1)
        except ValueError:
            return None
        expected = hmac.new(self._secret, raw.encode(), hashlib.sha256).digest()
        try:
            if not hmac.compare_digest(_b64url_decode(signature_b64), expected):
                return None
            payload = json.loads(_b64url_decode(raw))
        except Exception:
            return None
        if not isinstance(payload, dict) or "role" not in payload:
            return None
        if int(payload.get("exp", 0)) < int(time.time()):
            return None
        with self._lock:
            if payload.get("jti") in self._revoked:
                return None
        return payload

    def revoke(self, token: str) -> None:
        payload = self.verify(token)
        if payload and payload.get("jti"):
            with self._lock:
                self._revoked.add(payload["jti"])


class TokenMinter:
    """Signs short-lived HS256 bearer tokens for pDAL, one per demo role.

    The demo signs its own tokens with the same secret pDAL verifies. A real
    deployment would obtain these from an identity provider instead.
    """

    def __init__(
        self, secret: str, *, issuer: str, audience: str, ttl_seconds: int = 900
    ):
        if len(secret) < 16:
            raise ValueError("auth secret must be at least 16 bytes")
        self._secret = secret.encode()
        self._issuer = issuer
        self._audience = audience
        self._ttl = ttl_seconds
        self._cache: dict[str, tuple[str, float]] = {}

    def for_role(self, role: str) -> str:
        cached = self._cache.get(role)
        now = time.time()
        if cached and cached[1] - 30 > now:
            return cached[0]
        profile = role_profile(role)
        issued = int(now)
        header = {"alg": "HS256", "typ": "JWT"}
        payload = {
            "iss": self._issuer,
            "aud": self._audience,
            "sub": profile["principal"],
            "role": role,
            "org": "oem-demo",
            "iat": issued,
            "exp": issued + self._ttl,
        }
        signing_input = (
            _b64url(json.dumps(header, separators=(",", ":")).encode())
            + "."
            + _b64url(json.dumps(payload, separators=(",", ":")).encode())
        )
        signature = hmac.new(
            self._secret, signing_input.encode(), hashlib.sha256
        ).digest()
        token = signing_input + "." + _b64url(signature)
        self._cache[role] = (token, issued + self._ttl)
        return token


class PdalClient:
    def __init__(
        self,
        base_url: str,
        timeout: float = 30.0,
        minter: TokenMinter | None = None,
    ):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.minter = minter

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
        headers = {"Content-Type": "application/json"}
        if self.minter is not None:
            headers["Authorization"] = f"Bearer {self.minter.for_role(role)}"
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

    def access_report(self, role: str, trip_id: str | None = None) -> dict[str, Any]:
        """Per-resource allow/deny for `role`, each cell decided by a real pDAL
        call. Feeds the viewer's access-and-policy panel."""
        profile = role_profile(role)
        trips = self.catalog.scan()
        if not trips:
            return {"role": role, "resources": {}, "recordings": 0}
        trip = self.catalog.by_id(trip_id) if trip_id else trips[0]
        matrix = self.access_matrix(role, trip)
        resources = {
            resource: {
                "label": RESOURCE_INFO[resource]["label"],
                **cell,
            }
            for resource, cell in matrix.items()
        }
        return {
            "role": role,
            "label": profile["label"],
            "purpose": profile["purpose"],
            "principal": profile["principal"],
            "trip_id": trip["id"],
            "resources": resources,
        }

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

    def _bearer(self) -> str:
        value = self.headers.get("Authorization", "")
        return value[7:].strip() if value.lower().startswith("bearer ") else ""

    def _session(self) -> dict[str, Any] | None:
        sessions = getattr(self.server, "sessions", None)  # type: ignore[attr-defined]
        token = self._bearer()
        return sessions.verify(token) if sessions is not None and token else None

    # The role for a data request comes from the verified session token, never
    # from a query parameter, unless --allow-legacy-role is set.
    def _client_role(self, query: dict[str, list[str]]) -> str:
        payload = self._session()
        if payload:
            return str(payload["role"])
        if getattr(self.server, "allow_legacy_role", False) and query.get("role"):
            return query["role"][0]
        raise Unauthorized("sign in to a role at POST /api/login")

    def do_POST(self) -> None:
        parsed = urllib.parse.urlsplit(self.path)
        try:
            if parsed.path == "/api/login":
                self._handle_login()
            elif parsed.path == "/api/logout":
                sessions = getattr(self.server, "sessions", None)  # type: ignore[attr-defined]
                token = self._bearer()
                if sessions is not None and token:
                    sessions.revoke(token)
                self._json(200, {"ok": True})
            else:
                self._json(404, {"error": "endpoint not found"})
        except Unauthorized as error:
            self._json(401, {"error": str(error)})
        except (ValueError, KeyError) as error:
            self._json(400, {"error": str(error)})
        except Exception as error:  # noqa: BLE001
            print(f"gateway error: {error}", file=sys.stderr)
            self._json(502, {"error": "Pi gateway could not complete the request"})

    def _handle_login(self) -> None:
        sessions = getattr(self.server, "sessions", None)  # type: ignore[attr-defined]
        passwords = getattr(self.server, "passwords", None)  # type: ignore[attr-defined]
        throttle = getattr(self.server, "throttle", None)  # type: ignore[attr-defined]
        if sessions is None or passwords is None or throttle is None:
            self._json(503, {"error": "role login is not configured on this gateway"})
            return
        length = int(self.headers.get("Content-Length", "0") or "0")
        try:
            payload = json.loads(self.rfile.read(length) or b"{}")
        except Exception as error:  # noqa: BLE001
            raise ValueError("request body must be JSON") from error
        role = str(payload.get("role", ""))
        password = str(payload.get("password", ""))
        if not role or not password:
            raise ValueError("role and password are required")

        locked = throttle.locked_for(role)
        if locked > 0:
            self._json(
                429,
                {"error": "too many failed attempts", "retry_after_seconds": int(locked) + 1},
            )
            return
        # One response for "no such role" and "wrong password": do not disclose
        # which roles exist.
        if (
            not passwords.known_role(role)
            or role not in ROLE_INFO
            or not passwords.verify(role, password)
        ):
            throttle.record_failure(role)
            self._json(401, {"error": "invalid role or password"})
            return
        throttle.record_success(role)

        default_ttl = getattr(self.server, "session_ttl", 900)  # type: ignore[attr-defined]
        max_ttl = getattr(self.server, "session_max_ttl", 3600)  # type: ignore[attr-defined]
        requested = payload.get("ttl_seconds")
        ttl = default_ttl
        if isinstance(requested, (int, float)) and requested > 0:
            ttl = int(requested)
        ttl = max(30, min(ttl, max_ttl))
        token, expires_in = sessions.issue(role, ttl)
        profile = ROLE_INFO[role]
        self._json(
            200,
            {
                "token": token,
                "role": role,
                "label": profile["label"],
                "purpose": profile["purpose"],
                "expires_in": expires_in,
            },
        )

    def do_GET(self) -> None:
        parsed = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        try:
            if parsed.path == "/api/session":
                payload = self._session()
                if not payload:
                    self._json(401, {"authenticated": False})
                    return
                profile = ROLE_INFO.get(payload["role"], {})
                self._json(
                    200,
                    {
                        "authenticated": True,
                        "role": payload["role"],
                        "label": profile.get("label", payload["role"]),
                        "purpose": profile.get("purpose", ""),
                        "expires_in": max(0, int(payload["exp"]) - int(time.time())),
                    },
                )
                return
            if parsed.path == "/api/access":
                role = self._client_role(query)
                self._json(200, self.service.access_report(role, optional(query, "trip", "") or None))
                return
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
                self._json(200, self.service.trips(self._client_role(query)))
            elif parsed.path == "/api/timeline":
                self._json(
                    200,
                    self.service.timeline(self._client_role(query), required(query, "trip")),
                )
            elif parsed.path == "/api/history":
                body, headers, status = self.service.history(
                    self._client_role(query),
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
                    self._client_role(query),
                    required(query, "trip"),
                    required(query, "resource"),
                    int(required(query, "t_ns")),
                )
                self._bytes(status, body, headers)
            elif parsed.path == "/api/denial-proof":
                status, result = self.service.denial_probe(
                    self._client_role(query),
                    required(query, "trip"),
                    required(query, "resource"),
                )
                self._json(status, result)
            else:
                self._json(404, {"error": "endpoint not found"})
        except Unauthorized as error:
            self._json(401, {"error": str(error)})
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
    parser.add_argument(
        "--auth-secret",
        default=os.environ.get("DEMO_PDAL_AUTH_SECRET", ""),
        help="HS256 secret pDAL verifies; enables bearer tokens to pDAL",
    )
    parser.add_argument(
        "--auth-secret-file",
        default=os.environ.get("DEMO_PDAL_AUTH_SECRET_FILE", ""),
        help="file holding the HS256 secret (overrides --auth-secret if set)",
    )
    parser.add_argument("--auth-issuer", default="pdal-local-issuer")
    parser.add_argument("--auth-audience", default="pdal")
    parser.add_argument(
        "--roles-auth-file",
        default=os.environ.get(
            "DEMO_ROLES_AUTH_FILE",
            str(Path(__file__).resolve().parent / "config" / "roles.auth.json"),
        ),
        help="JSON file of per-role PBKDF2 password hashes; enables POST /api/login",
    )
    parser.add_argument(
        "--session-secret-file",
        default=os.environ.get("DEMO_SESSION_SECRET_FILE", ""),
        help="file holding the session-signing secret (default: random per start)",
    )
    parser.add_argument("--session-ttl", type=int, default=900)
    parser.add_argument("--session-max-ttl", type=int, default=3600)
    parser.add_argument(
        "--short-ttl",
        action="store_true",
        help="demo mode: issue 60 s sessions so expiry is easy to show",
    )
    parser.add_argument(
        "--allow-legacy-role",
        action="store_true",
        help="also accept ?role= without a session (transition aid; insecure)",
    )
    args = parser.parse_args()

    secret = ""
    if args.auth_secret_file:
        secret = Path(args.auth_secret_file).read_text(encoding="utf-8").strip()
    elif args.auth_secret:
        secret = args.auth_secret
    minter = (
        TokenMinter(secret, issuer=args.auth_issuer, audience=args.auth_audience)
        if secret
        else None
    )
    if minter is None:
        print(
            "WARNING: no auth secret provided; requests to pDAL will be "
            "unauthenticated and will fail if pDAL enforces auth.",
            file=sys.stderr,
        )

    passwords: PasswordStore | None = None
    throttle: LoginThrottle | None = None
    sessions: SessionStore | None = None
    session_ttl = 60 if args.short_ttl else args.session_ttl
    session_max_ttl = 120 if args.short_ttl else args.session_max_ttl
    roles_auth_path = Path(args.roles_auth_file)
    if roles_auth_path.is_file():
        try:
            passwords = PasswordStore.from_file(roles_auth_path)
            config = json.loads(roles_auth_path.read_text(encoding="utf-8"))
        except Exception as error:  # noqa: BLE001
            print(f"cannot load {roles_auth_path}: {error}", file=sys.stderr)
            return 1
        lockout = config.get("lockout", {}) if isinstance(config, dict) else {}
        throttle = LoginThrottle(
            int(lockout.get("max_attempts", 5)),
            int(lockout.get("cooldown_seconds", 60)),
        )
        if not args.short_ttl:
            session_ttl = int(config.get("session_ttl_seconds", session_ttl))
            session_max_ttl = int(config.get("session_max_ttl_seconds", session_max_ttl))
        secret_bytes = (
            Path(args.session_secret_file).read_bytes().strip()
            if args.session_secret_file
            else secrets.token_bytes(32)
        )
        sessions = SessionStore(secret_bytes)
    elif not args.allow_legacy_role:
        print(
            f"role login file not found: {roles_auth_path}\n"
            "Create it (start_pi.sh does this from roles.auth.example.json) or "
            "pass --allow-legacy-role for an insecure transition mode.",
            file=sys.stderr,
        )
        return 1
    else:
        print(
            "WARNING: no role login file; running with --allow-legacy-role. "
            "Any caller can pick any role via ?role=.",
            file=sys.stderr,
        )

    service = DemoService(
        PdalClient(args.pdal_url, minter=minter),
        TripCatalog(args.ssd_root, args.hdd_root),
    )
    try:
        health = service.pdal.health()
    except Exception as error:
        print(f"pDAL connectivity check failed: {error}", file=sys.stderr)
        return 1
    server = ThreadingHTTPServer((args.address, args.port), GatewayHandler)
    server.service = service  # type: ignore[attr-defined]
    server.passwords = passwords  # type: ignore[attr-defined]
    server.throttle = throttle  # type: ignore[attr-defined]
    server.sessions = sessions  # type: ignore[attr-defined]
    server.session_ttl = session_ttl  # type: ignore[attr-defined]
    server.session_max_ttl = session_max_ttl  # type: ignore[attr-defined]
    server.allow_legacy_role = args.allow_legacy_role  # type: ignore[attr-defined]
    print(
        f"Pi gateway listening on http://{args.address}:{args.port} "
        f"(pDAL {health.get('api_version', 'unknown')}; decoding disabled; "
        f"tokens {'on' if minter else 'off'}; "
        f"role login {'on' if sessions else 'off'}; "
        f"session ttl {session_ttl}s"
        f"{'; legacy ?role= allowed' if args.allow_legacy_role else ''})",
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
