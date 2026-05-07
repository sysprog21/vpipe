/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L

#include "vpipe-common.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define BUFFER_COUNT 2

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

int main(int argc, char **argv)
{
    const char *video = argc > 1 ? argv[1] : "/dev/video0";
    const char *heap_path = argc > 2 ? argv[2] : "/dev/dma_heap/system";
    const char *fixture = argc > 3 ? argv[3] : "tests/fixtures/ramp.pgm";
    const char *out_path = argc > 4 ? argv[4] : "bench/ramp.kernel.pgm";
    struct vpipe_pgm_image src = {0};
    struct vpipe_mmap_buffer capture_bufs[BUFFER_COUNT] = {0};
    void *dmabuf_map;
    int dmabuf_fd;
    int fd;
    size_t out_sizeimage;

    if (vpipe_read_pgm(fixture, &src) < 0) {
        perror("read fixture");
        return 1;
    }

    fd = open(video, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (vpipe_set_format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, src.width, src.height,
                         V4L2_PIX_FMT_GREY) < 0 ||
        vpipe_set_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, src.width, src.height,
                         V4L2_PIX_FMT_GREY) < 0) {
        perror("VIDIOC_S_FMT");
        return 1;
    }
    out_sizeimage = vpipe_get_sizeimage(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
    if (out_sizeimage == 0) {
        fprintf(stderr, "VIDIOC_G_FMT OUTPUT failed\n");
        return 1;
    }
    if (src.size > out_sizeimage) {
        fprintf(stderr, "fixture size %zu exceeds negotiated sizeimage %zu\n",
                src.size, out_sizeimage);
        return 1;
    }

    {
        struct v4l2_requestbuffers req;

        memset(&req, 0, sizeof(req));
        req.count = BUFFER_COUNT;
        req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        req.memory = V4L2_MEMORY_DMABUF;
        if (vpipe_xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            perror("REQBUFS output");
            return 1;
        }
    }

    if (vpipe_request_mmap_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                   BUFFER_COUNT, capture_bufs) < 0) {
        perror("REQBUFS capture");
        return 1;
    }

    dmabuf_fd = dma_heap_alloc_fd(heap_path, out_sizeimage);
    if (dmabuf_fd < 0) {
        perror("DMA_HEAP_IOCTL_ALLOC");
        return 1;
    }

    dmabuf_map = mmap(NULL, out_sizeimage, PROT_READ | PROT_WRITE, MAP_SHARED,
                      dmabuf_fd, 0);
    if (dmabuf_map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    /* This is the one explicit userspace copy in the fixture-driven DMABUF
     * path. */
    memcpy(dmabuf_map, src.pixels, src.size);

    if (vpipe_xioctl(fd, VIDIOC_S_CTRL,
                     &(struct v4l2_control) {.id = VPIPE_CID_ALGO,
                                             .value = VPIPE_ALGO_THRESHOLD}) <
            0 ||
        vpipe_xioctl(fd, VIDIOC_S_CTRL,
                     &(struct v4l2_control) {.id = VPIPE_CID_THRESHOLD,
                                             .value = 127}) < 0) {
        perror("VIDIOC_S_CTRL");
        return 1;
    }

    if (vpipe_queue_mmap_buffer(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                V4L2_MEMORY_MMAP, 0, -1) < 0 ||
        queue_dmabuf(fd, 0, dmabuf_fd, src.size, out_sizeimage) < 0) {
        perror("queue");
        return 1;
    }

    {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (vpipe_xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
            perror("STREAMON capture");
            return 1;
        }
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        if (vpipe_xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
            perror("STREAMON output");
            return 1;
        }
    }

    {
        struct v4l2_buffer cap_dq;

        memset(&cap_dq, 0, sizeof(cap_dq));
        cap_dq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        cap_dq.memory = V4L2_MEMORY_MMAP;
        if (vpipe_xioctl(fd, VIDIOC_DQBUF, &cap_dq) < 0) {
            perror("DQBUF capture");
            return 1;
        }
        if (vpipe_write_pgm(out_path, capture_bufs[cap_dq.index].addr,
                            src.width, src.height) < 0) {
            perror("write pgm");
            return 1;
        }
    }

    munmap(dmabuf_map, out_sizeimage);
    close(dmabuf_fd);
    vpipe_free_pgm(&src);
    vpipe_unmap_buffers(capture_bufs, BUFFER_COUNT);
    close(fd);
    return 0;
}
