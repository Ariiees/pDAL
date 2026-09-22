#!/usr/bin/env python3
"""Supervise the Pi gateway and a reconnecting, loopback-only SSH forward."""

import argparse
import ctypes
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


def child_setup():
    """Close SSH / stop the service shell even if the launcher is killed."""
    parent = os.getppid()
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(1, signal.SIGTERM, 0, 0, 0) != 0:  # Linux PR_SET_PDEATHSIG
        raise OSError(ctypes.get_errno(), "prctl(PR_SET_PDEATHSIG)")
    if os.getppid() != parent or parent == 1:
        os.kill(os.getpid(), signal.SIGTERM)


def stop_service(process, timeout=10):
    if process is None:
        return
    # start_pi and its children have their own process group.
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=timeout)
    except ProcessLookupError:
        pass
    except subprocess.TimeoutExpired:
        pass
    finally:
        # The group leader can exit before a stuck child. Always reap the group.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
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
           "-o", "ForkAfterAuthentication=no", "-o", "ControlPersist=no",
           "-o", "ServerAliveInterval=10", "-o", "ServerAliveCountMax=3"]
    service = None
    tunnel = None

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
                nonlocal tunnel
                stop_service(tunnel, timeout=3)
                tunnel = None

            def remote_health():
                try:
                    check = subprocess.run([
                        *control, "-o", "ControlMaster=no", target,
                        "curl", "--silent", "--show-error", "--fail", "--max-time", "5",
                        f"http://127.0.0.1:{args.remote_port}/api/health",
                    ], capture_output=True, text=True, timeout=10)
                    return check.returncode == 0 and json.loads(check.stdout).get("status") == "ready"
                except (ValueError, subprocess.TimeoutExpired):
                    return False

            try:
                env = {**os.environ, "DEMO_PI_ADDRESS": "127.0.0.1", "DEMO_PI_PORT": "8090"}
                log("Starting pDAL and the Pi gateway (ports 8080 and 8090).")
                service = subprocess.Popen([str(ROOT / "demo/scripts/start_pi.sh")],
                                           cwd=ROOT, env=env, start_new_session=True,
                                           preexec_fn=child_setup)
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
                next_probe = 0
                while service.poll() is None:
                    if tunnel is None or tunnel.poll() is not None:
                        if connected:
                            log("Host connection lost; reconnecting.")
                        connected = False
                        tunnel_stop()
                        log(f"Connecting to {target} (SSH key authentication).")
                        # Keep SSH in the foreground and own its PID/group. A
                        # temporary multiplex socket is only used for probes.
                        error_path = ROOT / "demo/pi/config/ssh-last-error.log"
                        with open(error_path, "w") as errors:
                            tunnel = subprocess.Popen([
                                *control, "-M", "-N", "-T",
                                "-o", "ExitOnForwardFailure=yes",
                                "-R", f"127.0.0.1:{args.remote_port}:127.0.0.1:8090", target,
                            ], stdin=subprocess.DEVNULL, stderr=errors,
                                start_new_session=True, preexec_fn=child_setup)
                        deadline = time.monotonic() + 20
                        while tunnel.poll() is None and time.monotonic() < deadline:
                            if tunnel_check():
                                break
                            time.sleep(.2)
                        else:
                            tunnel_stop()
                            detail = error_path.read_text(errors="replace").strip()[-2000:]
                            log(f"SSH failed: {detail or 'connection timed out'}")
                            if "remote port forwarding failed" in detail:
                                log(f"Host port {args.remote_port} is occupied or forwarding is denied. "
                                    "Stop the previous Pi tunnel; the host viewer does not own this port.")
                            log("Retrying in 5 seconds.")
                            time.sleep(5)
                            continue
                        # Verify an actual request from the host through the new tunnel.
                        if not remote_health():
                            tunnel_stop()
                            log("Host could not read gateway health through the tunnel; retrying in 5 seconds.")
                            time.sleep(5)
                            continue
                        connected = True
                        next_probe = time.monotonic() + 15
                        log(f"READY: host can retrieve Pi data at http://127.0.0.1:{args.remote_port}")
                        log(f"On the host: cd /home/yuxw/demo && ./scripts/start_host.sh --pi-url http://127.0.0.1:{args.remote_port}")
                        log("Open http://127.0.0.1:8088 in the host browser. Keep this terminal open; Ctrl+C stops everything.")
                    elif time.monotonic() >= next_probe:
                        next_probe = time.monotonic() + 15
                        if not remote_health():
                            log("Gateway health through the host failed; checking the local gateway.")
                            try:
                                local_ready = health()
                            except (OSError, ValueError):
                                local_ready = False
                            if local_ready:
                                log("Local gateway is healthy; rebuilding the SSH tunnel.")
                                tunnel_stop()
                            else:
                                log("Local gateway/pDAL is unhealthy; keeping SSH connected. See service output.")
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
