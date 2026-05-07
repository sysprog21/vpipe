#!/usr/bin/env bash
set -euo pipefail

repo_root=$(pwd)
check_dir=${TMPDIR:-/tmp}/vpipe-check
phase4_bench_dir=$check_dir/phase4-bench
phase1_frames=${VPIPE_CHECK_PHASE1_FRAMES:-32}
transport_frames=${VPIPE_CHECK_TRANSPORT_FRAMES:-16}
check_dmabuf_wrapper=${VPIPE_CHECK_DMABUF_WRAPPER:-0}
fixture_count=$(find tests/fixtures -maxdepth 1 -name '*.pgm' | wc -l | tr -d ' ')

log() {
    printf '[check] %s\n' "$*"
}

cleanup() {
    # The suite intentionally launches short-lived helper processes in the
    # background; clean them aggressively so reruns start from a known state.
    pkill -f '/vpipe-meta-drain ' 2>/dev/null || true
    pkill -f '/vpipe-capture-dmabuf ' 2>/dev/null || true
    pkill -f '/vpipe-fixture-feed ' 2>/dev/null || true
    pkill -f '/vpipe-capture-mmap ' 2>/dev/null || true
    rmmod vpipe 2>/dev/null || true
}

find_vpipe_video() {
    local node
    for node in /sys/class/video4linux/video*; do
        [ -f "$node/name" ] || continue
        if [ "$(cat "$node/name")" = "vpipe" ]; then
            basename "$node"
            return 0
        fi
    done
    return 1
}

assert_file() {
    local path=$1
    [ -f "$path" ] || {
        echo "missing file: $path" >&2
        exit 1
    }
}

load_vpipe_module() {
    log "loading vpipe module"
    insmod kmod/vpipe.ko
    vpipe_video=$(find_vpipe_video)
    vpipe_dev="/dev/${vpipe_video}"
    log "detected vpipe device: ${vpipe_dev}"
}

verify_phase1_csv() {
    local csv_path=$1
    local expected_frames=$2

    python3 - "$csv_path" "$expected_frames" <<'PY'
import csv
import sys

path = sys.argv[1]
expected = int(sys.argv[2])

with open(path, newline="") as fh:
    rows = list(csv.DictReader(fh))

if len(rows) != expected:
    raise SystemExit(f"{path}: expected {expected} rows, got {len(rows)}")

last_sequence = None
for idx, row in enumerate(rows):
    sequence = int(row["sequence"])
    bytesused = int(row["bytesused"])
    enqueue_ns = int(row["enqueue_monotonic_ns"])
    dequeue_ns = int(row["dequeue_monotonic_ns"])

    if bytesused <= 0:
        raise SystemExit(f"{path}: row {idx} has non-positive bytesused")
    if dequeue_ns < enqueue_ns:
        raise SystemExit(f"{path}: row {idx} has negative latency")
    if last_sequence is not None and sequence != last_sequence + 1:
        raise SystemExit(
            f"{path}: non-contiguous sequence at row {idx}: "
            f"{last_sequence} -> {sequence}"
        )
    last_sequence = sequence
PY
}

verify_phase4_artifacts() {
    local bench_dir=$1

    python3 - "$bench_dir" <<'PY'
from pathlib import Path
import sys

bench_dir = Path(sys.argv[1])
if not bench_dir.is_dir():
    raise SystemExit(f"missing bench directory: {bench_dir}")
expected_bases = sorted(p.stem for p in Path("tests/fixtures").glob("*.pgm"))
for role in ("input", "reference", "kernel", "diff"):
    names = sorted(
        p.name[: -len(f".{role}.pgm")]
        for p in bench_dir.glob(f"*.{role}.pgm")
    )
    if names != expected_bases:
        raise SystemExit(
            f"{bench_dir}: role={role} expected {expected_bases}, got {names}"
        )
PY
}

verify_fixture_meta_csv() {
    local meta_csv=$1
    local expected_frames=$2

    python3 - "$meta_csv" "$expected_frames" <<'PY'
import csv
import sys

meta_path, expected = sys.argv[1], int(sys.argv[2])

with open(meta_path, newline="") as fh:
    meta = list(csv.DictReader(fh))

if len(meta) != expected:
    raise SystemExit(f"expected {expected} metadata rows, got {len(meta)}")

prev_seq = None
for idx, row in enumerate(meta):
    seq = int(row["seq"])
    if prev_seq is not None and seq != prev_seq + 1:
        raise SystemExit(
            f"non-contiguous seq at row {idx}: {prev_seq} -> {seq}"
        )
    prev_seq = seq
    if int(row["bytesused"]) <= 0:
        raise SystemExit(f"meta row {idx} has non-positive bytesused")
    if int(row["algo_id"]) != 1:
        raise SystemExit(f"meta row {idx} has unexpected algo_id {row['algo_id']}")
    if int(row["algo_status"]) != 0:
        raise SystemExit(f"meta row {idx} has unexpected algo_status {row['algo_status']}")
    if int(row["roi_width"]) <= 0 or int(row["roi_height"]) <= 0:
        raise SystemExit(f"meta row {idx} has invalid ROI")
PY
}

verify_phase5_uapi_state() {
    local public_header=/usr/include/linux/videodev2.h
    local krel
    local kernel_uapi
    local kernel_alt
    local headers=()

    krel=$(uname -r)
    kernel_uapi="/usr/src/linux-headers-${krel}/include/uapi/linux/videodev2.h"
    kernel_alt="/usr/src/linux-headers-${krel}/include/linux/videodev2.h"

    [ -f "$public_header" ] || {
        echo "missing public V4L2 header: $public_header" >&2
        exit 1
    }
    [ -f "$kernel_uapi" ] || {
        echo "missing kernel V4L2 header: $kernel_uapi" >&2
        exit 1
    }
    headers+=("$public_header" "$kernel_uapi")
    [ -f "$kernel_alt" ] && headers+=("$kernel_alt")

    if grep -Eq '\bV4L2_BUF_FLAG_(IN|OUT)_FENCE\b|\bfence_fd\b' "${headers[@]}"; then
        echo "Phase 5 must be reopened: guest V4L2 headers now expose fence UAPI" >&2
        exit 1
    fi

    grep -Eq '\brequest_fd\b' "${headers[@]}" || {
        echo "expected request_fd support in guest V4L2 headers" >&2
        exit 1
    }
}

trap cleanup EXIT

mkdir -p "$check_dir"
cleanup

log "building user tools"
make -C user
log "running userspace unit tests"
make -C user test
log "building kernel module"
make -C kmod

log "enumerating vivid devices"
bash scripts/run_vivid.sh >/tmp/vpipe-run-vivid.log

log "loading media helper modules"
modprobe v4l2-mem2mem
modprobe videobuf2-vmalloc

load_vpipe_module

if [ "$check_dmabuf_wrapper" = "1" ]; then
    # Keep the standalone wrapper opt-in until its state interactions are as
    # stable as the fixture-driven validation legs.
    log "running standalone dmabuf wrapper sanity check"
    rm -f "$check_dir/dmabuf.csv"
    user/vpipe-capture-dmabuf /dev/video0 "$vpipe_dev" "$check_dir/dmabuf.csv" "$transport_frames"
    assert_file "$check_dir/dmabuf.csv"
fi

log "running fixture-driven metadata sanity check"
rm -f "$check_dir/meta.csv"
user/vpipe-meta-drain /dev/vpipe-meta "$check_dir/meta.csv" "$fixture_count" &
meta_pid=$!
sleep 1
for fixture in tests/fixtures/*.pgm; do
    # Drive metadata publication from deterministic fixtures before any live path.
    user/vpipe-fixture-feed "$vpipe_dev" /dev/dma_heap/system "$fixture" /tmp/$(basename "$fixture")
done
wait "$meta_pid"
assert_file "$check_dir/meta.csv"
verify_fixture_meta_csv "$check_dir/meta.csv" "$fixture_count"

log "running phase 1 mmap sanity capture"
user/vpipe-capture-mmap /dev/video0 "$check_dir/phase1-mmap.csv" "$phase1_frames"
assert_file "$check_dir/phase1-mmap.csv"
verify_phase1_csv "$check_dir/phase1-mmap.csv" "$phase1_frames"

log "checking current V4L2 UAPI for Phase 5 fence support"
verify_phase5_uapi_state

log "reloading vpipe module for tinycv fixture validation"
rmmod vpipe
load_vpipe_module

log "running full tinycv fixture validation"
rm -rf "$phase4_bench_dir"
bash tests/validate-tinycv.sh "$vpipe_dev" "$phase4_bench_dir"
verify_phase4_artifacts "$phase4_bench_dir"

log "built-in validation suite passed"
