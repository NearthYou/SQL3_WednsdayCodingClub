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

mkdir -p "$bench_dir"
mkdir -p "$repo_dir/artifacts/bench"
mkdir -p "$bench_dir/data"

cp "$repo_dir/Makefile" "$bench_dir/Makefile"
cp -R "$repo_dir/src" "$bench_dir/src"
cp -R "$repo_dir/tools" "$bench_dir/tools"
cp -R "$repo_dir/scripts" "$bench_dir/scripts"

if [ -d "$repo_dir/data" ]; then
    cp -R "$repo_dir/data/." "$bench_dir/data/"
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
