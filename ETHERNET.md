# Ethernet backup connection

With no connection option, the host keeps the existing reverse-tunnel address:

```sh
./scripts/start_host.sh
# Pi URL: http://127.0.0.1:18090
```

The existing `PI_URL` environment override is still honored. An explicit
`--pi-ip` or `--pi-url` takes precedence over that environment variable.

For the Ethernet backup:

1. Connect the Pi and host Ethernet adapters. Give both adapters addresses in
   the same subnet. For example, host `192.168.50.1/24` and Pi
   `192.168.50.36/24`. Use an unused subnet if that example conflicts with
   another network. A cable alone does not assign addresses unless a DHCP
   server is present; manual addresses work without DHCP or a gateway.
2. On the Pi, stop the normal `pDAL.sh` launcher if running, then run:

   ```sh
   cd /home/avs/pDAL
   ./pDAL.sh --direct
   ```

   Wait for READY. This serves the authenticated gateway on port 8090 on the
   Pi's IPv4 interfaces, without SSH or 5G. The internal pDAL service remains
   on loopback. Find the Pi Ethernet address with `ip -4 addr show eth0`.
3. On the host, stop the viewer if running, then run:

   ```sh
   cd /home/yuxw/demo
   ./scripts/start_host.sh --pi-ip 192.168.50.36
   ```

   Replace the example address with the Pi's actual Ethernet address.
4. Open `http://127.0.0.1:8088` in the host browser and sign in normally.

To check the connection from the host before opening the browser:

```sh
curl --max-time 6 http://192.168.50.36:8090/api/health
```

The firewall must permit host-to-Pi TCP port 8090 on the Ethernet link. Direct
mode removes the Pi's dependency on SSH/5G; the browser's external map and
rendering libraries still use the host's internet connection.

Additional options:

```sh
./scripts/start_host.sh --pi-ip 192.168.50.36 --pi-port 8090
./scripts/start_host.sh --pi-url http://192.168.50.36:8090
./scripts/start_host.sh --help
```

`--pi-port` requires `--pi-ip`. `--pi-ip` and `--pi-url` cannot be combined.
`--port` still selects the browser viewer's port, independently of the Pi port.

To return to 5G, stop both launchers, start `./pDAL.sh` on the Pi without
`--direct`, then start `./scripts/start_host.sh` on the host without `--pi-ip`.
The no-argument defaults are unchanged.
