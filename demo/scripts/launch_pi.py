#!/usr/bin/env python3
"""Supervise the Pi gateway and a reconnecting, loopback-only SSH forward."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]


def log(message):
    print(f"[pDAL] {message}", flush=True)


def health():
    with urllib.request.urlopen("http://127.0.0.1:8090/api/health", timeout=3) as response:
        data = json.load(response)
        return data.get("status") == "ready" and data.get("decode_location") == "host"


def stop_service(process):
    if process is None:
        return
    # start_pi and its children have their own process group.
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=10)
    except ProcessLookupError:
        pass
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("PDAL_SSH_HOST", "128.175.213.233"))
    parser.add_argument("--user", default=os.environ.get("PDAL_SSH_USER", "yuxw"))
    parser.add_argument("--key", type=Path, default=Path(os.environ.get(
        "PDAL_SSH_KEY", str(Path.home() / ".ssh/pdal_demo_ed25519"))))
    parser.add_argument("--remote-port", type=int,
                        default=int(os.environ.get("PDAL_REMOTE_PORT", "18090")))
    args = parser.parse_args()
    if not 1024 <= args.remote_port <= 65535:
        parser.error("--remote-port must be between 1024 and 65535")
    if not args.key.is_file():
        parser.error(f"SSH key not found: {args.key}; set --key to an authorized key")
    if args.host.startswith("-") or args.user.startswith("-"):
        parser.error("invalid SSH host or username")

    target = f"{args.user}@{args.host}"
    ssh = ["ssh", "-i", str(args.key), "-o", "IdentitiesOnly=yes",
           "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
           "-o", "ConnectTimeout=5", "-o", "ConnectionAttempts=1",
           "-o", "ServerAliveInterval=10", "-o", "ServerAliveCountMax=3"]
    service = None

    def interrupt(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupt)
    signal.signal(signal.SIGINT, interrupt)
    # The lock lives in the repo, so simultaneous launches cannot own the same ports.
    with open(ROOT / "demo/pi/config/launcher.lock", "w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            log("Already running. Stop the existing pDAL.sh before starting another.")
            return 1
        for port in (8080, 8090):
            with socket.socket() as probe:
                if probe.connect_ex(("127.0.0.1", port)) == 0:
                    log(f"Port {port} is already in use. Stop that service first.")
                    return 1

        with tempfile.TemporaryDirectory(prefix="pdal-ssh-") as runtime:
            control = [*ssh, "-S", str(Path(runtime) / "control")]

            def tunnel_check():
                try:
                    return subprocess.run([*control, "-O", "check", target],
                                          stdout=subprocess.DEVNULL,
                                          stderr=subprocess.DEVNULL, timeout=8).returncode == 0
                except subprocess.TimeoutExpired:
                    return False

            def tunnel_stop():
                subprocess.run([*control, "-O", "exit", target],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               timeout=8)

            try:
                env = {**os.environ, "DEMO_PI_ADDRESS": "127.0.0.1", "DEMO_PI_PORT": "8090"}
                log("Starting pDAL and the Pi gateway (ports 8080 and 8090).")
                service = subprocess.Popen([str(ROOT / "demo/scripts/start_pi.sh")],
                                           cwd=ROOT, env=env, start_new_session=True)
                deadline = time.monotonic() + 180
                while True:
                    if service.poll() is not None:
                        raise RuntimeError("Pi service exited during startup; see its output above")
                    try:
                        if health():
                            break
                    except (OSError, ValueError):
                        pass
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Pi gateway did not become healthy within 180 seconds")
                    time.sleep(1)

                connected = False
                while service.poll() is None:
                    if not tunnel_check():
                        if connected:
                            log("Host connection lost; reconnecting.")
                        connected = False
                        log(f"Connecting to {target} (SSH key authentication).")
                        try:
                            result = subprocess.run([
                                *control, "-M", "-f", "-N", "-T",
                                "-o", "ExitOnForwardFailure=yes",
                                "-R", f"127.0.0.1:{args.remote_port}:127.0.0.1:8090", target,
                            ], timeout=20)
                        except subprocess.TimeoutExpired:
                            log("SSH connection timed out; retrying in 5 seconds.")
                            time.sleep(5)
                            continue
                        if result.returncode != 0:
                            log("SSH failed; retrying in 5 seconds. Check the key, network, and remote port.")
                            time.sleep(5)
                            continue
                        # Verify an actual request from the host through the new tunnel.
                        try:
                            check = subprocess.run([
                                *control, target, "curl", "--silent", "--show-error", "--fail",
                                "--max-time", "5", f"http://127.0.0.1:{args.remote_port}/api/health",
                            ], capture_output=True, text=True, timeout=10)
                            ready = check.returncode == 0 and json.loads(check.stdout).get("status") == "ready"
                        except (ValueError, subprocess.TimeoutExpired):
                            ready = False
                        if not ready:
                            tunnel_stop()
                            log("Host could not read gateway health through the tunnel; retrying in 5 seconds.")
                            time.sleep(5)
                            continue
                        connected = True
                        log(f"READY: host can retrieve Pi data at http://127.0.0.1:{args.remote_port}")
                        log("On the host: cd /home/yuxw/demo && ./scripts/start_host.sh")
                        log("Open http://127.0.0.1:8088 in the host browser. Keep this terminal open; Ctrl+C stops everything.")
                    time.sleep(5)
                raise RuntimeError("Pi service stopped unexpectedly; see its output above")
            except KeyboardInterrupt:
                log("Stopping the tunnel and Pi services.")
                return 0
            except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
                log(str(error))
                return 1
            finally:
                # Avoid a second Ctrl+C interrupting cleanup halfway through.
                signal.signal(signal.SIGINT, signal.SIG_IGN)
                signal.signal(signal.SIGTERM, signal.SIG_IGN)
                try:
                    tunnel_stop()
                except (OSError, subprocess.TimeoutExpired):
                    pass
                stop_service(service)


if __name__ == "__main__":
    sys.exit(main())
