/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L

#include "vpipe-common.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-heap.h>
#include <linux/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static uint32_t crc32_table[256];
static bool crc32_ready;

int vpipe_xioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    /* V4L2 ioctls are routinely interrupted by signals during long captures. */
    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);

    return ret;
}

uint64_t vpipe_now_monotonic_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

FILE *vpipe_open_csv(const char *path, const char *header)
{
    FILE *fp;

    if (vpipe_ensure_parent_dir(path) < 0)
        return NULL;

    fp = fopen(path, "w");
    if (!fp)
        return NULL;

    if (header)
        fprintf(fp, "%s\n", header);

    return fp;
}

int vpipe_set_format(int fd,
                     enum v4l2_buf_type type,
                     uint32_t width,
                     uint32_t height,
                     uint32_t pixelformat)
{
    struct v4l2_format fmt;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = pixelformat;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (vpipe_xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
        return -1;

    return 0;
}

size_t vpipe_get_sizeimage(int fd, enum v4l2_buf_type type)
{
    struct v4l2_format fmt;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    if (vpipe_xioctl(fd, VIDIOC_G_FMT, &fmt) < 0)
        return 0;

    return fmt.fmt.pix.sizeimage;
}

int vpipe_request_mmap_buffers(int fd,
                               enum v4l2_buf_type type,
                               unsigned int count,
                               struct vpipe_mmap_buffer *buffers)
{
    struct v4l2_requestbuffers req;
    unsigned int i;

    memset(&req, 0, sizeof(req));
    req.count = count;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (vpipe_xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
        return -1;

    for (i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (vpipe_xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
            return -1;

        buffers[i].length = buf.length;
        buffers[i].dmabuf_fd = -1;
        /* The helpers only support the single-planar MMAP path used by vpipe.
         */
        buffers[i].addr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].addr == MAP_FAILED)
            return -1;
    }

    return req.count;
}

int vpipe_queue_mmap_buffer(int fd,
                            enum v4l2_buf_type type,
                            enum v4l2_memory memory,
                            unsigned int index,
                            int dmabuf_fd)
{
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    buf.type = type;
    buf.index = index;
    buf.memory = memory;
    if (memory == V4L2_MEMORY_DMABUF)
        buf.m.fd = dmabuf_fd;

    return vpipe_xioctl(fd, VIDIOC_QBUF, &buf);
}

void vpipe_unmap_buffers(struct vpipe_mmap_buffer *buffers, unsigned int count)
{
    unsigned int i;

    for (i = 0; i < count; i++) {
        if (buffers[i].addr && buffers[i].addr != MAP_FAILED)
            munmap(buffers[i].addr, buffers[i].length);
        if (buffers[i].dmabuf_fd >= 0)
            close(buffers[i].dmabuf_fd);
    }
}

static void vpipe_crc32_init(void)
{
    uint32_t i;

    if (crc32_ready)
        return;

    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        int bit;

        for (bit = 0; bit < 8; bit++) {
            if (c & 1U)
                c = 0xedb88320U ^ (c >> 1);
            else
                c >>= 1;
        }
        crc32_table[i] = c;
    }

    crc32_ready = true;
}

uint32_t vpipe_crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = ~0U;
    size_t i;

    vpipe_crc32_init();

    for (i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xffU] ^ (crc >> 8);

    return ~crc;
}

int vpipe_ensure_parent_dir(const char *path)
{
    char *copy;
    char *slash;
    char *p;

    copy = strdup(path);
    if (!copy)
        return -1;

    slash = strrchr(copy, '/');
    if (!slash) {
        free(copy);
        return 0;
    }

    *slash = '\0';
    if (copy[0] == '\0') {
        free(copy);
        return 0;
    }

    for (p = copy + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(copy, 0777) < 0 && errno != EEXIST) {
            free(copy);
            return -1;
        }
        *p = '/';
    }

    if (mkdir(copy, 0777) < 0 && errno != EEXIST) {
        free(copy);
        return -1;
    }

    free(copy);
    return 0;
}

int vpipe_write_pgm(const char *path,
                    const uint8_t *data,
                    unsigned int width,
                    unsigned int height)
{
    FILE *fp;
    size_t size = (size_t) width * height;

    if (vpipe_ensure_parent_dir(path) < 0)
        return -1;

    fp = fopen(path, "wb");
    if (!fp)
        return -1;

    fprintf(fp, "P5\n%u %u\n255\n", width, height);
    if (fwrite(data, 1, size, fp) != size) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

static int vpipe_skip_space(FILE *fp)
{
    int c;

    do {
        c = fgetc(fp);
        if (c == '#') {
            while (c != '\n' && c != EOF)
                c = fgetc(fp);
        }
    } while (isspace(c));

    return c;
}

/* Cap PGM dimensions; matches the kernel pixel-format clamp in vpipe-m2m.c. */
#define VPIPE_PGM_MAX_DIM 4096U

int vpipe_read_pgm(const char *path, struct vpipe_pgm_image *image)
{
    FILE *fp;
    int c;
    unsigned int maxval;
    size_t size;

    memset(image, 0, sizeof(*image));

    fp = fopen(path, "rb");
    if (!fp)
        return -1;

    if (fgetc(fp) != 'P' || fgetc(fp) != '5') {
        fclose(fp);
        return -1;
    }

    c = vpipe_skip_space(fp);
    ungetc(c, fp);
    if (fscanf(fp, "%u %u", &image->width, &image->height) != 2) {
        fclose(fp);
        return -1;
    }
    if (image->width == 0 || image->height == 0 ||
        image->width > VPIPE_PGM_MAX_DIM || image->height > VPIPE_PGM_MAX_DIM) {
        fclose(fp);
        return -1;
    }

    c = vpipe_skip_space(fp);
    ungetc(c, fp);
    if (fscanf(fp, "%u", &maxval) != 1 || maxval != 255) {
        fclose(fp);
        return -1;
    }

    fgetc(fp);
    size = (size_t) image->width * image->height;
    image->pixels = malloc(size);
    if (!image->pixels) {
        fclose(fp);
        return -1;
    }

    if (fread(image->pixels, 1, size, fp) != size) {
        free(image->pixels);
        image->pixels = NULL;
        fclose(fp);
        return -1;
    }

    image->size = size;
    fclose(fp);
    return 0;
}

void vpipe_free_pgm(struct vpipe_pgm_image *image)
{
    free(image->pixels);
    memset(image, 0, sizeof(*image));
}

int vpipe_write_diff_pgm(const char *path,
                         const struct vpipe_pgm_image *lhs,
                         const struct vpipe_pgm_image *rhs)
{
    struct vpipe_pgm_image diff = {0};
    size_t i;
    int ret;

    if (lhs->width != rhs->width || lhs->height != rhs->height)
        return -1;

    diff.width = lhs->width;
    diff.height = lhs->height;
    diff.size = lhs->size;
    diff.pixels = malloc(diff.size);
    if (!diff.pixels)
        return -1;

    for (i = 0; i < diff.size; i++) {
        /* Absolute per-pixel delta keeps mismatches visually obvious in PGM. */
        int delta = (int) lhs->pixels[i] - (int) rhs->pixels[i];

        if (delta < 0)
            delta = -delta;
        diff.pixels[i] = (uint8_t) delta;
    }

    ret = vpipe_write_pgm(path, diff.pixels, diff.width, diff.height);
    vpipe_free_pgm(&diff);
    return ret;
}

/*
 * Mirrors the kernel ROI clamp in vpipe-cv.c so userspace artifacts can be
 * compared byte-for-byte against the kernel's threshold output.
 */
int vpipe_threshold_reference(const struct vpipe_pgm_image *src,
                              struct vpipe_pgm_image *dst,
                              uint8_t threshold,
                              int roi_left,
                              int roi_top,
                              int roi_width,
                              int roi_height)
{
    unsigned int left, top, right, bottom;
    unsigned int x, y;

    memset(dst, 0, sizeof(*dst));
    dst->pixels = malloc(src->size);
    if (!dst->pixels)
        return -1;
    dst->width = src->width;
    dst->height = src->height;
    dst->size = src->size;
    memcpy(dst->pixels, src->pixels, src->size);

    if (roi_width <= 0 || roi_height <= 0) {
        left = 0;
        top = 0;
        right = src->width;
        bottom = src->height;
    } else {
        left = roi_left < 0                           ? 0
               : (unsigned int) roi_left > src->width ? src->width
                                                      : (unsigned int) roi_left;
        top = roi_top < 0                            ? 0
              : (unsigned int) roi_top > src->height ? src->height
                                                     : (unsigned int) roi_top;
        right = (unsigned int) roi_width > src->width - left
                    ? src->width
                    : left + (unsigned int) roi_width;
        bottom = (unsigned int) roi_height > src->height - top
                     ? src->height
                     : top + (unsigned int) roi_height;
    }

    for (y = top; y < bottom; y++) {
        for (x = left; x < right; x++) {
            size_t idx = (size_t) y * src->width + x;

            dst->pixels[idx] = src->pixels[idx] > threshold ? 255 : 0;
        }
    }

    return 0;
}
