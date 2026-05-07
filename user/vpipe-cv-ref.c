/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L

#include "vpipe-common.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *input = argc > 1 ? argv[1] : "tests/fixtures/ramp.pgm";
    const char *output = argc > 2 ? argv[2] : "bench/ramp.reference.pgm";
    unsigned int threshold =
        argc > 3 ? (unsigned int) strtoul(argv[3], NULL, 0) : 127;
    struct vpipe_pgm_image src;
    struct vpipe_pgm_image dst;

    if (vpipe_read_pgm(input, &src) < 0) {
        perror("read pgm");
        return 1;
    }

    if (vpipe_threshold_reference(&src, &dst, (uint8_t) threshold, 0, 0,
                                  (int) src.width, (int) src.height) < 0) {
        perror("threshold reference");
        vpipe_free_pgm(&src);
        return 1;
    }
    if (vpipe_write_pgm(output, dst.pixels, dst.width, dst.height) < 0) {
        perror("write pgm");
        vpipe_free_pgm(&dst);
        vpipe_free_pgm(&src);
        return 1;
    }

    vpipe_free_pgm(&dst);
    vpipe_free_pgm(&src);
    return 0;
}
