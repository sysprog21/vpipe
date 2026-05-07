/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L

#include "vpipe-common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BUFFER_COUNT 4

int main(int argc, char **argv)
{
    const char *video = argc > 1 ? argv[1] : "/dev/video0";
    const char *csv = argc > 2 ? argv[2] : "bench/mmap.csv";
    unsigned int frames =
        argc > 3 ? (unsigned int) strtoul(argv[3], NULL, 0) : 600;
    struct vpipe_mmap_buffer buffers[BUFFER_COUNT] = {0};
    uint64_t queued_at[BUFFER_COUNT] = {0};
    FILE *fp;
    int fd;
    unsigned int i;

    fd = open(video, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (vpipe_set_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 640, 480,
                         V4L2_PIX_FMT_GREY) < 0) {
        perror("VIDIOC_S_FMT");
        return 1;
    }

    if (vpipe_request_mmap_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                   BUFFER_COUNT, buffers) < 0) {
        perror("VIDIOC_REQBUFS/MMAP");
        return 1;
    }

    fp = vpipe_open_csv(
        csv,
        "frame,buffer_index,enqueue_monotonic_ns,dequeue_monotonic_ns,v4l2_"
        "timestamp_sec,v4l2_timestamp_usec,sequence,bytesused");
    if (!fp) {
        perror("csv");
        return 1;
    }

    for (i = 0; i < BUFFER_COUNT; i++) {
        queued_at[i] = vpipe_now_monotonic_ns();
        if (vpipe_queue_mmap_buffer(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                    V4L2_MEMORY_MMAP, i, -1) < 0) {
            perror("VIDIOC_QBUF");
            return 1;
        }
    }

    {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (vpipe_xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
            perror("VIDIOC_STREAMON");
            return 1;
        }
    }

    for (i = 0; i < frames; i++) {
        struct v4l2_buffer buf;
        uint64_t dq_ns;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (vpipe_xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            perror("VIDIOC_DQBUF");
            return 1;
        }

        dq_ns = vpipe_now_monotonic_ns();
        fprintf(fp, "%u,%u,%" PRIu64 ",%" PRIu64 ",%ld,%ld,%u,%u\n", i,
                buf.index, queued_at[buf.index], dq_ns,
                (long) buf.timestamp.tv_sec, (long) buf.timestamp.tv_usec,
                buf.sequence, buf.bytesused);

        queued_at[buf.index] = vpipe_now_monotonic_ns();
        if (vpipe_xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            return 1;
        }
    }

    fclose(fp);
    vpipe_unmap_buffers(buffers, BUFFER_COUNT);
    close(fd);
    return 0;
}
