/* SPDX-License-Identifier: MIT */
#ifndef _UAPI_LINUX_VPIPE_H
#define _UAPI_LINUX_VPIPE_H

#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/videodev2.h>

#define VPIPE_META_VERSION 1
#define VPIPE_META_IOC_MAGIC 'p'

#define VPIPE_META_F_OVERRUN (1U << 0)
#define VPIPE_META_F_HW_TS (1U << 1)

#define VPIPE_ALGO_NONE 0
#define VPIPE_ALGO_THRESHOLD 1
#define VPIPE_ALGO_MOTION_SCORE 2
#define VPIPE_ALGO_EDGE_DENSITY 3
#define VPIPE_ALGO_DIFF_MAP 4

#define VPIPE_CID_BASE (V4L2_CID_USER_BASE + 0x1000)
#define VPIPE_CID_SRC_SEQUENCE (VPIPE_CID_BASE + 0)
#define VPIPE_CID_ALGO (VPIPE_CID_BASE + 1)
#define VPIPE_CID_ROI_LEFT (VPIPE_CID_BASE + 2)
#define VPIPE_CID_ROI_TOP (VPIPE_CID_BASE + 3)
#define VPIPE_CID_ROI_WIDTH (VPIPE_CID_BASE + 4)
#define VPIPE_CID_ROI_HEIGHT (VPIPE_CID_BASE + 5)
#define VPIPE_CID_THRESHOLD (VPIPE_CID_BASE + 6)

struct vpipe_meta_entry {
    __u16 version;
    __u16 entry_size;
    __u32 flags;

    __u64 seq;
    __u32 src_v4l2_sequence;
    __u32 buffer_index;

    __u64 timestamp_ns;
    __u32 bytesused;
    __u32 crc32;

    __u32 algo_id;
    __u32 algo_status;

    __s32 roi_left;
    __s32 roi_top;
    __s32 roi_width;
    __s32 roi_height;
    __u64 algo_value0;
    __u64 algo_value1;
};

struct vpipe_meta_info {
    __u16 version;
    __u16 entry_size;
    __u32 ring_entries;
    __u32 flags;
    __u32 reserved[4];
};

struct vpipe_meta_stats {
    __u64 published;
    __u64 dropped;
    __u32 reserved[4];
};

#define VPIPE_META_IOC_QUERY \
    _IOR(VPIPE_META_IOC_MAGIC, 0x00, struct vpipe_meta_info)
#define VPIPE_META_IOC_GET_STATS \
    _IOR(VPIPE_META_IOC_MAGIC, 0x01, struct vpipe_meta_stats)

#endif
