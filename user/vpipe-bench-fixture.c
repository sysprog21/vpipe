/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L

#include "vpipe-common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define BUFFER_COUNT 2

struct fixture_dmabuf {
    int fd;
    void *addr;
    size_t length;
};

static int dma_heap_alloc_fd(const char *heap_path, size_t len)
{
    struct dma_heap_allocation_data alloc = {
        .len = len,
        .fd_flags = O_CLOEXEC | O_RDWR,
    };
    int heap_fd;

    heap_fd = open(heap_path, O_RDWR);
    if (heap_fd < 0)
        return -1;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        close(heap_fd);
        return -1;
    }
    close(heap_fd);
    return alloc.fd;
}

static int queue_dmabuf(int fd,
                        unsigned int index,
                        int dmabuf_fd,
                        size_t bytesused,
                        size_t length)
{
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = index;
    buf.m.fd = dmabuf_fd;
    buf.bytesused = bytesused;
    buf.length = length;
    return vpipe_xioctl(fd, VIDIOC_QBUF, &buf);
}

static int set_ctrl(int fd, uint32_t id, int32_t value)
{
    struct v4l2_control ctrl;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = id;
    ctrl.value = value;
    return vpipe_xioctl(fd, VIDIOC_S_CTRL, &ctrl);
}

static int parse_algo(const char *value, uint32_t *algo_id)
{
    if (strcmp(value, "none") == 0) {
        *algo_id = VPIPE_ALGO_NONE;
        return 0;
    }
    if (strcmp(value, "threshold") == 0) {
        *algo_id = VPIPE_ALGO_THRESHOLD;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

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

int main(int argc, char **argv)
{
    const char *video = argc > 1 ? argv[1] : "/dev/video0";
    const char *heap_path = argc > 2 ? argv[2] : "/dev/dma_heap/system";
    const char *fixture = argc > 3 ? argv[3] : "tests/fixtures/ramp.pgm";
    const char *csv = argc > 4 ? argv[4] : "bench/fixture.csv";
    const char *algo_name = argc > 6 ? argv[6] : "none";
    const char *out_path = argc > 8 ? argv[8] : NULL;
    unsigned long frames_ul = 600;
    unsigned long threshold_ul = 127;
    struct vpipe_pgm_image src = {0};
    struct vpipe_mmap_buffer capture_bufs[BUFFER_COUNT] = {0};
    struct fixture_dmabuf output_bufs[BUFFER_COUNT];
    size_t out_sizeimage = 0;
    bool capture_streaming = false;
    bool output_streaming = false;
    uint32_t algo_id;
    FILE *fp = NULL;
    int fd = -1;
    int rc = 1;
    unsigned int i;

    for (i = 0; i < BUFFER_COUNT; i++) {
        output_bufs[i].fd = -1;
        output_bufs[i].addr = MAP_FAILED;
        output_bufs[i].length = 0;
        capture_bufs[i].dmabuf_fd = -1;
    }

    if (argc > 5 && parse_uint_range(argv[5], 1, UINT_MAX, &frames_ul) < 0) {
        fprintf(stderr, "invalid frames: %s\n", argv[5]);
        return 1;
    }
    if (argc > 7 && parse_uint_range(argv[7], 0, 255, &threshold_ul) < 0) {
        fprintf(stderr, "invalid threshold: %s\n", argv[7]);
        return 1;
    }
    if (parse_algo(algo_name, &algo_id) < 0) {
        fprintf(stderr, "unsupported algo: %s\n", algo_name);
        return 1;
    }

    if (vpipe_read_pgm(fixture, &src) < 0) {
        perror("read fixture");
        return 1;
    }

    fd = open(video, O_RDWR);
    if (fd < 0) {
        perror("open");
        goto cleanup;
    }

    if (vpipe_set_format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, src.width, src.height,
                         V4L2_PIX_FMT_GREY) < 0 ||
        vpipe_set_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, src.width, src.height,
                         V4L2_PIX_FMT_GREY) < 0) {
        perror("VIDIOC_S_FMT");
        goto cleanup;
    }

    out_sizeimage = vpipe_get_sizeimage(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
    if (out_sizeimage == 0) {
        fprintf(stderr, "VIDIOC_G_FMT OUTPUT failed\n");
        goto cleanup;
    }
    if (src.size > out_sizeimage) {
        fprintf(stderr, "fixture size %zu exceeds negotiated sizeimage %zu\n",
                src.size, out_sizeimage);
        goto cleanup;
    }

    {
        struct v4l2_requestbuffers req;

        memset(&req, 0, sizeof(req));
        req.count = BUFFER_COUNT;
        req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        req.memory = V4L2_MEMORY_DMABUF;
        if (vpipe_xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            perror("REQBUFS output");
            goto cleanup;
        }
    }

    if (vpipe_request_mmap_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                   BUFFER_COUNT, capture_bufs) < 0) {
        perror("REQBUFS capture");
        goto cleanup;
    }

    for (i = 0; i < BUFFER_COUNT; i++) {
        output_bufs[i].fd = dma_heap_alloc_fd(heap_path, out_sizeimage);
        if (output_bufs[i].fd < 0) {
            perror("DMA_HEAP_IOCTL_ALLOC");
            goto cleanup;
        }
        output_bufs[i].length = out_sizeimage;
        output_bufs[i].addr = mmap(NULL, out_sizeimage, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, output_bufs[i].fd, 0);
        if (output_bufs[i].addr == MAP_FAILED) {
            perror("mmap");
            goto cleanup;
        }
        /* One explicit userspace copy populates the heap-backed fixture path.
         */
        memcpy(output_bufs[i].addr, src.pixels, src.size);
    }

    if (set_ctrl(fd, VPIPE_CID_ALGO, (int32_t) algo_id) < 0 ||
        set_ctrl(fd, VPIPE_CID_THRESHOLD, (int32_t) threshold_ul) < 0) {
        perror("VIDIOC_S_CTRL");
        goto cleanup;
    }

    fp = vpipe_open_csv(csv,
                        "frame,output_index,enqueue_monotonic_ns,dequeue_"
                        "monotonic_ns,vpipe_sequence,bytesused,algo");
    if (!fp) {
        perror("csv");
        goto cleanup;
    }
    /* Line-buffered so a SIGINT mid-run still preserves complete rows. */
    setvbuf(fp, NULL, _IOLBF, 0);

    for (i = 0; i < BUFFER_COUNT; i++) {
        if (vpipe_queue_mmap_buffer(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                    V4L2_MEMORY_MMAP, i, -1) < 0) {
            perror("queue capture");
            goto cleanup;
        }
    }

    {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (vpipe_xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
            perror("STREAMON capture");
            goto cleanup;
        }
        capture_streaming = true;
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        if (vpipe_xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
            perror("STREAMON output");
            goto cleanup;
        }
        output_streaming = true;
    }

    for (i = 0; i < (unsigned int) frames_ul; i++) {
        struct v4l2_buffer cap_dq;
        struct v4l2_buffer out_dq;
        unsigned int index = i % BUFFER_COUNT;
        uint64_t enqueue_ns;
        uint64_t dequeue_ns;

        enqueue_ns = vpipe_now_monotonic_ns();
        if (queue_dmabuf(fd, index, output_bufs[index].fd, src.size,
                         output_bufs[index].length) < 0) {
            perror("QBUF output");
            goto cleanup;
        }

        memset(&cap_dq, 0, sizeof(cap_dq));
        cap_dq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        cap_dq.memory = V4L2_MEMORY_MMAP;
        if (vpipe_xioctl(fd, VIDIOC_DQBUF, &cap_dq) < 0) {
            perror("DQBUF capture");
            goto cleanup;
        }
        dequeue_ns = vpipe_now_monotonic_ns();
        if (cap_dq.index >= BUFFER_COUNT) {
            fprintf(stderr, "capture index out of range: %u\n", cap_dq.index);
            goto cleanup;
        }

        memset(&out_dq, 0, sizeof(out_dq));
        out_dq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        out_dq.memory = V4L2_MEMORY_DMABUF;
        if (vpipe_xioctl(fd, VIDIOC_DQBUF, &out_dq) < 0) {
            perror("DQBUF output");
            goto cleanup;
        }

        if (out_path && i == 0) {
            if (vpipe_write_pgm(out_path, capture_bufs[cap_dq.index].addr,
                                src.width, src.height) < 0) {
                perror("write pgm");
                goto cleanup;
            }
        }

        fprintf(fp, "%u,%u,%" PRIu64 ",%" PRIu64 ",%u,%u,%s\n", i, index,
                enqueue_ns, dequeue_ns, cap_dq.sequence, cap_dq.bytesused,
                algo_name);

        if (vpipe_xioctl(fd, VIDIOC_QBUF, &cap_dq) < 0) {
            perror("requeue capture");
            goto cleanup;
        }
    }

    rc = 0;

cleanup:
    if (fd >= 0) {
        if (output_streaming) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            (void) vpipe_xioctl(fd, VIDIOC_STREAMOFF, &type);
        }
        if (capture_streaming) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            (void) vpipe_xioctl(fd, VIDIOC_STREAMOFF, &type);
        }
    }
    if (fp)
        fclose(fp);
    for (i = 0; i < BUFFER_COUNT; i++) {
        if (output_bufs[i].addr != MAP_FAILED && output_bufs[i].length)
            munmap(output_bufs[i].addr, output_bufs[i].length);
        if (output_bufs[i].fd >= 0)
            close(output_bufs[i].fd);
    }
    vpipe_unmap_buffers(capture_bufs, BUFFER_COUNT);
    vpipe_free_pgm(&src);
    if (fd >= 0)
        close(fd);
    return rc;
}
