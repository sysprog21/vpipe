#!/usr/bin/env bash
set -euo pipefail

frames=${1:-600}
video=${2:-/dev/video0}
mode=${3:-reset}
out_dir=${4:-bench}

case "$mode" in
    reset|noreset) ;;
    *) echo "invalid mode '$mode' (expected reset|noreset)" >&2; exit 1 ;;
esac

mkdir -p "$out_dir"

scripts/bench_perf.sh "$out_dir/userptr-$mode" \
    user/vpipe-capture-userptr "$video" "$out_dir/userptr-$mode.csv" "$frames" "$mode"
