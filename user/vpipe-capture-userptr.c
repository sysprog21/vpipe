/* SPDX-License-Identifier: MIT */

#define _POSIX_C_SOURCE 200809L
/* _DEFAULT_SOURCE exposes MAP_ANONYMOUS and ru_minflt on glibc. */
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include "vpipe-common.h"

#define BUFFER_COUNT 4

static int parse_uint_range(const char *s,
                            unsigned long min,
                            unsigned long max,
                            unsigned long *out)
{
    char *end;
    unsigned long v;

    if (!s || *s == '\0' || *s == '-')
        return -1;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || *end != '\0' || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

static long minor_faults_now(void)
{
    struct rusage ru;

    if (getrusage(RUSAGE_SELF, &ru) < 0)
        return -1;
    return ru.ru_minflt;
}

/* USERPTR ingress harness for the vpipe Phase 6 cost-decomposition row.
 *
 * vivid backs its capture queue with vb2_vmalloc, so the kernel pin path here
 * is mmap-and-fault, not DMA pinning. v4l2's __qbuf_userptr caches pinned-page
 * mapping per buffer slot: if the same userptr/length is QBUFed to the same
 * slot, vb2 reuses the existing pin and skips GUP entirely, so a fixed buffer
 * pool incurs zero per-frame faults. To measure the worst-case per-QBUF pin
 * cost (the row named userptr_minor_faults_per_frame), reset mode mmaps a fresh
 * anonymous region for each re-QBUF, forcing the cache to miss and the kernel
 * to re-pin from scratch. noreset mode preserves the fixed pool and
 * demonstrates the cached pin path.
 */
int main(int argc, char **argv)
{
    const char *video = argc > 1 ? argv[1] : "/dev/video0";
    const char *csv = argc > 2 ? argv[2] : "bench/userptr.csv";
    unsigned long frames_ul = 600;

    /* Default reset to expose the per-frame fault cost the row is named for;
     * "noreset" measures the steady-state buffer-reuse case.
     */
    bool reset_between_frames = true;
    void *buffers[BUFFER_COUNT] = {0};
    size_t lengths[BUFFER_COUNT] = {0};
    uint64_t queued_at[BUFFER_COUNT] = {0};
    long qbuf_minflt[BUFFER_COUNT] = {0};
    bool streaming = false;
    size_t sizeimage;
    size_t aligned_len;
    uint32_t userptr_length;
    long page_size;
    FILE *fp = NULL;
    int fd = -1;
    int rc = 1;
    unsigned long i;

    if (argc > 3 && parse_uint_range(argv[3], 1, UINT_MAX, &frames_ul) < 0) {
        fprintf(stderr, "invalid frames: %s\n", argv[3]);
        return 1;
    }

    if (argc > 4) {
        if (strcmp(argv[4], "reset") == 0)
            reset_between_frames = true;
        else if (strcmp(argv[4], "noreset") == 0)
            reset_between_frames = false;
        else {
            fprintf(stderr, "invalid mode '%s' (expected reset|noreset)\n",
                    argv[4]);
            return 1;
        }
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        page_size = 4096;

    fd = open(video, O_RDWR);
    if (fd < 0) {
        perror("open");
        goto cleanup;
    }

    if (vpipe_set_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 640, 480,
                         V4L2_PIX_FMT_GREY) < 0) {
        perror("VIDIOC_S_FMT");
        goto cleanup;
    }

    sizeimage = vpipe_get_sizeimage(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
    if (sizeimage == 0) {
        fprintf(stderr, "VIDIOC_G_FMT: zero sizeimage\n");
        goto cleanup;
    }

    /* Page-align so the kernel pin path operates on whole pages and reset
     * mode's mmap/munmap pair sees a clean per-frame VA reuse.
     */
    aligned_len =
        (sizeimage + (size_t) page_size - 1) & ~((size_t) page_size - 1);

    /* buf.length is __u32; reject formats whose page-aligned size would
     * truncate before any QBUF advertises a shorter buffer than was mapped.
     */
    if (aligned_len > UINT32_MAX) {
        fprintf(stderr, "aligned buffer length %zu exceeds uint32_t\n",
                aligned_len);
        goto cleanup;
    }
    userptr_length = (uint32_t) aligned_len;

    for (i = 0; i < BUFFER_COUNT; i++) {
        void *p = mmap(NULL, aligned_len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            perror("mmap anonymous");
            goto cleanup;
        }
        buffers[i] = p;
        lengths[i] = aligned_len;
    }

    {
        struct v4l2_requestbuffers req;

        memset(&req, 0, sizeof(req));
        req.count = BUFFER_COUNT;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_USERPTR;
        if (vpipe_xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            perror("VIDIOC_REQBUFS USERPTR");
            goto cleanup;
        }
        if (req.count < BUFFER_COUNT) {
            fprintf(stderr,
                    "USERPTR REQBUFS returned %u (expected %u); driver may "
                    "lack USERPTR support\n",
                    req.count, (unsigned int) BUFFER_COUNT);
            goto cleanup;
        }
    }

    fp = vpipe_open_csv(
        csv,
        "frame,buffer_index,enqueue_monotonic_ns,dequeue_monotonic_ns,v4l2_"
        "timestamp_sec,v4l2_timestamp_usec,sequence,bytesused,minor_faults_"
        "delta");
    if (!fp) {
        perror("csv");
        goto cleanup;
    }
    setvbuf(fp, NULL, _IOLBF, 0);

    for (i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;
        buf.index = i;
        buf.m.userptr = (unsigned long) buffers[i];
        buf.length = userptr_length;
        queued_at[i] = vpipe_now_monotonic_ns();
        /* Pre-streamon QBUFs are the warmup pin; their fault cost is
         * intentionally not in the CSV. */
        if (vpipe_xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            goto cleanup;
        }
        qbuf_minflt[i] = 0;
    }

    {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (vpipe_xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
            perror("VIDIOC_STREAMON");
            goto cleanup;
        }
        streaming = true;
    }

    for (i = 0; i < frames_ul; i++) {
        struct v4l2_buffer buf;
        uint64_t dq_ns;
        uint64_t enqueue_ns;
        long minflt_before_qbuf;
        long minflt_after_qbuf;
        long delta;
        unsigned int idx;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;
        if (vpipe_xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            perror("VIDIOC_DQBUF");
            goto cleanup;
        }
        if (buf.index >= BUFFER_COUNT) {
            fprintf(stderr, "userptr index out of range: %u\n", buf.index);
            goto cleanup;
        }
        idx = buf.index;
        if (buf.m.userptr != (unsigned long) buffers[idx]) {
            fprintf(stderr,
                    "DQBUF returned mismatched userptr %#lx for index %u "
                    "(expected %#lx)\n",
                    (unsigned long) buf.m.userptr, idx,
                    (unsigned long) buffers[idx]);
            goto cleanup;
        }

        dq_ns = vpipe_now_monotonic_ns();
        /* Pair this DQBUF with the QBUF that submitted it so the row's latency
         * and fault delta both describe the same buffer transit.
         */
        enqueue_ns = queued_at[idx];
        delta = qbuf_minflt[idx];

        fprintf(fp, "%lu,%u,%" PRIu64 ",%" PRIu64 ",%ld,%ld,%u,%u,%ld\n", i,
                idx, enqueue_ns, dq_ns, (long) buf.timestamp.tv_sec,
                (long) buf.timestamp.tv_usec, buf.sequence, buf.bytesused,
                delta);

        if (reset_between_frames) {
            /* Replace the slot with a fresh anonymous mapping so the next QBUF
             * advertises a userptr the vb2 cache has not seen, forcing GUP to
             * re-fault all pages and surfacing the cost in min_flt. Allocate
             * the new region BEFORE freeing the old so the kernel cannot reuse
             * the same VA — a same-address remap would silently hit
             * __qbuf_userptr's (userptr, length) cache and bypass GUP.
             */
            void *old = buffers[idx];
            size_t old_len = lengths[idx];
            void *fresh = mmap(NULL, aligned_len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

            if (fresh == MAP_FAILED) {
                perror("mmap anonymous (reset)");
                goto cleanup;
            }
            if (fresh == old) {
                fprintf(stderr, "reset mmap collided with prior buffer at %p\n",
                        old);
                munmap(fresh, aligned_len);
                goto cleanup;
            }
            buffers[idx] = fresh;
            lengths[idx] = aligned_len;
            if (munmap(old, old_len) < 0) {
                perror("munmap (reset)");
                goto cleanup;
            }
        }

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;
        buf.index = idx;
        buf.m.userptr = (unsigned long) buffers[idx];
        buf.length = userptr_length;

        /* Wrap only the QBUF ioctl so the delta attributes faults to the kernel
         * pin path, not to other buffers' work in this thread.
         */
        minflt_before_qbuf = minor_faults_now();
        queued_at[idx] = vpipe_now_monotonic_ns();
        if (vpipe_xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            goto cleanup;
        }
        minflt_after_qbuf = minor_faults_now();
        qbuf_minflt[idx] = (minflt_before_qbuf < 0 || minflt_after_qbuf < 0)
                               ? -1
                               : minflt_after_qbuf - minflt_before_qbuf;
    }

    rc = 0;

cleanup:
    if (fd >= 0 && streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void) vpipe_xioctl(fd, VIDIOC_STREAMOFF, &type);
    }
    if (fp)
        fclose(fp);
    for (i = 0; i < BUFFER_COUNT; i++) {
        if (buffers[i])
            munmap(buffers[i], lengths[i]);
    }
    if (fd >= 0)
        close(fd);
    return rc;
}
