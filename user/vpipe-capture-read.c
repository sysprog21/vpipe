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
#include <unistd.h>

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
    const char *csv = argc > 2 ? argv[2] : "bench/read.csv";
    unsigned long frames_ul = 600;
    size_t frame_size = 640U * 480U;
    uint8_t *buffer = NULL;
    FILE *fp = NULL;
    int fd = -1;
    int rc = 1;
    unsigned long i;

    if (argc > 3 && parse_uint_range(argv[3], 1, UINT_MAX, &frames_ul) < 0) {
        fprintf(stderr, "invalid frames: %s\n", argv[3]);
        return 1;
    }

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

    buffer = malloc(frame_size);
    if (!buffer) {
        perror("malloc");
        goto cleanup;
    }

    fp = vpipe_open_csv(csv,
                        "frame,synthetic_sequence,read_start_monotonic_ns,read_"
                        "end_monotonic_ns,bytesused,crc32");
    if (!fp) {
        perror("csv");
        goto cleanup;
    }
    setvbuf(fp, NULL, _IOLBF, 0);

    for (i = 0; i < frames_ul; i++) {
        uint64_t start_ns = vpipe_now_monotonic_ns();
        ssize_t got = read(fd, buffer, frame_size);
        uint64_t end_ns = vpipe_now_monotonic_ns();

        if (got < 0) {
            perror("read");
            goto cleanup;
        }

        fprintf(fp, "%lu,%lu,%" PRIu64 ",%" PRIu64 ",%zd,%u\n", i, i, start_ns,
                end_ns, got, vpipe_crc32(buffer, got));
    }

    rc = 0;

cleanup:
    if (fp)
        fclose(fp);
    free(buffer);
    if (fd >= 0)
        close(fd);
    return rc;
}
