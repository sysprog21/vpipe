/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L

#include "vpipe-common.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *input = argc > 1 ? argv[1] : NULL;
    const char *output = argc > 2 ? argv[2] : "dump.pgm";
    unsigned int width =
        argc > 3 ? (unsigned int) strtoul(argv[3], NULL, 0) : 640;
    unsigned int height =
        argc > 4 ? (unsigned int) strtoul(argv[4], NULL, 0) : 480;
    FILE *fp;
    size_t size = (size_t) width * height;
    uint8_t *data;

    if (!input) {
        fprintf(stderr, "usage: %s raw-input output.pgm [width height]\n",
                argv[0]);
        return 1;
    }

    fp = fopen(input, "rb");
    if (!fp) {
        perror("fopen");
        return 1;
    }

    data = malloc(size);
    if (!data) {
        fclose(fp);
        return 1;
    }
    if (fread(data, 1, size, fp) != size) {
        perror("fread");
        fclose(fp);
        free(data);
        return 1;
    }
    fclose(fp);

    if (vpipe_write_pgm(output, data, width, height) < 0) {
        perror("write pgm");
        return 1;
    }

    free(data);
    return 0;
}
