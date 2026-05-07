#!/usr/bin/env bash
set -euo pipefail

frames=${1:-600}
video=${2:-/dev/video0}
out_dir=${3:-bench}

mkdir -p "$out_dir"

scripts/bench_perf.sh "$out_dir/mmap" user/vpipe-capture-mmap "$video" "$out_dir/mmap.csv" "$frames"
scripts/bench_perf.sh "$out_dir/read" user/vpipe-capture-read "$video" "$out_dir/read.csv" "$frames"
