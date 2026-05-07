# vpipe Design

This file records architecture-specific decisions and the ownership model of
the current frame path. For the project overview, build commands, and repo
layout, see `README.md`.

## Design Goals

The implementation is shaped by a few constraints:

- keep kernel work bounded, deterministic, and easy to reason about
- keep the first measurable transport path small before generalizing formats
- preserve a userspace reference path for any kernel-side image logic
- make frame correlation explicit through metadata rather than implicit timing

## Execution Boundaries

```text
+-------------------------- userspace --------------------------+
|                                                              |
|  vivid capture toolchain                                     |
|    - dequeue source frames                                   |
|    - preserve source sequence numbers                        |
|    - feed vpipe OUTPUT queue                                 |
|                                                              |
|  vpipe result toolchain                                      |
|    - dequeue CAPTURE buffers                                 |
|    - drain /dev/vpipe-meta                                   |
|    - write CSV and PGM artifacts                             |
+-------------------------------+------------------------------+
                                |
                                v
+---------------------------- kmod/vpipe ----------------------+
|                                                              |
|  vpipe-m2m.c   transport, queueing, controls, sequencing     |
|  vpipe-cv.c    bounded Tiny CV work over GREY buffers        |
|  vpipe-meta.c  sideband metadata publication                 |
|  vpipe.h       userspace-visible UAPI                        |
|                                                              |
+--------------------------------------------------------------+
```

This split is deliberate: userspace owns orchestration, artifact capture, and
benchmark recording; the kernel owns transport mechanics and a deliberately
small image transform.

## Baseline Data Flow

```text
source frame
  -> userspace dequeue from vivid
  -> userspace records source sequence and timestamps
  -> userspace queues frame to vpipe OUTPUT
  -> kernel snapshots per-buffer controls
  -> kernel processes frame and fills CAPTURE buffer
  -> kernel publishes metadata row
  -> userspace dequeues CAPTURE buffer
  -> userspace correlates image result with metadata CSV
```

The metadata publication is not an afterthought. It is the mechanism that ties
transport timing, source sequence, ROI, and algorithm output to a specific
frame without embedding those details into the pixel payload.

## Current Format And Algorithm Scope

The baseline path is intentionally narrow:

- image format: single-plane `V4L2_PIX_FMT_GREY`
- algorithm: threshold over a GREY ROI
- output model: full-frame output with the ROI-mutated result

This keeps reasoning simple when a mismatch appears:

- transport mismatch points to queueing, format, or copy handling
- image mismatch points to the Tiny CV implementation or control snapshotting
- metadata mismatch points to sequencing or sideband publication

## Metadata Model

The metadata path uses a single global miscdevice with per-open reader cursors.

Implications:

- multiple readers can open the device independently
- each reader tracks its own cursor through the shared ring
- overruns are handled explicitly by catching readers up to the current window
- correlation is driven by `src_v4l2_sequence` propagated from userspace

That design keeps the metadata device simple while still allowing separate
inspection tools to consume the same publication stream.

## Timestamp Model

The current measurement design distinguishes at least two timing domains:

- V4L2 buffer timestamps carried in `struct v4l2_buffer.timestamp`
- userspace `CLOCK_MONOTONIC` timestamps recorded around `DQBUF`

For the current baseline, Phase 1 should capture both so the repo can compare:

- what the driver reports as buffer time
- what userspace experiences as dequeue latency

This is important because benchmark interpretation is weaker if only one of
those clocks is recorded.

Resolved on the validated guest dated 2026-05-07:

- on the Ubuntu 6.17.0-22 `aarch64` lima guest, vivid is a synthetic
  `platform:vivid-000` device, not a hardware sensor
- vivid exposes a `timestamp_source` control, but that is still synthetic guest
  behavior and not a meaningful hardware timestamp for this repository's
  latency claims
- treat `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC` plus userspace
  `CLOCK_MONOTONIC` sampling as the only useful timing signals on this guest

## DMA-BUF And dma-heap Notes

The deterministic fixture path uses `dma-heap` so the transport is explicit.

Current heap assumptions:

- `/dev/dma_heap/system` is the default allocator used by fixture validation
- userspace copies fixture bytes into the heap allocation before queueing
- kernel-side transport begins only after that explicit userspace copy

Current CPU mapping touch points:

- fixture load into heap allocation before queueing
- CAPTURE readback when userspace writes PGM output artifacts

Current sync note:

- there are no `dma_buf_begin_cpu_access()` / `dma_buf_end_cpu_access()` calls
  in the current tree
- the current implementation relies on direct CPU mappings for the heap-backed
  deterministic path, so there are no additional CPU-access fences to document
  yet

That means the fixture path is not a zero-copy claim. It is a deterministic and
observable transport path with one known userspace copy before queueing.

## Validation Model

Kernel-side image logic must always have a userspace comparison path.

The intended correctness loop is:

```text
fixture
  -> userspace reference output
  -> kernel output
  -> byte compare
  -> visual diff artifact
```

That is why the repository keeps:

- source fixtures under `tests/fixtures/`
- reference outputs as `bench/<fixture>.reference.pgm`
- kernel outputs as `bench/<fixture>.kernel.pgm`
- per-pixel diffs as `bench/<fixture>.diff.pgm`

## Media Graph Scope

The current implementation registers a `v4l2_device` but not a full
`media_device`.

Reason:

- direct `/dev/videoN` access is enough for the current transport experiments
- avoiding a graph layer keeps the bring-up smaller while the path is still
  being validated

If graph inspection becomes a hard requirement later, that should be reopened
as a separate design step after the current transport path is benchmarked.

Resolved for the current repo scope:

- `v4l2_device` plus direct `/dev/videoN` access is enough for the validated
  lima guest bring-up and benchmark paths
- `media_device` support remains optional future scope rather than a current
  acceptance requirement

## Phase 5 Sync Status

Phase 5 is `N/A` for the current target path.

Evidence used:

- checked V4L2 userspace buffer documentation does not describe
  `V4L2_BUF_FLAG_IN_FENCE`, `V4L2_BUF_FLAG_OUT_FENCE`, or a `fence_fd` field in
  `struct v4l2_buffer`
- checked target headers in the validation environment also do not expose those
  fence flags in `videodev2.h`; on the validated Ubuntu 25.10 lima guest
  (`6.17.0-22-generic`), both `/usr/include/linux/videodev2.h` and the matching
  kernel UAPI header expose `request_fd` but not fence flags or a `fence_fd`
  field
- generic `dma-buf` and `sync_file` docs still describe fence concepts at the
  subsystem level, but that is not enough to claim a usable V4L2 userspace API

Practical rule for this repository:

- do not claim a V4L2 out-fence userspace API without kernel-version-specific
  UAPI evidence for the actual deployment target
- keep `scripts/check.sh` aligned with that rule so validation fails as soon as
  the guest headers grow explicit fence UAPI and the repository needs to
  reopen Phase 5
- keep Phase 5 closed as `N/A` on the validated 6.17.0-22 lima guest until
  such evidence appears
