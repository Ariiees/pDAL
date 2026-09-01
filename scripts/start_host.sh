#!/usr/bin/env bash
set -euo pipefail

HOST_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

echo "Demo passwords: fleet_analyst=fleet-demo  service_technician=service-demo  incident_investigator=incident-demo" >&2

# Pass PI_GATEWAY_KEY as --pi-key when set
KEY_ARG=()
if [[ -n "${PI_GATEWAY_KEY:-}" ]]; then
  KEY_ARG=(--pi-key "${PI_GATEWAY_KEY}")
fi

exec python3 "${HOST_ROOT}/host/server.py" "${KEY_ARG[@]}" "$@"
