/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L

#include "vpipe-common.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned int g_failures;

#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static void test_crc32(void)
{
    /* Reference values: zlib/PNG CRC-32 with polynomial 0xedb88320. */
    EXPECT(vpipe_crc32("", 0) == 0u);
    EXPECT(vpipe_crc32("a", 1) == 0xe8b7be43u);
    EXPECT(vpipe_crc32("123456789", 9) == 0xcbf43926u);
    EXPECT(vpipe_crc32("The quick brown fox jumps over the lazy dog", 43) ==
           0x414fa339u);
}

static void test_ensure_parent_dir(char *root)
{
    char path[256];
    struct stat st;

    snprintf(path, sizeof(path), "%s/a/b/c/file.txt", root);
    EXPECT(vpipe_ensure_parent_dir(path) == 0);
    snprintf(path, sizeof(path), "%s/a/b/c", root);
    EXPECT(stat(path, &st) == 0 && S_ISDIR(st.st_mode));

    /* Idempotent: second invocation must succeed against existing tree. */
    snprintf(path, sizeof(path), "%s/a/b/c/file.txt", root);
    EXPECT(vpipe_ensure_parent_dir(path) == 0);

    /* Bare filename (no slash) is a no-op success. */
    EXPECT(vpipe_ensure_parent_dir("file.txt") == 0);
}

static void test_pgm_roundtrip(char *root)
{
    char path[256];
    struct vpipe_pgm_image img;
    uint8_t pixels[16];
    unsigned int i;

    for (i = 0; i < 16; i++)
        pixels[i] = (uint8_t) (i * 17);

    snprintf(path, sizeof(path), "%s/round.pgm", root);
    EXPECT(vpipe_write_pgm(path, pixels, 4, 4) == 0);

    EXPECT(vpipe_read_pgm(path, &img) == 0);
    EXPECT(img.width == 4);
    EXPECT(img.height == 4);
    EXPECT(img.size == 16);
    EXPECT(img.pixels && memcmp(img.pixels, pixels, 16) == 0);
    vpipe_free_pgm(&img);
}

static void test_pgm_rejects_bad_dims(char *root)
{
    char path[256];
    struct vpipe_pgm_image img;
    FILE *fp;

    snprintf(path, sizeof(path), "%s/zero.pgm", root);
    fp = fopen(path, "wb");
    EXPECT(fp != NULL);
    fprintf(fp, "P5\n0 0\n255\n");
    fclose(fp);
    EXPECT(vpipe_read_pgm(path, &img) < 0);

    snprintf(path, sizeof(path), "%s/huge.pgm", root);
    fp = fopen(path, "wb");
    EXPECT(fp != NULL);
    fprintf(fp, "P5\n100000 100000\n255\n");
    fclose(fp);
    EXPECT(vpipe_read_pgm(path, &img) < 0);
}

static void test_threshold_full(void)
{
    struct vpipe_pgm_image src = {0};
    struct vpipe_pgm_image dst = {0};
    uint8_t pixels[9] = {0, 50, 100, 127, 128, 200, 255, 10, 250};

    src.width = 3;
    src.height = 3;
    src.size = 9;
    src.pixels = pixels;

    EXPECT(vpipe_threshold_reference(&src, &dst, 127, 0, 0, 3, 3) == 0);
    EXPECT(dst.pixels[0] == 0);
    EXPECT(dst.pixels[1] == 0);
    EXPECT(dst.pixels[2] == 0);
    EXPECT(dst.pixels[3] == 0);   /* 127 not > 127 */
    EXPECT(dst.pixels[4] == 255); /* 128 > 127 */
    EXPECT(dst.pixels[5] == 255);
    EXPECT(dst.pixels[6] == 255);
    EXPECT(dst.pixels[7] == 0);
    EXPECT(dst.pixels[8] == 255);
    vpipe_free_pgm(&dst);
}

static void test_threshold_roi_subset(void)
{
    struct vpipe_pgm_image src = {0};
    struct vpipe_pgm_image dst = {0};
    uint8_t pixels[9] = {200, 200, 200, 200, 200, 200, 200, 200, 200};

    src.width = 3;
    src.height = 3;
    src.size = 9;
    src.pixels = pixels;

    /* ROI is the centre column only; outer pixels must remain untouched. */
    EXPECT(vpipe_threshold_reference(&src, &dst, 100, 1, 0, 1, 3) == 0);
    EXPECT(dst.pixels[0] == 200);
    EXPECT(dst.pixels[1] == 255);
    EXPECT(dst.pixels[2] == 200);
    EXPECT(dst.pixels[3] == 200);
    EXPECT(dst.pixels[4] == 255);
    EXPECT(dst.pixels[5] == 200);
    EXPECT(dst.pixels[6] == 200);
    EXPECT(dst.pixels[7] == 255);
    EXPECT(dst.pixels[8] == 200);
    vpipe_free_pgm(&dst);
}

static void test_threshold_roi_clamp(void)
{
    struct vpipe_pgm_image src = {0};
    struct vpipe_pgm_image dst = {0};
    uint8_t pixels[4] = {10, 200, 200, 10};

    src.width = 2;
    src.height = 2;
    src.size = 4;
    src.pixels = pixels;

    /* Negative origin and oversized extent must clamp to the frame bounds. */
    EXPECT(vpipe_threshold_reference(&src, &dst, 100, -5, -5, 1000, 1000) == 0);
    EXPECT(dst.pixels[0] == 0);
    EXPECT(dst.pixels[1] == 255);
    EXPECT(dst.pixels[2] == 255);
    EXPECT(dst.pixels[3] == 0);
    vpipe_free_pgm(&dst);
}

static void test_threshold_invalid_roi(void)
{
    struct vpipe_pgm_image src = {0};
    struct vpipe_pgm_image dst = {0};
    uint8_t pixels[4] = {10, 200, 200, 10};

    src.width = 2;
    src.height = 2;
    src.size = 4;
    src.pixels = pixels;

    /* width=0 means "use full frame" per the kernel contract. */
    EXPECT(vpipe_threshold_reference(&src, &dst, 100, 0, 0, 0, 0) == 0);
    EXPECT(dst.pixels[0] == 0);
    EXPECT(dst.pixels[1] == 255);
    EXPECT(dst.pixels[2] == 255);
    EXPECT(dst.pixels[3] == 0);
    vpipe_free_pgm(&dst);
}

int main(void)
{
    char tmpl[] = "/tmp/vpipe-unit-test.XXXXXX";
    char *root = mkdtemp(tmpl);
    char cmd[256];

    if (!root) {
        perror("mkdtemp");
        return 1;
    }

    test_crc32();
    test_ensure_parent_dir(root);
    test_pgm_roundtrip(root);
    test_pgm_rejects_bad_dims(root);
    test_threshold_full();
    test_threshold_roi_subset();
    test_threshold_roi_clamp();
    test_threshold_invalid_roi();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: failed to clean %s\n", root);

    if (g_failures) {
        fprintf(stderr, "%u test(s) FAILED\n", g_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
