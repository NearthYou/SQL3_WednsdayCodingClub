#!/bin/sh
set -eu

PORT="${DBSV_PORT:-18080}"
ROOT="${DB_ROOT:-data}"
PY="${PYTHON:-python3}"

DBSV_PORT="$PORT" DB_ROOT="$ROOT" ./bin/dbsrv >/tmp/dbsrv_api.log 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT INT TERM

sleep 1

curl -s "http://127.0.0.1:${PORT}/api/v1/health" | grep '"status":"up"'
curl -s -X POST "http://127.0.0.1:${PORT}/api/v1/sql" \
  -H 'Content-Type: application/json' \
  -d '{"query":"SELECT * FROM restaurants WHERE zone = '\''seoul_east'\''"}' | grep '"count":2'
curl -s -X POST "http://127.0.0.1:${PORT}/api/v1/batch" \
  -H 'Content-Type: application/json' \
  -d '{"tx":false,"queries":["SELECT * FROM restaurants WHERE zone = '\''seoul_east'\''","SELECT * FROM coupons WHERE user_id = 1"]}' | grep '"ok":true'
curl -s -X POST "http://127.0.0.1:${PORT}/api/v1/tx" \
  -H 'Content-Type: application/json' \
  -d '{"queries":["INSERT INTO cart VALUES (9, 9, 1, 7000)","INVALID SQL"]}' | grep '"code":"TX_ABORT"'
curl -s "http://127.0.0.1:${PORT}/api/v1/page?user_id=1&lat=37.5&lng=127.0" | grep '"trace":'
curl -s "http://127.0.0.1:${PORT}/api/v1/metrics" | grep '"mvcc":'
curl -s -X POST "http://127.0.0.1:${PORT}/api/v1/sql" -H 'Content-Type: application/json' -d '' | grep '"code":"BAD_REQ"'
curl -s -X POST "http://127.0.0.1:${PORT}/api/v1/sql" -H 'Content-Type: application/json' -d '{' | grep '"code":"BAD_JSON"'
curl -s -X POST "http://127.0.0.1:${PORT}/api/v1/sql" -H 'Content-Type: application/json' -d '{"oops":1}' | grep '"code":"BAD_REQ"'
curl -s -X PUT "http://127.0.0.1:${PORT}/api/v1/health" | grep '"code":"BAD_METH"'
curl -s "http://127.0.0.1:${PORT}/api/v1/nope" | grep '"code":"NO_ROUTE"'

BODY=$("$PY" - <<'PY'
print('{"queries":[' + ','.join(['"SELECT * FROM users WHERE id = 1"'] * 33) + ']}')
PY
)
curl -s -X POST "http://127.0.0.1:${PORT}/api/v1/batch" -H 'Content-Type: application/json' -d "$BODY" | grep '"code":"TOO_MANY"'

BIG=$("$PY" - <<'PY'
print('{"query":"' + ('A' * 1048600) + '"}')
PY
)
printf '%s' "$BIG" >/tmp/api_big.json
code=$(curl -s -o /tmp/api_big.out -w '%{http_code}' -X POST "http://127.0.0.1:${PORT}/api/v1/sql" -H 'Content-Type: application/json' --data-binary @/tmp/api_big.json)
[ "$code" = "413" ]
grep '"code":"TOO_BIG"' /tmp/api_big.out >/dev/null

echo "api_test: ok"
