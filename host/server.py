#!/usr/bin/env python3
"""Static OEM viewer and transparent host-to-Pi API proxy."""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
import signal
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class ViewerHandler(SimpleHTTPRequestHandler):
    server_version = "pdal-oem-viewer/1"

    def setup(self) -> None:
        super().setup()
        self.connection.settimeout(65)

    def handle(self) -> None:
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            # This also covers disconnects while sending an upstream error.
            pass

    def do_POST(self) -> None:
        if self.path.split("?", 1)[0] in {"/api/login", "/api/logout"}:
            self._proxy()
            return
        self.send_error(404)

    def do_GET(self) -> None:
        if self.path == "/health":
            self._send(200, b'{"status":"ready","decode_location":"host"}', "application/json")
            return
        if self.path.startswith("/client-status"):
            self._client_status()
            return
        if self.path.startswith("/api/"):
            self._proxy()
            return
        super().do_GET()

    def _client_status(self) -> None:
        parsed = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        allowed = {
            "state", "role", "route_points", "camera_width", "camera_height",
            "lidar_points", "lidar_radius", "lidar_span_x", "lidar_span_y", "lidar_span_z"
        }
        updates = {name: values[0] for name, values in query.items() if name in allowed and values}
        if updates:
            updates["updated_at"] = time.time()
            self.server.client_status.update(updates)  # type: ignore[attr-defined]
        body = json.dumps(self.server.client_status).encode()  # type: ignore[attr-defined]
        self._send(200, body, "application/json")

    def _proxy(self) -> None:
        target = self.server.pi_url.rstrip("/") + self.path  # type: ignore[attr-defined]

        # Read request body for POST/PUT/PATCH
        req_body = None
        if self.command in ("POST", "PUT", "PATCH"):
            try:
                length = int(self.headers.get("Content-Length", 0))
            except ValueError:
                self.send_error(400, "Invalid Content-Length")
                return
            if not 0 <= length <= 16384:
                self.send_error(413)
                return
            req_body = self.rfile.read(length) if length else b""

        request = urllib.request.Request(target, data=req_body, method=self.command)

        # Forward relevant headers from the browser
        for header in ("Authorization", "X-Demo-Key", "Content-Type"):
            value = self.headers.get(header)
            if value:
                request.add_header(header, value)

        # Inject pi_key when configured and browser didn't already send one
        pi_key: str = self.server.pi_key  # type: ignore[attr-defined]
        if pi_key and not self.headers.get("X-Demo-Key"):
            request.add_header("X-Demo-Key", pi_key)

        try:
            path = urllib.parse.urlsplit(self.path).path
            timeout = (self.server.control_timeout if path in {
                "/api/health", "/api/roles", "/api/session", "/api/login", "/api/logout"
            } else self.server.request_timeout)
            with urllib.request.urlopen(request, timeout=timeout) as response:
                body = response.read()
                self.send_response(response.status)
                self._copy_headers(response.headers, len(body))
                self.end_headers()
                self.wfile.write(body)
        except urllib.error.HTTPError as error:
            body = error.read()
            self.send_response(error.code)
            self._copy_headers(error.headers, len(body))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            # Refresh/navigation closes browser sockets; it is not a Pi failure.
            return
        except Exception as error:
            body = json.dumps({"error": f"Pi connectivity failed: {error}. Check the Pi tunnel and retry."}).encode()
            self._send(502, body, "application/json")

    def _copy_headers(self, headers, length: int) -> None:
        allowed = {
            "content-type",
            "x-pdal-request-id",
            "x-demo-network-bytes",
            "x-demo-onboard-candidate-bytes",
            "x-demo-modality-candidate-bytes",
            "x-demo-query-kind",
            "x-demo-requested-t",
            "x-demo-record-timestamp",
            "x-demo-delta-ns",
            "x-demo-policy-decision",
        }
        for name, value in headers.items():
            if name.lower() in allowed:
                self.send_header(name, value)
        self.send_header("Content-Length", str(length))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")

    def _send(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def end_headers(self) -> None:
        # Reloads must pick up repaired HTML and JavaScript too.
        if not self.path.startswith("/api/"):
            self.send_header("Cache-Control", "no-store")
        self.send_header("Referrer-Policy", "no-referrer")
        super().end_headers()


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description="pDAL OEM host viewer")
    parser.add_argument("--address", default=os.environ.get("DEMO_HOST_ADDRESS", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("DEMO_HOST_PORT", "8088")))
    connection = parser.add_mutually_exclusive_group()
    connection.add_argument("--pi-url", help="Full Pi gateway URL (default: PI_URL or http://127.0.0.1:18090)")
    connection.add_argument("--pi-ip", type=ipaddress.ip_address,
                            help="Optional Pi Ethernet IP; connects directly to port 8090")
    parser.add_argument("--pi-port", type=int, help="Override the direct Pi port (requires --pi-ip)")
    parser.add_argument("--pi-key", default=os.environ.get("PI_GATEWAY_KEY", ""),
                        help="Gateway key (PI_GATEWAY_KEY). Added as X-Demo-Key on every proxied /api/* request.")
    parser.add_argument("--control-timeout", type=float, default=5,
                        help="Pi health/login socket timeout in seconds")
    parser.add_argument("--request-timeout", type=float, default=60,
                        help="Pi data socket timeout in seconds")
    args = parser.parse_args(argv)
    if args.pi_port is not None:
        if args.pi_ip is None:
            parser.error("--pi-port requires --pi-ip")
        if not 1 <= args.pi_port <= 65535:
            parser.error("--pi-port must be between 1 and 65535")
    if args.pi_ip is not None:
        host = f"[{args.pi_ip}]" if args.pi_ip.version == 6 else str(args.pi_ip)
        args.pi_url = f"http://{host}:{args.pi_port or 8090}"
    elif args.pi_url is None:
        args.pi_url = os.environ.get("PI_URL") or "http://127.0.0.1:18090"
    if args.control_timeout <= 0 or args.request_timeout <= 0:
        parser.error("timeouts must be positive")
    return args


def main() -> int:
    args = parse_args()
    root = Path(__file__).resolve().parent / "viewer"
    os.chdir(root)
    server = ThreadingHTTPServer((args.address, args.port), ViewerHandler)
    server.pi_url = args.pi_url  # type: ignore[attr-defined]
    server.pi_key = args.pi_key  # type: ignore[attr-defined]
    server.control_timeout = args.control_timeout
    server.request_timeout = args.request_timeout
    server.client_status = {}  # type: ignore[attr-defined]
    key_note = " | gateway key: set" if args.pi_key else ""
    print(
        f"OEM viewer: http://127.0.0.1:{args.port} | "
        f"Pi: {args.pi_url} (browser will check/retry connection) | decode: host{key_note}",
        file=sys.stderr,
    )
    print(
        "Demo passwords: fleet_analyst=fleet-demo  "
        "service_technician=service-demo  incident_investigator=incident-demo",
        file=sys.stderr,
    )
    def stop(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
