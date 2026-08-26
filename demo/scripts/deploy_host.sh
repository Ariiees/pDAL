#!/usr/bin/env bash
set -euo pipefail

DEMO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HOST_TARGET=${HOST_TARGET:-yuxw@128.175.213.233}
REMOTE_ROOT=${REMOTE_ROOT:-/home/yuxw/demo}

echo "Checking SSH access to ${HOST_TARGET}..."
ssh -o ConnectTimeout=5 "${HOST_TARGET}" "mkdir -p '${REMOTE_ROOT}'"
rsync -az \
  "${DEMO_ROOT}/demo/host" \
  "${DEMO_ROOT}/demo/scripts" \
  "${DEMO_ROOT}/demo/tests" \
  "${DEMO_ROOT}/demo/README.md" \
  "${DEMO_ROOT}/demo/TODO.md" \
  "${HOST_TARGET}:${REMOTE_ROOT}/"

echo "Host files deployed to ${HOST_TARGET}:${REMOTE_ROOT}"
echo "Start with: ${REMOTE_ROOT}/scripts/start_host.sh"
