#!/usr/bin/env bash
set -euo pipefail

DEMO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
PDAL_BIN=${PDAL_BIN:-"${DEMO_ROOT}/build/pdal"}
PDAL_CONFIG=${PDAL_CONFIG:-"${DEMO_ROOT}/demo/pi/config/pdal.yaml"}
CONF_DIR="${DEMO_ROOT}/demo/pi/config"
AUTH_SECRET_FILE=${AUTH_SECRET_FILE:-"${CONF_DIR}/auth-secret"}
ROLES_AUTH_FILE=${ROLES_AUTH_FILE:-"${CONF_DIR}/roles.auth.json"}
SESSION_SECRET_FILE=${SESSION_SECRET_FILE:-"${CONF_DIR}/session-secret"}

# ONNX Runtime for the camera privacy stage (git-ignored; downloaded once).
if [[ ! -f "${DEMO_ROOT}/third_party/onnxruntime/lib/libonnxruntime.so" ]]; then
  "${DEMO_ROOT}/scripts/fetch_privacy_deps.sh" || \
    echo "WARNING: could not fetch ONNX Runtime; camera requests will fail closed" >&2
fi

if [[ ! -x "${PDAL_BIN}" ]]; then
  cmake -S "${DEMO_ROOT}" -B "${DEMO_ROOT}/build" \
    -DPDAL_AVS_SOURCE_ROOT=/home/avs/AVS-PI/src/avs \
    -DPDAL_WITH_ROS_LIVE=OFF
  cmake --build "${DEMO_ROOT}/build" -j2
fi

# Shared bearer-token signing secret for pDAL and the gateway. Generated once,
# git-ignored, readable only by the current user.
if [[ ! -s "${AUTH_SECRET_FILE}" ]]; then
  ( umask 077; openssl rand -hex 32 > "${AUTH_SECRET_FILE}" )
  echo "generated ${AUTH_SECRET_FILE}" >&2
fi
chmod 600 "${AUTH_SECRET_FILE}" 2>/dev/null || true
export PDAL_AUTH_SECRET
PDAL_AUTH_SECRET=$(cat "${AUTH_SECRET_FILE}")

# Per-role login for the gateway. Copy the committed example on first run so the
# demo works out of the box (passwords: fleet-demo / service-demo / incident-demo).
if [[ ! -s "${ROLES_AUTH_FILE}" ]]; then
  cp "${CONF_DIR}/roles.auth.example.json" "${ROLES_AUTH_FILE}"
  echo "created ${ROLES_AUTH_FILE} from the example (change the passwords for anything real)" >&2
fi
if [[ ! -s "${SESSION_SECRET_FILE}" ]]; then
  ( umask 077; openssl rand -hex 32 > "${SESSION_SECRET_FILE}" )
fi
chmod 600 "${SESSION_SECRET_FILE}" 2>/dev/null || true

GATEWAY_ARGS=(--roles-auth-file "${ROLES_AUTH_FILE}" --session-secret-file "${SESSION_SECRET_FILE}")

# Demo mode: 60-second sessions so token expiry is easy to show.
if [[ "${DEMO_SHORT_TTL:-0}" == "1" ]]; then
  GATEWAY_ARGS+=(--short-ttl)
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
    python3 "${DEMO_ROOT}/demo/pi/gateway.py" \
      --auth-secret-file "${AUTH_SECRET_FILE}" \
      "${GATEWAY_ARGS[@]}" "$@"
    exit $?
  fi
  sleep 0.2
done

echo "pDAL did not become ready on 127.0.0.1:8080" >&2
exit 1
