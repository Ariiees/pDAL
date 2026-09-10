#!/usr/bin/env bash
# Start the AVS data gateway and its outbound connection to the demo host.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
exec python3 "${ROOT}/demo/scripts/launch_pi.py" "$@"
