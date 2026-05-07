/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L

#include "vpipe-common.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    struct vpipe_pgm_image lhs;
    struct vpipe_pgm_image rhs;

    if (argc != 4) {
        fprintf(stderr, "usage: %s lhs.pgm rhs.pgm diff.pgm\n", argv[0]);
        return 1;
    }

    if (vpipe_read_pgm(argv[1], &lhs) < 0 ||
        vpipe_read_pgm(argv[2], &rhs) < 0) {
        perror("vpipe_read_pgm");
        return 1;
    }

    if (vpipe_write_diff_pgm(argv[3], &lhs, &rhs) < 0) {
        perror("vpipe_write_diff_pgm");
        return 1;
    }

    vpipe_free_pgm(&lhs);
    vpipe_free_pgm(&rhs);
    return 0;
}
