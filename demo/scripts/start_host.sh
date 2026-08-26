#!/usr/bin/env bash
set -euo pipefail

DEMO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
exec python3 "${DEMO_ROOT}/demo/host/server.py" "$@"
