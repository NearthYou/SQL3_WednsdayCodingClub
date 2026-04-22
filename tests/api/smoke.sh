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
  echo "$body" | grep -Eq '"status"[[:space:]]*:[[:space:]]*"ok"' || {
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

echo "smoke.sh: OK"
