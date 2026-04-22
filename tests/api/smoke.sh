#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-8080}"
BIN="${BIN:-./mini_dbms_api}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN_ABS="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
WORKDIR="$(mktemp -d)"

cp "$ROOT_DIR/examples/data/case_basic_users.csv" "$WORKDIR/case_basic_users.csv"

(cd "$WORKDIR" && "$BIN_ABS" >"$WORKDIR/server.log" 2>&1) &
SERVER_PID=$!
trap 'kill $SERVER_PID >/dev/null 2>&1 || true; rm -rf "$WORKDIR"' EXIT
sleep 1

if ! kill -0 "$SERVER_PID" >/dev/null 2>&1; then
  echo "server failed to start"
  cat "$WORKDIR/server.log" || true
  exit 1
fi

request_ok() {
  local sql="$1"
  local body
  body="$(curl -sS -X POST "http://127.0.0.1:${PORT}/query" \
    -H 'Content-Type: text/plain' \
    --data "$sql")"
  echo "$body" | grep '"status":"ok"' >/dev/null || {
    echo "unexpected response: $body"
    echo "server log:"
    cat "$WORKDIR/server.log" || true
    return 1
  }
}

request_ok "SELECT * FROM case_basic_users WHERE id = 1;"

request_ok "INSERT INTO case_basic_users VALUES (99999,'smoke@test.com','010-9999','pass999','SmokeUser');"

STATUS_GET="$(curl -sS -o /dev/null -w "%{http_code}" "http://127.0.0.1:${PORT}/query")"
test "$STATUS_GET" = "405"

STATUS_WRONG="$(curl -sS -o /dev/null -w "%{http_code}" -X POST "http://127.0.0.1:${PORT}/wrong" -H 'Content-Type: text/plain' --data "SELECT 1")"
test "$STATUS_WRONG" = "404"

python3 - "$PORT" <<'PY'
import http.client
import json
import socket
import sys
import time

port = int(sys.argv[1])

conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)

def request(method, path, body=None, headers=None):
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    payload = resp.read().decode("utf-8")
    return resp.status, payload

select_sql = "SELECT * FROM case_basic_users WHERE id = 1;"
insert_sql = "INSERT INTO case_basic_users VALUES (100001,'cache@test.com','010-1000','pass1000','CacheUser');"

status, body = request("POST", "/query", select_sql, {"Content-Type": "text/plain"})
assert status == 200 and '"status":"ok"' in body

status, body = request("GET", "/stats")
assert status == 200
stats_after_first = json.loads(body)
first_hits = stats_after_first["cache_hits"]
first_misses = stats_after_first["cache_misses"]
assert first_misses >= 1

status, body = request("POST", "/query", select_sql, {"Content-Type": "text/plain"})
assert status == 200 and '"status":"ok"' in body
status, body = request("GET", "/stats")
stats_after_hit = json.loads(body)
assert stats_after_hit["cache_hits"] >= first_hits + 1
assert stats_after_hit["keep_alive_reuse"] >= 1

status, body = request("POST", "/query", insert_sql, {"Content-Type": "text/plain"})
assert status == 200 and '"status":"ok"' in body
status, body = request("POST", "/query", select_sql, {"Content-Type": "text/plain"})
assert status == 200 and '"status":"ok"' in body
status, body = request("GET", "/stats")
stats_after_invalidation = json.loads(body)
assert stats_after_invalidation["cache_misses"] >= stats_after_hit["cache_misses"] + 1

time.sleep(1.2)
status, body = request("POST", "/query", select_sql, {"Content-Type": "text/plain"})
assert status == 200 and '"status":"ok"' in body
status, body = request("GET", "/stats")
stats_after_ttl = json.loads(body)
assert stats_after_ttl["cache_misses"] >= stats_after_invalidation["cache_misses"] + 1
conn.close()

sock = socket.create_connection(("127.0.0.1", port), timeout=5)
raw = (
    "GET /stats HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Connection: close\r\n"
    "\r\n"
)
sock.sendall(raw.encode("utf-8"))
chunks = []
while True:
    data = sock.recv(4096)
    if not data:
        break
    chunks.append(data)
payload = b"".join(chunks).decode("utf-8")
assert "Connection: close" in payload
PY

echo "smoke.sh: OK"
