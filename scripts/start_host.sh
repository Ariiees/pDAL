#!/usr/bin/env bash
set -euo pipefail

HOST_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# Default: existing reverse SSH tunnel. Optional Ethernet backup:
#   ./scripts/start_host.sh --pi-ip 192.168.50.36
# --pi-ip/--pi-port and the existing --pi-url are parsed by server.py.
export PI_URL=${PI_URL:-http://127.0.0.1:18090}

# Pass PI_GATEWAY_KEY as --pi-key when set
KEY_ARG=()
if [[ -n "${PI_GATEWAY_KEY:-}" ]]; then
  KEY_ARG=(--pi-key "${PI_GATEWAY_KEY}")
fi

exec python3 "${HOST_ROOT}/host/server.py" "${KEY_ARG[@]}" "$@"
