# vpipe

`vpipe` is a Linux-only V4L2 mem2mem prototype for measuring the cost of
a camera-to-userspace frame path: copies, queueing, context switches,
scheduler jitter, cache behavior, and a small deterministic kernel-side
preprocessing step. It is not a vision stack.

The driving question:

> Which costs in the frame path come from copies, context switches, buffer
> queueing, scheduler jitter, and cache behavior?

To keep that measurable, the baseline is constrained to single-plane
`V4L2_PIX_FMT_GREY` at 640×480, one mem2mem node, one metadata sideband
miscdevice, one threshold algorithm over a clamped ROI, and deterministic
fixture-driven validation before any live camera path.

## Architecture

```text
┌──────────────────────────── userspace ────────────────────────────┐
│                                                                   │
│   vivid /dev/video0          vpipe /dev/videoN     /dev/vpipe-meta│
│        │                         ▲      │              │          │
│        │ VIDIOC_DQBUF            │      │ VIDIOC_DQBUF │ read(2)  │
│        │      (CAPTURE)          │      │   (CAPTURE)  │          │
│        ▼                         │      ▼              ▼          │
│   correlate src_v4l2_sequence    │   write CSV / PGM artifacts    │
│        │                         │                                │
│        └──► VIDIOC_QBUF (OUTPUT, DMABUF or MMAP) ──┐              │
│                                                    │              │
└────────────────────────────────────────────────────┼──────────────┘
                                                     ▼
┌─────────────────────────── kmod/vpipe.ko ─────────────────────────┐
│   OUTPUT queue ──► Tiny CV (threshold over ROI) ──► CAPTURE queue │
│                              │                                    │
│                              └──► /dev/vpipe-meta (ring buffer)   │
│                                                                   │
│   src_v4l2_sequence, timestamp_ns, algo_id, algo_status, ROI,     │
│   algo_value0/1, flags  → one row per processed frame             │
└───────────────────────────────────────────────────────────────────┘
```

Userspace owns orchestration, sequence correlation, and artifact capture.
The kernel owns transport mechanics and a deliberately small image
transform. Metadata is a separate device so transport timing and
algorithm output can be correlated without overloading the pixel
payload. See `docs/design.md` for the ownership model.

## Data Path Concretely

The mem2mem node accepts source frames on its OUTPUT queue (either
imported via `V4L2_MEMORY_DMABUF` or staged through `V4L2_MEMORY_MMAP`)
and produces processed frames on its CAPTURE queue. Per-buffer controls
(`VPIPE_CID_SRC_SEQUENCE`, `VPIPE_CID_ALGO`, `VPIPE_CID_THRESHOLD`,
ROI controls) are snapshotted at QBUF time so concurrent control
updates cannot race a frame already in flight.

The fixture-driven path uses `dma-heap` (`/dev/dma_heap/system`) for
deterministic source allocation: one explicit userspace `memcpy()` from
fixture bytes into the heap mapping, then DMABUF transport into vpipe.
This makes the copy count auditable and prevents accidental zero-copy
claims on virtual devices.

## Components

Kernel side, in `kmod/`:

- `vpipe-m2m.c` — V4L2 mem2mem node, queueing, format negotiation,
  per-buffer control snapshotting, `device_run()` entry point
- `vpipe-meta.c` — metadata miscdevice with per-open reader cursors
  over a shared ring; overruns catch readers up to the current window
- `vpipe-cv.c` — bounded Tiny CV; currently threshold over a clamped
  GREY ROI, no floating point or hot-path allocation
- `vpipe.h` — shared UAPI: ioctls, control IDs, metadata layout

Userspace side, in `user/` (binaries are kebab-case):

- `vpipe-capture-mmap`, `vpipe-capture-read` — Phase 1 baselines
- `vpipe-capture-dmabuf` — DMABUF transport exerciser via vivid `EXPBUF`
- `vpipe-capture-m2m` — full vivid → vpipe pipeline with selectable
  DMABUF or MMAP OUTPUT transport
- `vpipe-bench-fixture` — repeated heap-backed fixture transport bench
- `vpipe-meta-drain` — `read(/dev/vpipe-meta)` to CSV recorder
- `vpipe-fixture-feed` — deterministic single-shot fixture injection
- `vpipe-cv-ref` — userspace threshold reference for byte-for-byte
  comparison against the kernel output
- `vpipe-pgm-diff` — absolute per-pixel PGM diff generator

## Build And Validation

Linux-only; the top-level `Makefile` does not enter a guest
automatically. The reference validation environment is an Ubuntu 25.10
`aarch64` lima guest running kernel `6.17.0-22-generic`.

```sh
make             # install hooks (first run), then build kmod/ and user/
sudo make check  # validation suite (requires privileges)
```

`make check` runs userspace + kernel builds, the userspace unit tests
(CRC32, PGM I/O, threshold reference), vivid enumeration, module load,
fixture-driven metadata sanity (sequence contiguity and algo state),
a short Phase 1 mmap capture, the Phase 5 UAPI-state probe, and the
full Tiny CV fixture validation. Phase 5 is gated programmatically:
the suite fails loudly if the guest's V4L2 headers ever grow
`V4L2_BUF_FLAG_IN_FENCE`, `V4L2_BUF_FLAG_OUT_FENCE`, or a `fence_fd`
field, forcing a Phase 5 reopen rather than silent acceptance.

Longer-run measurement entrypoints:

- `sudo scripts/bench_capture.sh 600 /dev/video0 bench`
- `sudo scripts/bench_vpipe.sh /dev/video0 /dev/videoN bench/dmabuf-none 600 dmabuf`
- `sudo scripts/bench_vpipe.sh /dev/video0 /dev/videoN bench/mmap-none 600 mmap`
- `sudo scripts/bench_fixture.sh /dev/videoN /dev/dma_heap/system tests/fixtures/ramp.pgm bench/heap-threshold 600 threshold`

## Validation Artifacts

Bench and `make check` runs write into a flat, gitignored `bench/`
directory using suffix-based naming:

- `bench/<run>.csv` — per-frame log (enqueue/dequeue ns, sequence,
  bytesused)
- `bench/<run>.meta.csv` — corresponding metadata sideband drain
- `bench/<run>.perf.csv` — `perf stat` counters for the run
- `bench/<fixture>.{input,reference,kernel,diff}.pgm` — Tiny CV review
  set per fixture

The review loop is `fixture → userspace reference → kernel output →
cmp → diff image`, so kernel-side image logic stays visually
inspectable rather than asserted.

## Measurement Model

Phase-oriented; values below are illustrative medians from the
2026-05-07 lima guest (full rows in `docs/benchmark.md`):

- Phase 1: vivid baselines — `read()` p50 31.3 ms, `mmap` p50 133.2 ms
  at 30 fps, both with zero `DQBUF` errors over 600-frame runs
- Phase 2: vivid `EXPBUF` → vpipe DMABUF — added latency p50 0.107 ms,
  exact `src_v4l2_sequence` correlation 0..599, no duplicates or gaps
- Phase 3: DMA-BUF variants — copyful `mmap` p50 0.100 ms vs.
  vivid-DMABUF p50 0.083 ms; heap-backed fixture path p50 3.4 µs with
  one explicit userspace fixture copy
- Phase 4: threshold over heap-DMABUF — p50 3.4 µs, p99 9.1 µs;
  byte-identical against the userspace reference for the full fixture
  set
- Phase 5: explicit sync — currently `N/A`; gated by the UAPI-state
  probe in `scripts/check.sh`

Each phase captures, where the path permits: frame interval and drops,
p50/p95/p99 latency, cycles and instructions, cache references and
misses, context switches, and the timestamp source used.

## Current Limits

- transport is GREY-only and narrow by design
- the ledger still has `TBD` rows where long-form measurement is missing
- the lima guest does not expose PMU events for `cycles`, `instructions`,
  or `cache-*`; `perf stat` reports them as `<not supported>`
- no V4L2 userspace out-fence claim: the validated guest exposes
  `request_fd` but no fence flags or `fence_fd` field
- `kmemleak` cannot be exercised here: the validated guest kernel
  lacks `CONFIG_DEBUG_KMEMLEAK` and does not expose the debugfs node
- some paths are validated for API shape and correlation before any
  zero-copy claim is made

## Licensing

MIT License. See `LICENSE`.
