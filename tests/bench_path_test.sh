#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d)
trap 'rm -rf "$test_root"' EXIT

cp "$repo_root/sqlsprocessor" "$test_root/sqlsprocessor"
cp "$repo_root/bench_workload_generator" "$test_root/bench_workload_generator"
mkdir -p "$test_root/data/legacy"

cd "$test_root"
./bench_workload_generator \
  --profile smoke \
  --seed 20260415 \
  --preload 5 \
  --update-rows 2 \
  --delete-rows 2 \
  --ops 5 \
  --output-dir generated_sql

test -f data/legacy/jungle_benchmark_users.csv
test -f data/legacy/jungle_workload_users.csv
test -f generated_sql/jungle_insert_smoke.sql
test -f generated_sql/jungle_update_smoke.sql
test -f generated_sql/jungle_delete_smoke.sql

echo "bench_path_test: ok"
