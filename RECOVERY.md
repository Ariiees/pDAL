# Browser and reverse-tunnel recovery

The viewer uses a reverse SSH tunnel from the Pi. An unreachable SSH connection
can leave the host listening on port 18090 while HTTP requests hang. A new Pi
tunnel then fails with `remote port forwarding failed for listen port 18090`.
Restarting the host HTTP viewer cannot release a port owned by SSH.

## Host setup

Run once on the SSH host, substituting the SSH login user if necessary:

```sh
sudo python3 scripts/install_ssh_liveness.py --user yuxw
```

This installs a user-scoped SSH configuration with `ClientAliveInterval 10` and
`ClientAliveCountMax 3`, validates the configuration, and reloads SSH. It applies
to new connections. Responsive idle terminals remain connected. An already
stale connection predating the change must be identified and stopped separately;
do not kill unrelated SSH sessions.

Start the viewer with:

```sh
./scripts/start_host.sh --pi-url http://127.0.0.1:18090
```

The viewer starts even if the Pi is unavailable. The login page retries every
five seconds and offers Retry connection. Refresh restores a valid session;
expired sessions require sign-in. Optional map/LiDAR dependencies cannot block
login, though visualization still needs those libraries and graphics support.
The proxy uses short health/login timeouts, and browser requests have deadlines.

Update the Pi launcher from the repository's `pDAL` branch too. It owns the SSH
process directly, performs bounded cleanup, and probes the forwarded HTTP path.
After a lost network connection, allow roughly 30–45 seconds for a stale host
connection to expire, plus time to reconnect once networking is restored.

Diagnostics on the host:

```sh
curl --max-time 6 http://127.0.0.1:8088/health
curl --max-time 6 http://127.0.0.1:18090/api/health
```

The first checks the viewer; the second checks the SSH tunnel and Pi service.
Ctrl+C on the host stops only the viewer. Ctrl+C on the Pi stops its services
and tunnel. Both can then be started using the same commands.

## Verification

```sh
python3 tests/host_recovery_test.py
node tests/browser_recovery_test.cjs
node tests/viewer_selection_test.cjs
node tests/storage_latency_browser.cjs
node tests/brake_timeline_browser.cjs
```

Browser tests require Playwright and its Chromium browser. Fixtures cover
offline startup, reconnection, unavailable CDNs, refresh/session restoration,
expired sessions, hung login, and immediate local logout.

Live validation on 2026-09-22 also covered three refreshes, role enforcement,
SSD/HDD GPS/camera/LiDAR retrieval, clean shutdown/restart, and forced launcher
death. Pausing the demo SSH client released the stale host port in 32.91 seconds;
after resume, automatic reconnection restored HTTP health in 6.52 seconds.

Reference: [OpenSSH client liveness checks](https://man.openbsd.org/sshd_config#ClientAliveInterval).
