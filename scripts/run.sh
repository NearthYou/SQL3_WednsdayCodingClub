#!/bin/sh
set -eu

DBSV_PORT="${DBSV_PORT:-8080}"
DB_ROOT="${DB_ROOT:-data}"

make build
exec env DBSV_PORT="$DBSV_PORT" DB_ROOT="$DB_ROOT" ./bin/dbsrv
