#!/usr/bin/env bash
# Live demonstration of the pDAL security boundary: authentication, role
# authorization, and quota narrowing. Prints one line per check.
#
#   demo/scripts/security_demo.sh --secret-file demo/pi/config/auth-secret
#
# Options:
#   --pdal-url URL       (default http://127.0.0.1:8080)
#   --secret-file PATH    HS256 secret pDAL verifies                (required)
#   --gateway-url URL     also demo the per-role password login     (optional)
#   --ssd-db PATH         AVS catalog for a real time range
#   --issuer / --audience must match the running pDAL config
set -uo pipefail

PDAL_URL=http://127.0.0.1:8080
GATEWAY_URL=
SECRET_FILE=
SSD_DB=/home/avs/DATA/SSD/global.sqlite3
ISS=pdal-local-issuer
AUD=pdal
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pdal-url) PDAL_URL=$2; shift 2;;
    --gateway-url) GATEWAY_URL=$2; shift 2;;
    --secret-file) SECRET_FILE=$2; shift 2;;
    --ssd-db) SSD_DB=$2; shift 2;;
    --issuer) ISS=$2; shift 2;;
    --audience) AUD=$2; shift 2;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done
[[ -n "${SECRET_FILE}" && -s "${SECRET_FILE}" ]] || { echo "--secret-file is required" >&2; exit 2; }

fail=0
mint() { python3 "${SRC}/demo/scripts/mint_token.py" --secret-file "${SECRET_FILE}" \
  --issuer "${ISS}" --audience "${AUD}" --org oem-demo "$@"; }
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
check() {  # label expected actual
  if [[ "$3" == "$2" ]]; then printf '  ok   %-42s %s\n' "$1" "$3"
  else printf '  FAIL %-42s got %s want %s\n' "$1" "$3" "$2"; fail=1; fi
}
note() { printf '  --   %-42s %s\n' "$1" "$2"; }

GOOD=$(mint --subject demo --role incident_investigator --ttl 300)

echo "[1] Authentication (pDAL ${PDAL_URL})"
check "no token -> 401"              401 "$(code "${PDAL_URL}/pdal/v1/resources")"
check "spoofed X-PDAL-Role -> 401"   401 "$(code -H 'X-PDAL-Role: incident_investigator' -H 'X-PDAL-Principal: x' "${PDAL_URL}/pdal/v1/resources")"
check "garbage token -> 401"         401 "$(code -H 'Authorization: Bearer not.a.jwt' "${PDAL_URL}/pdal/v1/resources")"
check "valid token -> 200"           200 "$(code -H "Authorization: Bearer ${GOOD}" "${PDAL_URL}/pdal/v1/resources")"
TAMPERED="${GOOD:0:34}$([[ "${GOOD:34:1}" == "A" ]] && echo B || echo A)${GOOD:35}"
check "tampered token -> 401"        401 "$(code -H "Authorization: Bearer ${TAMPERED}" "${PDAL_URL}/pdal/v1/resources")"
check "wrong issuer -> 401"          401 "$(code -H "Authorization: Bearer $(mint --subject d --role incident_investigator --issuer other --ttl 300)" "${PDAL_URL}/pdal/v1/resources")"
check "expired token -> 401"         401 "$(code -H "Authorization: Bearer $(mint --subject d --role incident_investigator --ttl -86400)" "${PDAL_URL}/pdal/v1/resources")"

echo "[2] Authorization (role x resource x purpose)"
if S=$(python3 -c "import sqlite3,sys;print(sqlite3.connect('file:${SSD_DB}?mode=ro',uri=True).execute('select min(start_ts_ns) from global').fetchone()[0])" 2>/dev/null) && [[ -n "$S" ]]; then
  E=$((S + 500000000))
  FLEET=$(mint --subject demo --role fleet_analyst --ttl 300)
  q() { code -X POST -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
    -d "{\"purpose\":\"$2\",\"resources\":[\"$3\"],\"time\":{\"start_ns\":$S,\"end_ns\":$E},\"representation\":{\"format\":\"metadata\"},\"delivery\":{\"mode\":\"metadata\",\"max_records\":1,\"max_bytes\":1}}" \
    "${PDAL_URL}/pdal/v1/query"; }
  check "fleet_analyst -> position -> 200"        200 "$(q "${FLEET}" fleet-monitoring position)"
  check "fleet_analyst -> lidar.top -> 403"       403 "$(q "${FLEET}" fleet-monitoring lidar.top)"
  check "fleet_analyst -> camera.front -> 403"    403 "$(q "${FLEET}" fleet-monitoring camera.front)"
  check "fleet_analyst wrong purpose -> 403"      403 "$(q "${FLEET}" incident-investigation position)"
  check "incident_investigator -> lidar.top -> 200" 200 "$(q "${GOOD}" incident-investigation lidar.top)"

  echo "[3] Quota narrowing (policy may only narrow)"
  WIDE=$((S + 3600000000000))   # request 1 hour; demo policy caps the span at 20 min
  META=$(curl -s -X POST -H "Authorization: Bearer ${GOOD}" -H 'Content-Type: application/json' \
    -d "{\"purpose\":\"incident-investigation\",\"resources\":[\"position\"],\"time\":{\"start_ns\":$S,\"end_ns\":$WIDE},\"representation\":{\"format\":\"metadata\"},\"delivery\":{\"mode\":\"metadata\",\"max_records\":99999,\"max_bytes\":1}}" \
    "${PDAL_URL}/pdal/v1/query")
  GRANTED=$(printf '%s' "${META}" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('time_range',{}).get('end_ns',''))" 2>/dev/null || true)
  if [[ -n "${GRANTED}" ]]; then
    note "requested end_ns" "${WIDE}"
    note "granted  end_ns" "${GRANTED} (span $(( (GRANTED - S) / 1000000000 )) s)"
    [[ "${GRANTED}" -lt "${WIDE}" ]] && printf '  ok   %-42s %s\n' "range was narrowed by policy" "yes" || { printf '  FAIL range not narrowed\n'; fail=1; }
  else
    note "quota narrowing" "no metadata returned (skipped)"
  fi

  echo "[3b] Camera privacy (human blur before the frame leaves pDAL)"
  CAM="/tmp/security_demo_cam.$$"
  curl -s -X POST "${PDAL_URL}/pdal/v1/query" \
    -H "Authorization: Bearer ${GOOD}" -H 'Content-Type: application/json' \
    -d "{\"purpose\":\"incident-investigation\",\"resources\":[\"camera.front\"],\"time\":{\"start_ns\":$S,\"end_ns\":$((S + 120000000000))},\"representation\":{\"format\":\"jpeg\"},\"delivery\":{\"mode\":\"stream\",\"max_records\":1,\"max_bytes\":268435456}}" \
    -o "${CAM}"
  if head -c 8 "${CAM}" | grep -q PDALSTR1 && python3 - "${CAM}" <<'PY'
import sys, struct
d = open(sys.argv[1], 'rb').read()
if d[:8] != b'PDALSTR1' or len(d) < 21:
    sys.exit(1)
ml = struct.unpack('>I', d[8:12])[0]
pl = struct.unpack('>Q', d[12:20])[0]
payload = d[20 + ml: 20 + ml + pl]
sys.exit(0 if payload[:3] == b'\xff\xd8\xff' and pl > 1000 else 1)
PY
  then
    printf '  ok   %-42s %s\n' "camera.front returns a valid JPEG frame" "yes"
    note "verification" "run tests/privacy_tests or re-detect: no person should be found"
  else
    note "camera privacy" "no camera frame in this window (skipped)"
  fi
  rm -f "${CAM}"
else
  note "authorization + quota + privacy checks" "no AVS catalog at ${SSD_DB} (skipped)"
fi

if [[ -n "${GATEWAY_URL}" ]]; then
  echo "[4] Per-role password login (gateway ${GATEWAY_URL})"
  check "no session -> /api/trips 401" 401 "$(code "${GATEWAY_URL}/api/trips")"
  check "wrong password -> 401" 401 "$(code -X POST -H 'Content-Type: application/json' -d '{"role":"incident_investigator","password":"nope"}' "${GATEWAY_URL}/api/login")"
  TOK=$(curl -s -X POST -H 'Content-Type: application/json' -d '{"role":"fleet_analyst","password":"fleet-demo"}' "${GATEWAY_URL}/api/login" | python3 -c "import json,sys;print(json.load(sys.stdin).get('token',''))" 2>/dev/null || true)
  if [[ -n "${TOK}" ]]; then
    check "good password -> session -> /api/trips 200" 200 "$(code -H "Authorization: Bearer ${TOK}" "${GATEWAY_URL}/api/trips")"
  else
    note "login" "no token returned (is roles.auth.json present with demo passwords?)"
  fi
fi

echo
[[ "${fail}" == 0 ]] && echo "security_demo: all checks passed" || echo "security_demo: FAILURES above"
exit "${fail}"
