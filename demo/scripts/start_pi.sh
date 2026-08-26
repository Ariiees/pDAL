#!/usr/bin/env bash
set -euo pipefail

DEMO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
PDAL_BIN=${PDAL_BIN:-"${DEMO_ROOT}/build/pdal"}
PDAL_CONFIG=${PDAL_CONFIG:-"${DEMO_ROOT}/demo/pi/config/pdal.yaml"}

if [[ ! -x "${PDAL_BIN}" ]]; then
  cmake -S "${DEMO_ROOT}" -B "${DEMO_ROOT}/build" \
    -DPDAL_AVS_SOURCE_ROOT=/home/avs/AVS-PI/src/avs \
    -DPDAL_WITH_ROS_LIVE=OFF
  cmake --build "${DEMO_ROOT}/build" -j2
fi

"${PDAL_BIN}" serve --config "${PDAL_CONFIG}" &
PDAL_PID=$!
cleanup() {
  kill "${PDAL_PID}" 2>/dev/null || true
  wait "${PDAL_PID}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for _ in $(seq 1 30); do
  if curl --silent --fail http://127.0.0.1:8080/pdal/v1 >/dev/null; then
    python3 "${DEMO_ROOT}/demo/pi/gateway.py" "$@"
    exit $?
  fi
  sleep 0.2
done

echo "pDAL did not become ready on 127.0.0.1:8080" >&2
exit 1
