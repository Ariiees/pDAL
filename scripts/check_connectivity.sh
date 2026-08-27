#!/usr/bin/env bash
set -euo pipefail

PI_URL=${PI_URL:-http://128.175.213.254:8090}
response=$(curl --silent --show-error --fail --max-time 5 "${PI_URL}/api/health")
python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["status"] == "ready"; print("Pi ↔ host connectivity OK; decode location:", d["decode_location"])' <<<"${response}"
