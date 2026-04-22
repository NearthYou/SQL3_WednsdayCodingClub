#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-8080}"
BIN="${BIN:-./mini_dbms_api}"
CONCURRENCY="${CONCURRENCY:-50}"
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

PIDS=()
for i in $(seq 1 "$CONCURRENCY"); do
  (
    if (( i % 4 == 0 )); then
      SQL="SELECT * FROM case_basic_users WHERE id = 1;"
    elif (( i % 4 == 1 )); then
      SQL="INSERT INTO case_basic_users VALUES ($((500000 + i)),'stress${i}@test.com','010-5${i}','pass${i}','Stress${i}');"
    elif (( i % 4 == 2 )); then
      SQL="UPDATE case_basic_users SET name = 'updated${i}' WHERE id = 1;"
    else
      SQL="DELETE FROM case_basic_users WHERE id = $((700000 + i));"
    fi
    curl -sS --max-time 5 -X POST "http://127.0.0.1:${PORT}/query" \
      -H 'Content-Type: text/plain' \
      --data "$SQL" | grep -q '"status":"'
  ) &
  PIDS+=("$!")
done

for pid in "${PIDS[@]}"; do
  wait "$pid"
done
echo "stress.sh: OK (concurrency=${CONCURRENCY})"
