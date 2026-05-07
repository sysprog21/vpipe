/* SPDX-License-Identifier: MIT */
#ifndef VPIPE_COMMON_H
#define VPIPE_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <linux/videodev2.h>

#include "../kmod/vpipe.h"

struct vpipe_mmap_buffer {
    void *addr;
    size_t length;
    int dmabuf_fd;
};

struct vpipe_pgm_image {
    unsigned int width;
    unsigned int height;
    uint8_t *pixels;
    size_t size;
};

int vpipe_xioctl(int fd, unsigned long request, void *arg);
uint64_t vpipe_now_monotonic_ns(void);
FILE *vpipe_open_csv(const char *path, const char *header);
int vpipe_set_format(int fd,
                     enum v4l2_buf_type type,
                     uint32_t width,
                     uint32_t height,
                     uint32_t pixelformat);
size_t vpipe_get_sizeimage(int fd, enum v4l2_buf_type type);
/* Request, query, and map a single-planar MMAP queue into userspace. */
int vpipe_request_mmap_buffers(int fd,
                               enum v4l2_buf_type type,
                               unsigned int count,
                               struct vpipe_mmap_buffer *buffers);
int vpipe_queue_mmap_buffer(int fd,
                            enum v4l2_buf_type type,
                            enum v4l2_memory memory,
                            unsigned int index,
                            int dmabuf_fd);
void vpipe_unmap_buffers(struct vpipe_mmap_buffer *buffers, unsigned int count);
uint32_t vpipe_crc32(const void *data, size_t len);
int vpipe_write_pgm(const char *path,
                    const uint8_t *data,
                    unsigned int width,
                    unsigned int height);
int vpipe_read_pgm(const char *path, struct vpipe_pgm_image *image);
void vpipe_free_pgm(struct vpipe_pgm_image *image);
int vpipe_write_diff_pgm(const char *path,
                         const struct vpipe_pgm_image *lhs,
                         const struct vpipe_pgm_image *rhs);
/*
 * Threshold only inside ROI, preserving pixels outside the processed region.
 * Returns 0 on success, -1 on allocation failure (in which case dst is zeroed).
 */
int vpipe_threshold_reference(const struct vpipe_pgm_image *src,
                              struct vpipe_pgm_image *dst,
                              uint8_t threshold,
                              int roi_left,
                              int roi_top,
                              int roi_width,
                              int roi_height);
int vpipe_ensure_parent_dir(const char *path);

#endif
