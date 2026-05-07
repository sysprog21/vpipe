/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L

#include "vpipe-common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/vpipe-meta";
    const char *csv = argc > 2 ? argv[2] : "bench/meta.csv";
    unsigned int frames =
        argc > 3 ? (unsigned int) strtoul(argv[3], NULL, 0) : 600;
    FILE *fp;
    int fd;
    unsigned int i;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    fp = vpipe_open_csv(csv,
                        "seq,src_v4l2_sequence,buffer_index,timestamp_ns,"
                        "bytesused,crc32,algo_id,algo_status,roi_left,roi_top,"
                        "roi_width,roi_height,algo_value0,algo_value1,flags");
    if (!fp) {
        perror("csv");
        return 1;
    }

    for (i = 0; i < frames; i++) {
        struct vpipe_meta_entry entry;
        ssize_t got = read(fd, &entry, sizeof(entry));

        if (got < 0) {
            perror("read");
            return 1;
        }
        if (got == 0)
            break;

        fprintf(fp,
                "%" PRIu64 ",%u,%u,%" PRIu64 ",%u,%u,%u,%u,%d,%d,%d,%d,%" PRIu64
                ",%" PRIu64 ",%u\n",
                (uint64_t) entry.seq, entry.src_v4l2_sequence,
                entry.buffer_index, (uint64_t) entry.timestamp_ns,
                entry.bytesused, entry.crc32, entry.algo_id, entry.algo_status,
                entry.roi_left, entry.roi_top, entry.roi_width,
                entry.roi_height, (uint64_t) entry.algo_value0,
                (uint64_t) entry.algo_value1, entry.flags);
    }

    fclose(fp);
    close(fd);
    return 0;
}
