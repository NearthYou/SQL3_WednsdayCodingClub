#!/bin/sh
set -eu

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
PROFILE="${PROFILE:-smoke}"
VUS="${VUS:-30}"
DURATION="${DURATION:-30s}"
TARGET_REQUESTS="${TARGET_REQUESTS:-200000}"
MAX_DURATION="${MAX_DURATION:-20m}"

if [ "$PROFILE" = "massive" ]; then
  SCRIPT="scripts/k6_api_massive.js"
else
  SCRIPT="scripts/k6_api_load.js"
fi

run_k6() {
  if [ "$PROFILE" = "massive" ]; then
    BASE_URL="$BASE_URL" \
      VUS="$VUS" \
      TARGET_REQUESTS="$TARGET_REQUESTS" \
      MAX_DURATION="$MAX_DURATION" \
      "$@"
  else
    BASE_URL="$BASE_URL" \
      VUS="$VUS" \
      DURATION="$DURATION" \
      "$@"
  fi
}

if command -v k6 >/dev/null 2>&1; then
  run_k6 k6 run "$SCRIPT"
  exit 0
fi

echo "[info] local k6 not found. fallback to docker image grafana/k6"
if [ "$PROFILE" = "massive" ]; then
  docker run --rm -i \
    -e BASE_URL="$BASE_URL" \
    -e VUS="$VUS" \
    -e TARGET_REQUESTS="$TARGET_REQUESTS" \
    -e MAX_DURATION="$MAX_DURATION" \
    -v "$(pwd):/work" \
    -w /work \
    grafana/k6:0.53.0 run "$SCRIPT"
else
  docker run --rm -i \
    -e BASE_URL="$BASE_URL" \
    -e VUS="$VUS" \
    -e DURATION="$DURATION" \
    -v "$(pwd):/work" \
    -w /work \
    grafana/k6:0.53.0 run "$SCRIPT"
fi
