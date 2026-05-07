#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 output-prefix command [args...]" >&2
    exit 1
fi

out_prefix=$1
shift

mkdir -p "$(dirname "$out_prefix")"
perf stat \
    -x, \
    -o "${out_prefix}.perf.csv" \
    -e cycles,instructions,cache-references,cache-misses,context-switches,cpu-migrations,page-faults \
    -- "$@"
