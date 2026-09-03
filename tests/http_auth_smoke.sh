#!/usr/bin/env bash
# End-to-end check that the HTTP layer enforces bearer-token authentication and
# reuses the shared policy decision. Args: <pdal-binary> <source-dir>
set -euo pipefail

PDAL_BIN=${1:?usage: http_auth_smoke.sh <pdal-binary> <source-dir>}
SRC=${2:?usage: http_auth_smoke.sh <pdal-binary> <source-dir>}

WORK=$(mktemp -d)
cleanup() {
  [[ -n "${SERVER_PID:-}" ]] && kill "${SERVER_PID}" 2>/dev/null || true
  [[ -n "${SERVER_PID:-}" ]] && wait "${SERVER_PID}" 2>/dev/null || true
  rm -rf "${WORK}"
}
trap cleanup EXIT

mkdir -p "${WORK}/ssd" "${WORK}/hdd"
SECRET_FILE="${WORK}/auth-secret"
printf 'smoke-test-secret-0123456789-abcdef' > "${SECRET_FILE}"

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
BASE="http://127.0.0.1:${PORT}"

cat > "${WORK}/pdal.yaml" <<YAML
resource_catalog: ${SRC}/config/resources.yaml
policy: ${SRC}/demo/pi/config/policy.yaml
audit_log: ${WORK}/audit.jsonl
continuation_secret: smoke-test-continuation-secret
storage:
  ssd_root: ${WORK}/ssd
  hdd_root: ${WORK}/hdd
runtime:
  max_concurrent_bulk_queries: 2
  max_live_subscriptions: 0
  live_queue_bytes: 1048576
auth:
  required: true
  issuer: pdal-smoke-issuer
  audience: pdal
  algorithm: HS256
  hmac_secret_file: auth-secret
  clock_skew_s: 60
server:
  address: 127.0.0.1
  port: ${PORT}
  demo_html: ${WORK}/none.html
YAML

"${PDAL_BIN}" serve --config "${WORK}/pdal.yaml" &
SERVER_PID=$!

ready=0
for _ in $(seq 1 50); do
  if curl -s -o /dev/null "${BASE}/pdal/v1"; then ready=1; break; fi
  sleep 0.1
done
[[ "${ready}" == 1 ]] || { echo "server did not start" >&2; exit 1; }

TOKEN=$(python3 "${SRC}/demo/scripts/mint_token.py" \
  --secret-file "${SECRET_FILE}" \
  --issuer pdal-smoke-issuer --audience pdal \
  --subject oem-fleet-analyst --role fleet_analyst --org oem-demo --ttl 300)

code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
fail=0
expect() {
  local what=$1 want=$2 got=$3
  if [[ "${got}" != "${want}" ]]; then
    echo "FAIL ${what}: expected ${want}, got ${got}" >&2
    fail=1
  else
    echo "ok   ${what} (${got})"
  fi
}

expect "health is public" 200 "$(code "${BASE}/pdal/v1")"
expect "capabilities is public" 200 "$(code "${BASE}/pdal/v1/capabilities")"
expect "discovery without token is 401" 401 "$(code "${BASE}/pdal/v1/resources")"
expect "spoofed identity headers are 401" 401 \
  "$(code -H 'X-PDAL-Principal: attacker' -H 'X-PDAL-Role: incident_investigator' "${BASE}/pdal/v1/resources")"
expect "forged token is 401" 401 \
  "$(code -H 'Authorization: Bearer not.a.token' "${BASE}/pdal/v1/resources")"
expect "valid token reaches discovery" 200 \
  "$(code -H "Authorization: Bearer ${TOKEN}" "${BASE}/pdal/v1/resources")"

denied=$(code -X POST -H "Authorization: Bearer ${TOKEN}" -H 'Content-Type: application/json' \
  -d '{"purpose":"fleet-monitoring","resources":["camera.front"],"time":{"start_ns":1,"end_ns":2},"representation":{"format":"jpeg"},"delivery":{"mode":"metadata","max_records":1,"max_bytes":1}}' \
  "${BASE}/pdal/v1/query")
expect "policy denial is 403 on the bulk path" 403 "${denied}"

allowed=$(code -X POST -H "Authorization: Bearer ${TOKEN}" -H 'Content-Type: application/json' \
  -d '{"purpose":"fleet-monitoring","resources":["position"],"time":{"start_ns":1,"end_ns":2},"representation":{"format":"metadata"},"delivery":{"mode":"metadata","max_records":1,"max_bytes":1}}' \
  "${BASE}/pdal/v1/query")
if [[ "${allowed}" == 401 || "${allowed}" == 403 ]]; then
  echo "FAIL authorized bulk request was blocked (${allowed})" >&2
  fail=1
else
  echo "ok   authorized bulk request is not blocked (${allowed})"
fi

exit "${fail}"
