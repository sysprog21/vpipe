/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L

#include "vpipe-common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BUFFER_COUNT 4

enum transport_mode {
    TRANSPORT_DMABUF = 0,
    TRANSPORT_MMAP,
};

static int export_dmabuf(int fd, enum v4l2_buf_type type, unsigned int index)
{
    struct v4l2_exportbuffer expbuf;

    memset(&expbuf, 0, sizeof(expbuf));
    expbuf.type = type;
    expbuf.index = index;
    expbuf.plane = 0;
    expbuf.flags = O_CLOEXEC;
    if (vpipe_xioctl(fd, VIDIOC_EXPBUF, &expbuf) < 0)
        return -1;

    return expbuf.fd;
}

static int set_ctrl(int fd, uint32_t id, int32_t value)
{
    struct v4l2_control ctrl;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = id;
    ctrl.value = value;
    return vpipe_xioctl(fd, VIDIOC_S_CTRL, &ctrl);
}

static int parse_transport(const char *value, enum transport_mode *mode)
{
    if (strcmp(value, "dmabuf") == 0) {
        *mode = TRANSPORT_DMABUF;
        return 0;
    }
    if (strcmp(value, "mmap") == 0) {
        *mode = TRANSPORT_MMAP;
        return 0;
    }
    errno = EINVAL;
    return -1;
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
    const char *vivid = argc > 1 ? argv[1] : "/dev/video0";
    const char *vpipe = argc > 2 ? argv[2] : "/dev/video1";
    const char *csv = argc > 3 ? argv[3] : "bench/m2m.csv";
    const char *transport_name = argc > 5 ? argv[5] : "dmabuf";
    const char *algo_name = argc > 6 ? argv[6] : "none";
    unsigned long frames_ul = 600;
    unsigned long threshold_ul = 127;
    struct vpipe_mmap_buffer vivid_bufs[BUFFER_COUNT] = {0};
    struct vpipe_mmap_buffer vpipe_out_bufs[BUFFER_COUNT] = {0};
    struct vpipe_mmap_buffer vpipe_bufs[BUFFER_COUNT] = {0};
    enum transport_mode transport_mode;
    uint32_t algo_id;
    size_t vivid_sizeimage = 0;
    size_t vpipe_out_sizeimage = 0;
    bool vivid_streaming = false;
    bool vpipe_cap_streaming = false;
    bool vpipe_out_streaming = false;
    FILE *fp = NULL;
    int vivid_fd = -1;
    int vpipe_fd = -1;
    int rc = 1;
    unsigned int i;

    for (i = 0; i < BUFFER_COUNT; i++) {
        vivid_bufs[i].dmabuf_fd = -1;
        vpipe_out_bufs[i].dmabuf_fd = -1;
        vpipe_bufs[i].dmabuf_fd = -1;
    }

    if (parse_transport(transport_name, &transport_mode) < 0) {
        fprintf(stderr, "unsupported transport: %s\n", transport_name);
        return 1;
    }
    if (parse_algo(algo_name, &algo_id) < 0) {
        fprintf(stderr, "unsupported algo: %s\n", algo_name);
        return 1;
    }
    if (argc > 4 && parse_uint_range(argv[4], 1, UINT_MAX, &frames_ul) < 0) {
        fprintf(stderr, "invalid frames: %s\n", argv[4]);
        return 1;
    }
    if (argc > 7 && parse_uint_range(argv[7], 0, 255, &threshold_ul) < 0) {
        fprintf(stderr, "invalid threshold: %s\n", argv[7]);
        return 1;
    }

    vivid_fd = open(vivid, O_RDWR);
    if (vivid_fd < 0) {
        perror("open vivid");
        goto cleanup;
    }
    vpipe_fd = open(vpipe, O_RDWR);
    if (vpipe_fd < 0) {
        perror("open vpipe");
        goto cleanup;
    }

    if (vpipe_set_format(vivid_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 640, 480,
                         V4L2_PIX_FMT_GREY) < 0 ||
        vpipe_set_format(vpipe_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 640, 480,
                         V4L2_PIX_FMT_GREY) < 0 ||
        vpipe_set_format(vpipe_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 640, 480,
                         V4L2_PIX_FMT_GREY) < 0) {
        perror("VIDIOC_S_FMT");
        goto cleanup;
    }

    vivid_sizeimage =
        vpipe_get_sizeimage(vivid_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
    vpipe_out_sizeimage =
        vpipe_get_sizeimage(vpipe_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
    if (vivid_sizeimage == 0 || vpipe_out_sizeimage == 0) {
        fprintf(stderr, "VIDIOC_G_FMT failed\n");
        goto cleanup;
    }

    if (vpipe_request_mmap_buffers(vivid_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                   BUFFER_COUNT, vivid_bufs) < 0 ||
        vpipe_request_mmap_buffers(vpipe_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                   BUFFER_COUNT, vpipe_bufs) < 0) {
        perror("mmap");
        goto cleanup;
    }

    {
        struct v4l2_requestbuffers req;

        memset(&req, 0, sizeof(req));
        req.count = BUFFER_COUNT;
        req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        req.memory = transport_mode == TRANSPORT_DMABUF ? V4L2_MEMORY_DMABUF
                                                        : V4L2_MEMORY_MMAP;
        if (vpipe_xioctl(vpipe_fd, VIDIOC_REQBUFS, &req) < 0) {
            perror("VIDIOC_REQBUFS OUTPUT");
            goto cleanup;
        }
    }

    if (transport_mode == TRANSPORT_MMAP &&
        vpipe_request_mmap_buffers(vpipe_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
                                   BUFFER_COUNT, vpipe_out_bufs) < 0) {
        perror("output mmap");
        goto cleanup;
    }

    fp = vpipe_open_csv(
        csv,
        "frame,transport,algo,vivid_sequence,vpipe_sequence,vivid_dq_monotonic_"
        "ns,vpipe_dq_monotonic_ns,vpipe_bytesused");
    if (!fp) {
        perror("csv");
        goto cleanup;
    }
    setvbuf(fp, NULL, _IOLBF, 0);

    for (i = 0; i < BUFFER_COUNT; i++) {
        if (transport_mode == TRANSPORT_DMABUF) {
            vivid_bufs[i].dmabuf_fd =
                export_dmabuf(vivid_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i);
            if (vivid_bufs[i].dmabuf_fd < 0) {
                perror("VIDIOC_EXPBUF");
                goto cleanup;
            }
        }
        if (vpipe_queue_mmap_buffer(vivid_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                    V4L2_MEMORY_MMAP, i, -1) < 0 ||
            vpipe_queue_mmap_buffer(vpipe_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                    V4L2_MEMORY_MMAP, i, -1) < 0) {
            perror("initial qbuf");
            goto cleanup;
        }
    }

    {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (vpipe_xioctl(vivid_fd, VIDIOC_STREAMON, &type) < 0) {
            perror("streamon vivid capture");
            goto cleanup;
        }
        vivid_streaming = true;
        if (vpipe_xioctl(vpipe_fd, VIDIOC_STREAMON, &type) < 0) {
            perror("streamon vpipe capture");
            goto cleanup;
        }
        vpipe_cap_streaming = true;
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        if (vpipe_xioctl(vpipe_fd, VIDIOC_STREAMON, &type) < 0) {
            perror("streamon vpipe output");
            goto cleanup;
        }
        vpipe_out_streaming = true;
    }

    for (i = 0; i < (unsigned int) frames_ul; i++) {
        struct v4l2_buffer vivid_dq;
        struct v4l2_buffer vpipe_dq;
        struct v4l2_buffer out_dq;
        struct v4l2_buffer qbuf;
        size_t copy_len;
        uint64_t vivid_ns;
        uint64_t vpipe_ns;

        memset(&vivid_dq, 0, sizeof(vivid_dq));
        vivid_dq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vivid_dq.memory = V4L2_MEMORY_MMAP;
        if (vpipe_xioctl(vivid_fd, VIDIOC_DQBUF, &vivid_dq) < 0) {
            perror("vivid dqbuf");
            goto cleanup;
        }
        vivid_ns = vpipe_now_monotonic_ns();
        if (vivid_dq.index >= BUFFER_COUNT) {
            fprintf(stderr, "vivid index out of range: %u\n", vivid_dq.index);
            goto cleanup;
        }

        if (set_ctrl(vpipe_fd, VPIPE_CID_SRC_SEQUENCE, vivid_dq.sequence) < 0 ||
            set_ctrl(vpipe_fd, VPIPE_CID_ALGO, (int32_t) algo_id) < 0 ||
            set_ctrl(vpipe_fd, VPIPE_CID_THRESHOLD, (int32_t) threshold_ul) <
                0) {
            perror("VIDIOC_S_CTRL");
            goto cleanup;
        }

        memset(&qbuf, 0, sizeof(qbuf));
        qbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        qbuf.index = vivid_dq.index;
        qbuf.bytesused = vivid_dq.bytesused;
        if (transport_mode == TRANSPORT_DMABUF) {
            qbuf.memory = V4L2_MEMORY_DMABUF;
            qbuf.m.fd = vivid_bufs[vivid_dq.index].dmabuf_fd;
            qbuf.length = vivid_bufs[vivid_dq.index].length;
        } else {
            qbuf.memory = V4L2_MEMORY_MMAP;
            copy_len = vivid_dq.bytesused;
            if (copy_len > vivid_bufs[vivid_dq.index].length)
                copy_len = vivid_bufs[vivid_dq.index].length;
            if (copy_len > vpipe_out_bufs[vivid_dq.index].length)
                copy_len = vpipe_out_bufs[vivid_dq.index].length;
            memcpy(vpipe_out_bufs[vivid_dq.index].addr,
                   vivid_bufs[vivid_dq.index].addr, copy_len);
            qbuf.bytesused = copy_len;
            qbuf.length = vpipe_out_bufs[vivid_dq.index].length;
        }
        if (vpipe_xioctl(vpipe_fd, VIDIOC_QBUF, &qbuf) < 0) {
            perror("vpipe qbuf output");
            goto cleanup;
        }

        memset(&vpipe_dq, 0, sizeof(vpipe_dq));
        vpipe_dq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vpipe_dq.memory = V4L2_MEMORY_MMAP;
        if (vpipe_xioctl(vpipe_fd, VIDIOC_DQBUF, &vpipe_dq) < 0) {
            perror("vpipe dqbuf capture");
            goto cleanup;
        }
        vpipe_ns = vpipe_now_monotonic_ns();

        /* Reclaim the OUTPUT slot so the next iteration can re-QBUF that index.
         */
        memset(&out_dq, 0, sizeof(out_dq));
        out_dq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        out_dq.memory = transport_mode == TRANSPORT_DMABUF ? V4L2_MEMORY_DMABUF
                                                           : V4L2_MEMORY_MMAP;
        if (vpipe_xioctl(vpipe_fd, VIDIOC_DQBUF, &out_dq) < 0) {
            perror("vpipe dqbuf output");
            goto cleanup;
        }

        fprintf(fp, "%u,%s,%s,%u,%u,%" PRIu64 ",%" PRIu64 ",%u\n", i,
                transport_name, algo_name, vivid_dq.sequence, vpipe_dq.sequence,
                vivid_ns, vpipe_ns, vpipe_dq.bytesused);

        if (vpipe_xioctl(vpipe_fd, VIDIOC_QBUF, &vpipe_dq) < 0 ||
            vpipe_xioctl(vivid_fd, VIDIOC_QBUF, &vivid_dq) < 0) {
            perror("requeue");
            goto cleanup;
        }
    }

    rc = 0;

cleanup:
    if (vpipe_fd >= 0) {
        if (vpipe_out_streaming) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            (void) vpipe_xioctl(vpipe_fd, VIDIOC_STREAMOFF, &type);
        }
        if (vpipe_cap_streaming) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            (void) vpipe_xioctl(vpipe_fd, VIDIOC_STREAMOFF, &type);
        }
    }
    if (vivid_fd >= 0 && vivid_streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void) vpipe_xioctl(vivid_fd, VIDIOC_STREAMOFF, &type);
    }
    if (fp)
        fclose(fp);
    vpipe_unmap_buffers(vivid_bufs, BUFFER_COUNT);
    vpipe_unmap_buffers(vpipe_out_bufs, BUFFER_COUNT);
    vpipe_unmap_buffers(vpipe_bufs, BUFFER_COUNT);
    if (vpipe_fd >= 0)
        close(vpipe_fd);
    if (vivid_fd >= 0)
        close(vivid_fd);
    return rc;
}
