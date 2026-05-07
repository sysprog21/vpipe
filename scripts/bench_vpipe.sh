#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 5 ]; then
    echo "usage: $0 vivid-dev vpipe-dev out-prefix frames transport [algo] [threshold]" >&2
    exit 1
fi

vivid_dev=$1
vpipe_dev=$2
out_prefix=$3
frames=$4
transport=$5
algo=${6:-none}
threshold=${7:-127}

mkdir -p "$(dirname "$out_prefix")"
rm -f "${out_prefix}.csv" "${out_prefix}.meta.csv" "${out_prefix}.perf.csv"

user/vpipe-meta-drain /dev/vpipe-meta "${out_prefix}.meta.csv" "$frames" &
meta_pid=$!

cleanup() {
    if kill -0 "$meta_pid" 2>/dev/null; then
        kill "$meta_pid" 2>/dev/null || true
        wait "$meta_pid" 2>/dev/null || true
    fi
}

trap cleanup EXIT

# Wait for meta-drain to open its CSV (proves /dev/vpipe-meta open and
# vpipe_open_csv both succeeded) before launching the benchmark, so a
# meta-drain failure is not masked by a blind sleep.
deadline=$(( SECONDS + 10 ))
while [ ! -e "${out_prefix}.meta.csv" ]; do
    if ! kill -0 "$meta_pid" 2>/dev/null; then
        echo "vpipe-meta-drain exited before becoming ready" >&2
        exit 1
    fi
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "vpipe-meta-drain readiness timeout" >&2
        exit 1
    fi
    sleep 0.1
done

scripts/bench_perf.sh "$out_prefix" \
    user/vpipe-capture-m2m "$vivid_dev" "$vpipe_dev" "${out_prefix}.csv" \
    "$frames" "$transport" "$algo" "$threshold"

wait "$meta_pid"
trap - EXIT
