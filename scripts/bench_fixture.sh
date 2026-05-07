#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 6 ]; then
    echo "usage: $0 vpipe-dev dma-heap fixture out-prefix frames algo [threshold] [out-pgm]" >&2
    exit 1
fi

vpipe_dev=$1
heap=$2
fixture=$3
out_prefix=$4
frames=$5
algo=$6
threshold=${7:-127}
out_pgm=${8:-}

mkdir -p "$(dirname "$out_prefix")"
rm -f "${out_prefix}.csv" "${out_prefix}.perf.csv"

args=(
    user/vpipe-bench-fixture
    "$vpipe_dev"
    "$heap"
    "$fixture"
    "${out_prefix}.csv"
    "$frames"
    "$algo"
    "$threshold"
)

if [ -n "$out_pgm" ]; then
    args+=("$out_pgm")
fi

scripts/bench_perf.sh "$out_prefix" "${args[@]}"
