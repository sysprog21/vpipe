# Benchmark Matrix

This file is the benchmark ledger for `vpipe`. Keep design rationale in
`docs/design.md`; use this file for measurable paths, validation notes, and the
artifact layout that supports those measurements.

## How To Read This File

Each phase row is trying to answer a specific transport question:

- how many copies are still present
- how many context switches the path induces
- which timestamps are authoritative
- whether the path is only API-valid or also performance-credible

The table values should be interpreted conservatively:

- `TBD` means the measurement is still missing and must not be inferred
- `see Phase N` means reuse of a previously measured reference row
- validation notes describe what has been proven so far, not what is assumed

## Artifact Layout

Generated artifacts live in a flat, gitignored `bench/` directory at the
repo root. The intent is to keep results regenerable rather than committed,
so file naming carries the role rather than directory hierarchy:

- `bench/<run>.csv`: frame-by-frame capture logs (e.g. `mmap.csv`,
  `read.csv`, `m2m.csv`, `dmabuf.csv`, `fixture.csv`)
- `bench/<run>.meta.csv`: metadata sideband drain when present
- `bench/<run>.perf.csv`: perf stat output for the corresponding run
- `bench/<fixture>.input.pgm`: original fixture copy
- `bench/<fixture>.reference.pgm`: userspace reference output
- `bench/<fixture>.kernel.pgm`: kernel path output
- `bench/<fixture>.diff.pgm`: absolute per-pixel diff

Caller-supplied prefixes (e.g. `bench/dmabuf-none`) follow the same flat
naming and gain the suffixes above.

## Measurement Checklist

When a phase is filled in, it should capture as many of these as the path
permits:

- frame interval and dropped frames
- median, p95, and p99 latency
- cycles and instructions
- cache references and misses
- context switches and scheduler effects
- timestamp source used for interpretation

## Phase 1

| Path | Copies / frame | Context switches / frame | Latency median / p95 / p99 | Cache refs / misses | Timestamp source | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| vivid `read()` | 1 userspace copy into the `read()` buffer | 1.047 | 31.323 / 36.289 / 37.478 ms | unsupported on the 2026-05-07 lima guest PMU | userspace `CLOCK_MONOTONIC` plus synthetic frame counter | 600-frame run completed at 30 fps with zero read errors; frame-interval stddev 2.599 ms; this tool still lacks native per-buffer V4L2 sequence reporting |
| vivid `mmap` | no userspace `memcpy()` in the capture tool; still a synthetic virtual-device path | 1.045 | 133.223 / 137.648 / 144.007 ms | unsupported on the 2026-05-07 lima guest PMU | V4L2 buffer timestamp and `CLOCK_MONOTONIC` | 600-frame run completed at 30 fps with zero `DQBUF` errors; frame-interval stddev 3.264 ms; vivid exposes a synthetic timestamp-source control, but the useful timing signal on this guest is `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC` rather than a meaningful hardware timestamp |

## Phase 2

| Path | Copies / frame | Context switches / frame | Latency median / p95 / p99 | Cache refs / misses | Timestamp source | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| vivid `EXPBUF` -> `vpipe` OUTPUT DMABUF -> `vpipe` CAPTURE MMAP | 1 bounded kernel full-frame copy in `device_run()`; no extra userspace copy after vivid dequeue | 1.655 | 0.107 / 0.194 / 0.294 ms | unsupported on the 2026-05-07 lima guest PMU | `CLOCK_MONOTONIC` at vivid/vpipe dequeue plus `vpipe_meta.timestamp_ns` | 600-frame run completed with exact `src_v4l2_sequence` correlation, no duplicates, and no gaps; median added latency after vivid dequeue was 0.107 ms; keep the copy-count claim explicit about the `vpipe_copy_frame()` work still present in `device_run()` |

Current validation note:

- the long-run metadata `src_v4l2_sequence` values matched the vivid sequences
  exactly for all 600 frames: `0 .. 599`
- `bench/dmabuf-none.meta.csv` and `bench/dmabuf-none.csv` were checked for
  exact equality,
  duplicates `0`, and gaps `0`
- the `kmemleak` debugfs interface is absent on this guest, so the
  `insmod` -> 600-frame run -> `rmmod` leak check still needs a kernel
  built with `CONFIG_DEBUG_KMEMLEAK`

## Phase 3

| Path | Copies / frame | Context switches / frame | Latency median / p95 / p99 | Cache refs / misses | Timestamp source | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| vivid `mmap` reference | see Phase 1 vivid `mmap` | see Phase 1 vivid `mmap` | see Phase 1 vivid `mmap` | see Phase 1 vivid `mmap` | see Phase 1 vivid `mmap` | 30 fps source baseline |
| vivid `mmap` -> `vpipe` `mmap` | 1 userspace `memcpy()` from vivid MMAP to `vpipe` OUTPUT MMAP plus 1 bounded kernel full-frame copy in `device_run()` | 1.633 | 0.100 / 0.216 / 0.284 ms | unsupported on the 2026-05-07 lima guest PMU | `CLOCK_MONOTONIC` at vivid/vpipe dequeue plus metadata sideband | 600-frame run completed with exact `src_v4l2_sequence` correlation; explicit copyful comparison path |
| vivid `EXPBUF` -> `vpipe` DMABUF | 1 bounded kernel full-frame copy in `device_run()`; do not call this zero-copy on a virtual path | 1.638 | 0.083 / 0.181 / 0.242 ms | unsupported on the 2026-05-07 lima guest PMU | `CLOCK_MONOTONIC` at vivid/vpipe dequeue plus metadata sideband | 600-frame run completed with exact `src_v4l2_sequence` correlation; lower median transport cost than the copyful `mmap` handoff |
| heap alloc -> `vpipe` DMABUF | 1 explicit userspace copy from fixture bytes into dma-heap plus 1 bounded kernel full-frame copy in `device_run()` | 0.908 | 0.003411 / 0.003874 / 0.007255 ms | unsupported on the 2026-05-07 lima guest PMU | userspace `CLOCK_MONOTONIC` around queue/dequeue | measured on the deterministic heap-backed fixture harness; this row exists to make the fixture-copy cost explicit, not to claim zero-copy transport |

Interpretation note:

- copy-count claims are backed by path descriptions in the userspace harnesses
  plus the measured context-switch counters above; they are not inferred from
  cache deltas alone
- the lima guest does not expose PMU hardware events for `cycles`,
  `instructions`, or `cache-*`, and `perf stat` reported those events as
  `<not supported>`

## Phase 4

| Path | Copies / frame | Context switches / frame | Latency median / p95 / p99 | Cache refs / misses | Timestamp source | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| threshold over heap alloc -> `vpipe` DMABUF GREY transport | 1 explicit userspace copy from fixture bytes into dma-heap plus 1 bounded kernel full-frame copy in `device_run()` | 0.920 | 0.003410 / 0.003834 / 0.009105 ms | unsupported on the 2026-05-07 lima guest PMU | userspace `CLOCK_MONOTONIC` around queue/dequeue | transport-matched Phase 4 row: compared against `heap alloc -> vpipe` with `algo=none`, median overhead was not measurable on this guest and p99 increased by about 1.85 us; full fixture set remains byte-identical before any second algorithm is considered |

Current validation note:

- the full fixture set under `tests/fixtures/*.pgm` passed byte-for-byte
  against the kernel path using `tests/validate-tinycv.sh`
- matching reference/kernel SHA256 values on 2026-05-07:
  `edge-block.pgm` `5eb9065a6fc396330bb6b1b0763fd939d1bb9688a758b7db09ca80b3a44ed6f0`
  `flat-bright.pgm` `f1a32e59b54d65041ae3397da0fbb8581db17be6325c6500c49a89aa52706f58`
  `flat-dark.pgm` `5eb9065a6fc396330bb6b1b0763fd939d1bb9688a758b7db09ca80b3a44ed6f0`
  `ramp.pgm` `f3ae7154f7c7eb05018c0fff7f9e2f35ee63129edb4986435986dbb8d329a42c`

Illustration:

```text
tests/fixtures/<fixture>.pgm
  -> user/vpipe-cv-ref
  -> bench/<fixture>.reference.pgm

tests/fixtures/<fixture>.pgm
  -> user/vpipe-fixture-feed
  -> bench/<fixture>.kernel.pgm

reference + kernel
  -> cmp
  -> user/vpipe-pgm-diff
  -> bench/<fixture>.diff.pgm
```

## Phase 5

| Path | Copies / frame | Context switches / frame | Latency median / p95 / p99 | Cache refs / misses | Timestamp source | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| explicit fence path | N/A | N/A | N/A | N/A | N/A | validated Ubuntu 25.10 lima guest (`6.17.0-22-generic`) exposes `request_fd` request support but no cited V4L2 userspace out-fence API, fence flags, or `fence_fd` field in the checked headers |

## Phase 6: Cost Decomposition

Phase 6 rows separate currently-aggregated costs into named knobs against the
heap-backed Phase 4 baseline. Each row names its column additions explicitly.

### USERPTR ingress (memory and cache cost)

Buffer model column distinguishes the two USERPTR usage patterns vivid + vb2
exposes; per-QBUF minor faults come from the `minor_faults_delta` column the
harness records around each `VIDIOC_QBUF` ioctl.

| Path | Buffer model | Per-QBUF minor faults (mean / p95 / max) | Latency median / p95 / p99 | mmap_lock_hold_us | Notes |
| --- | --- | --- | --- | --- | --- |
| vivid -> USERPTR (4-buffer pool) | fixed pool of 4 anon mmaps reused indefinitely | 0 / 0 / 0 | 132.778 / 137.110 / 138.562 ms | TBD (ftrace) | `__qbuf_userptr` caches the `(userptr, length)` -> pinned-page mapping per slot; re-QBUF on the same userptr reuses the existing pin and skips GUP entirely |
| vivid -> USERPTR (fresh per QBUF) | fresh anon `mmap` per re-QBUF, allocated before the previous one is freed so the kernel cannot reuse the VA | 75 / 75 / 75 | 132.745 / 137.901 / 166.039 ms | TBD (ftrace) | 75 == `sizeimage / PAGE_SIZE`; vb2 cache miss forces a full GUP slow-path on each QBUF, charged to the calling thread; per-QBUF re-pin inflates p99 by ~22 ms over the cached row without moving the median |

Validation conditions:

- 600-frame run on Ubuntu 25.10 lima guest (`6.17.0-22-generic`, aarch64),
  `vivid` 640x480 `V4L2_PIX_FMT_GREY` at 30 fps, 4 buffers
- harness: `user/vpipe-capture-userptr.c`
- summarizer: `scripts/summarize-benchmark.py phase_userptr <csv>`
- artifacts: `bench/userptr-noreset.csv`, `bench/userptr-reset.csv`
- `mmap_lock_hold_us` cell still depends on the ftrace `function_graph`
  capture filtered to `mmap_read_lock`/`mmap_write_lock` around `VIDIOC_QBUF`
  that the TODO calls out separately
