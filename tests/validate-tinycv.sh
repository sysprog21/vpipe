#!/usr/bin/env bash
set -euo pipefail

video=${1:-/dev/video0}
bench_dir=${2:-bench}

mkdir -p "$bench_dir"

for fixture in tests/fixtures/*.pgm; do
    base=$(basename "$fixture" .pgm)
    cp "$fixture" "$bench_dir/${base}.input.pgm"
    user/vpipe-cv-ref "$fixture" "$bench_dir/${base}.reference.pgm" 127
    user/vpipe-fixture-feed "$video" /dev/dma_heap/system "$fixture" "$bench_dir/${base}.kernel.pgm"
    cmp "$bench_dir/${base}.reference.pgm" "$bench_dir/${base}.kernel.pgm"
    user/vpipe-pgm-diff "$bench_dir/${base}.reference.pgm" "$bench_dir/${base}.kernel.pgm" "$bench_dir/${base}.diff.pgm"
done
