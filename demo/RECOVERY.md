# Reliable Pi launcher and reverse SSH forwarding

Start the demo with `./pDAL.sh`. The launcher owns the pDAL service group and a
foreground SSH process. Ctrl+C or SIGTERM terminates and reaps these processes.
On Linux, parent-death signaling also stops the service shell and SSH client if
the launcher is killed unexpectedly.

The SSH process reconnects after failure. The launcher periodically checks the
actual host-to-Pi HTTP path, rebuilding a failed tunnel if the local gateway is
healthy. A local service-health failure is reported separately. The actual SSH
failure is printed, with the latest attempt logged in the ignored file
`demo/pi/config/ssh-last-error.log`.

## Required host configuration

Client-side SSH keepalives alone cannot release an unreachable host's reverse
listener. Configure host-side client liveness too. The repository's `host`
branch provides a validated installation helper:

```sh
sudo python3 scripts/install_ssh_liveness.py --user yuxw
```

It installs `ClientAliveInterval 10` and `ClientAliveCountMax 3` for the specified
SSH user and reloads SSH. New connections then release their forwarded port
after missed client replies. Existing stale sessions need separate identification
and cleanup; do not terminate unrelated SSH sessions.

If SSH reports `remote port forwarding failed for listen port 18090`, the port
may still belong to an older tunnel, or server forwarding may be denied. The
host viewer does not own port 18090. Restarting it cannot fix a stale SSH
listener. A simulated unresponsive connection was released in 32.91 seconds
during live validation; reconnection after resume took 6.52 seconds.

Update the viewer from the `host` branch as well. It starts while the Pi is
offline, retries startup connections, and restores valid browser sessions after
refresh. Launch it with the URL printed by `pDAL.sh`:

```sh
./scripts/start_host.sh --pi-url http://127.0.0.1:18090
```

Run the Linux process-lifecycle regression tests with:

```sh
python3 tests/test_launcher_cleanup.py
```

These verify cleanup when the process-group leader has already exited and when
the supervisor is forcibly killed. Live validation also checked Ctrl+C,
immediate restart, role enforcement, browser refresh, and SSD/HDD retrieval.

Reference: [OpenSSH client liveness checks](https://man.openbsd.org/sshd_config#ClientAliveInterval).
