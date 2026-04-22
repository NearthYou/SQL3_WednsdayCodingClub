#!/bin/sh
set -eu

ROOT="${DB_ROOT:-data}"
PY="${PYTHON:-python3}"

if [ "${DBSV_PORT:-}" = "" ]; then
PORT=$((20000 + $$ % 20000))
else
PORT="$DBSV_PORT"
fi

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

curl -s "http://127.0.0.1:${PORT}/demo" | grep '<!doctype html>'
curl -s "http://127.0.0.1:${PORT}/demo.css" | grep 'race-board'
curl -s "http://127.0.0.1:${PORT}/demo.js" | grep 'runConcurrencyRace'
curl -s -D /tmp/demo_headers.out -o /tmp/demo_body.out "http://127.0.0.1:${PORT}/demo" >/dev/null
grep -i '^Content-Type: text/html; charset=utf-8' /tmp/demo_headers.out >/dev/null

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

python3 - "$PORT" <<'PY'
import http.client
import json
import socket
import sys
import time

port = int(sys.argv[1])
conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)

def req(method, path, body=None, headers=None):
    conn.request(method, path, body=body, headers=headers or {})
    res = conn.getresponse()
    payload = res.read().decode("utf-8")
    return res.status, payload

sel_body = json.dumps({"query":"SELECT * FROM restaurants WHERE zone = 'seoul_east'"})
upd_body = json.dumps({"query":"UPDATE restaurants SET status = 'open' WHERE id = 1"})
h = {"Content-Type": "application/json"}

status, body = req("POST", "/api/v1/sql", sel_body, h)
assert status == 200 and '"ok":true' in body

status, body = req("GET", "/api/v1/metrics")
assert status == 200
data1 = json.loads(body)["data"]
miss1 = data1["cache"]["misses"]
hit1 = data1["cache"]["hits"]

status, body = req("POST", "/api/v1/sql", sel_body, h)
assert status == 200 and '"ok":true' in body
status, body = req("GET", "/api/v1/metrics")
data2 = json.loads(body)["data"]
assert data2["cache"]["hits"] >= hit1 + 1
assert data2["http"]["keep_alive_reuse"] >= 1

status, body = req("POST", "/api/v1/sql", upd_body, h)
assert status == 200 and '"ok":true' in body
status, body = req("POST", "/api/v1/sql", sel_body, h)
assert status == 200 and '"ok":true' in body
status, body = req("GET", "/api/v1/metrics")
data3 = json.loads(body)["data"]
assert data3["cache"]["misses"] >= miss1 + 1

time.sleep(1.2)
status, body = req("POST", "/api/v1/sql", sel_body, h)
assert status == 200 and '"ok":true' in body
status, body = req("GET", "/api/v1/metrics")
data4 = json.loads(body)["data"]
assert data4["cache"]["misses"] >= data3["cache"]["misses"] + 1
assert "timing_ns" in data4 and "http" in data4 and "cache" in data4

conn.close()
sock = socket.create_connection(("127.0.0.1", port), timeout=5)
sock.sendall(
    b"GET /api/v1/metrics HTTP/1.1\r\n"
    b"Host: 127.0.0.1\r\n"
    b"Connection: close\r\n\r\n"
)
recv = b""
while True:
    chunk = sock.recv(4096)
    if not chunk:
        break
    recv += chunk
sock.close()
assert b"Connection: close" in recv
PY

echo "api_test: ok"
