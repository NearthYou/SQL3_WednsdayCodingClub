#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-8080}"
BIN="${BIN:-./mini_dbms_api}"
CONCURRENCY="${CONCURRENCY:-50}"
REQUESTS="${REQUESTS:-1000}"
MIX="${MIX:-read-heavy}"
REPORT="${REPORT:-artifacts/api-load/report.json}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN_ABS="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
WORKDIR="$(mktemp -d)"

mkdir -p "$ROOT_DIR/artifacts/api-load"
cp "$ROOT_DIR/examples/data/case_basic_users.csv" "$WORKDIR/case_basic_users.csv"

(cd "$WORKDIR" && "$BIN_ABS" >"$ROOT_DIR/artifacts/api-load/server.log" 2>&1) &
SERVER_PID=$!
trap 'kill $SERVER_PID >/dev/null 2>&1 || true; rm -rf "$WORKDIR"' EXIT
sleep 1

if ! kill -0 "$SERVER_PID" >/dev/null 2>&1; then
  echo "server failed to start"
  cat "$ROOT_DIR/artifacts/api-load/server.log" || true
  exit 1
fi

python3 "$ROOT_DIR/scripts/run_api_load_test.py" \
  --url "http://127.0.0.1:${PORT}/query" \
  --concurrency "$CONCURRENCY" \
  --requests "$REQUESTS" \
  --mix "$MIX" \
  --report "$ROOT_DIR/$REPORT"
