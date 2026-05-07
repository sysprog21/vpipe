/* SPDX-License-Identifier: MIT */
#ifndef VPIPE_INTERNAL_H
#define VPIPE_INTERNAL_H

#include <linux/miscdevice.h>
#include <linux/types.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>

#include "vpipe.h"

#define VPIPE_NAME "vpipe"
#define VPIPE_DEFAULT_WIDTH 640U
#define VPIPE_DEFAULT_HEIGHT 480U
#define VPIPE_DEFAULT_THRESHOLD 127U

struct vpipe_roi {
    s32 left;
    s32 top;
    s32 width;
    s32 height;
};

struct vpipe_q_data {
    u32 width;
    u32 height;
    u32 bytesperline;
    u32 sizeimage;
    u32 pixelformat;
};

struct vpipe_job_desc {
    const u8 *src;
    u8 *dst;
    u32 width;
    u32 height;
    u32 src_stride;
    u32 dst_stride;
};

struct vpipe_cv_result {
    u32 algo_status;
    u64 value0;
    u64 value1;
    struct vpipe_roi roi;
};

struct vpipe_buffer {
    struct v4l2_m2m_buffer m2m_buf;
    u32 src_sequence;
    u32 algo_id;
    u32 threshold;
    struct vpipe_roi roi;
};

struct vpipe_dev {
    struct video_device vfd;
    struct v4l2_device v4l2_dev;
    struct v4l2_m2m_dev *m2m_dev;
    struct mutex dev_lock;
    atomic64_t sequence;
};

struct vpipe_ctx {
    struct v4l2_fh fh;
    struct vpipe_dev *dev;
    struct v4l2_ctrl_handler ctrl_handler;
    struct v4l2_m2m_ctx *m2m_ctx;
    struct vpipe_q_data out_q;
    struct vpipe_q_data cap_q;
    u32 src_sequence;
    u32 algo_id;
    u32 threshold;
    struct vpipe_roi roi;
};

int vpipe_meta_init(void);
void vpipe_meta_exit(void);
void vpipe_meta_publish(const struct vpipe_meta_entry *src);

int vpipe_cv_process(const struct vpipe_job_desc *job,
                     u32 algo_id,
                     u32 threshold,
                     const struct vpipe_roi *roi,
                     struct vpipe_cv_result *result);

#endif
