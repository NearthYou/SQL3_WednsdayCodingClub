#!/bin/sh
set -eu

repo_dir=$1
update_rows=$2
delete_rows=$3
tmp_root=${TMPDIR:-/tmp}
bench_dir="$tmp_root/sqlprocessor-bench-$$"

cleanup() {
    rm -rf "$bench_dir"
}
trap cleanup EXIT INT TERM

mkdir -p "$bench_dir/src"
mkdir -p "$bench_dir/tools/bench"
mkdir -p "$repo_dir/artifacts/bench"

for file in \
    Makefile \
    src/main.c src/lexer.c src/parser.c src/bptree.c src/executor.c \
    src/lexer.h src/parser.h src/bptree.h src/executor.h src/types.h \
    tools/bench/bench_workload_generator.c tools/bench/benchmark_runner.c tools/bench/bench_formula_test.c
do
    cp "$repo_dir/$file" "$bench_dir/$file"
done

if [ -f "$repo_dir/jungle_benchmark_users.csv" ]; then
    cp "$repo_dir/jungle_benchmark_users.csv" "$bench_dir/jungle_benchmark_users.csv"
fi

(
    cd "$bench_dir"
    make bench-score \
        BENCH_SCORE_IN_TMP=0 \
        BENCH_SCORE_UPDATE_ROWS="$update_rows" \
        BENCH_SCORE_DELETE_ROWS="$delete_rows"
)

rm -rf "$repo_dir/artifacts/bench"
mkdir -p "$repo_dir/artifacts/bench"
cp -R "$bench_dir/artifacts/bench/." "$repo_dir/artifacts/bench/"
