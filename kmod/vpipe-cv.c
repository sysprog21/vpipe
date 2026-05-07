// SPDX-License-Identifier: MIT
#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/string.h>

#include "vpipe-internal.h"

/*
 * Clamp ROI to the frame. Additions are widened to s64 so the math is correct
 * for any s32 input — the helper does not depend on V4L2 control limits being
 * enforced upstream.
 */
static void vpipe_clamp_roi(const struct vpipe_job_desc *job,
                            const struct vpipe_roi *roi,
                            struct vpipe_roi *clamped)
{
    s32 fw = (s32) job->width;
    s32 fh = (s32) job->height;
    s32 left, top, right, bottom;

    if (!roi || roi->width <= 0 || roi->height <= 0) {
        clamped->left = 0;
        clamped->top = 0;
        clamped->width = fw;
        clamped->height = fh;
        return;
    }

    left = clamp_t(s32, roi->left, 0, fw);
    top = clamp_t(s32, roi->top, 0, fh);
    right = clamp_t(s64, (s64) left + roi->width, left, fw);
    bottom = clamp_t(s64, (s64) top + roi->height, top, fh);

    clamped->left = left;
    clamped->top = top;
    clamped->width = right - left;
    clamped->height = bottom - top;
}

static void vpipe_copy_frame(const struct vpipe_job_desc *job)
{
    u32 row;

    for (row = 0; row < job->height; row++) {
        memcpy(job->dst + (row * job->dst_stride),
               job->src + (row * job->src_stride), job->width);
    }
}

static void vpipe_threshold_frame(const struct vpipe_job_desc *job,
                                  u32 threshold,
                                  const struct vpipe_roi *roi,
                                  struct vpipe_cv_result *result)
{
    u64 white = 0;
    u64 black = 0;
    s32 x;
    s32 y;

    /*
     * Preserve the full frame and only rewrite pixels inside the validated ROI
     * so userspace can inspect both the untouched border and the transformed
     * region in one output image.
     */
    vpipe_copy_frame(job);

    for (y = roi->top; y < roi->top + roi->height; y++) {
        u8 *dst_row = job->dst + (y * job->dst_stride);
        const u8 *src_row = job->src + (y * job->src_stride);

        for (x = roi->left; x < roi->left + roi->width; x++) {
            u8 pixel = src_row[x] > threshold ? 255 : 0;

            dst_row[x] = pixel;
            if (pixel)
                white++;
            else
                black++;
        }
    }

    result->algo_status = 0;
    result->value0 = white;
    result->value1 = black;
}

int vpipe_cv_process(const struct vpipe_job_desc *job,
                     u32 algo_id,
                     u32 threshold,
                     const struct vpipe_roi *roi,
                     struct vpipe_cv_result *result)
{
    struct vpipe_roi clamped;

    memset(result, 0, sizeof(*result));
    vpipe_clamp_roi(job, roi, &clamped);
    result->roi = clamped;

    switch (algo_id) {
    case VPIPE_ALGO_NONE:
        vpipe_copy_frame(job);
        break;
    case VPIPE_ALGO_THRESHOLD:
        vpipe_threshold_frame(job, threshold, &clamped, result);
        break;
    default:
        return -EINVAL;
    }

    return 0;
}
