#!/bin/sh
set -eu

PORT="${DBSV_PORT:-8080}"
ROOT="${DB_ROOT:-data}"
N="${LOAD_N:-100}"
C="${LOAD_C:-10}"
PY="${PYTHON:-python3}"
URL="http://127.0.0.1:${PORT}/api/v1/page?user_id=1&lat=37.5&lng=127.0"
TMP_DIR=$(mktemp -d /tmp/dbloadXXXXXX)
RES_FILE="${TMP_DIR}/res.txt"

now_ms() {
  "$PY" - <<'PY'
import time
print(int(time.time() * 1000))
PY
}

cleanup() {
  if [ -n "${PID:-}" ]; then
    kill "$PID" >/dev/null 2>&1 || true
    wait "$PID" >/dev/null 2>&1 || true
  fi
  rm -rf "$TMP_DIR"
}

trap cleanup EXIT INT TERM

make build >/dev/null
DBSV_PORT="$PORT" DB_ROOT="$ROOT" ./bin/dbsrv >/tmp/dbsrv_load.log 2>&1 &
PID=$!
sleep 1

START_MS=$(now_ms)
i=1
while [ "$i" -le "$N" ]; do
  batch=0
  pids=""
  while [ "$batch" -lt "$C" ] && [ "$i" -le "$N" ]; do
    (
      if curl -sf "$URL" >/dev/null; then
        echo ok >> "$RES_FILE"
      else
        echo fail >> "$RES_FILE"
      fi
    ) &
    pids="$pids $!"
    batch=$((batch + 1))
    i=$((i + 1))
  done
  for p in $pids; do
    wait "$p"
  done
done
END_MS=$(now_ms)

SUCCESS=$(grep -c '^ok$' "$RES_FILE" 2>/dev/null || true)
FAIL=$(grep -c '^fail$' "$RES_FILE" 2>/dev/null || true)
TOTAL_MS=$((END_MS - START_MS))
AVG_MS=$((TOTAL_MS / N))

printf 'Total requests: %s\n' "$N"
printf 'Concurrency: %s\n' "$C"
printf 'Success: %s\n' "$SUCCESS"
printf 'Fail: %s\n' "$FAIL"
printf 'Avg latency: %sms\n' "$AVG_MS"
