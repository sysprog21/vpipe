// SPDX-License-Identifier: MIT
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/crc32.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>

#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-vmalloc.h>

#include "vpipe-internal.h"

static struct vpipe_dev g_vpipe_dev;
static atomic_t g_vpipe_instance = ATOMIC_INIT(0);

static struct vpipe_ctx *fh_to_ctx(struct v4l2_fh *fh)
{
    return container_of(fh, struct vpipe_ctx, fh);
}

static struct vpipe_q_data *vpipe_get_q_data(struct vpipe_ctx *ctx,
                                             enum v4l2_buf_type type)
{
    if (V4L2_TYPE_IS_OUTPUT(type))
        return &ctx->out_q;
    return &ctx->cap_q;
}

static struct vpipe_buffer *vb_to_vpipe_buffer(struct vb2_buffer *vb)
{
    return container_of(to_vb2_v4l2_buffer(vb), struct vpipe_buffer,
                        m2m_buf.vb);
}

static void vpipe_fill_pix_format(struct v4l2_pix_format *pix,
                                  u32 width,
                                  u32 height)
{
    /* Tiny fixtures are valid test inputs, so the lower bound stays at 1x1. */
    pix->width = clamp_t(u32, width, 1, 4096);
    pix->height = clamp_t(u32, height, 1, 4096);
    pix->pixelformat = V4L2_PIX_FMT_GREY;
    pix->field = V4L2_FIELD_NONE;
    pix->bytesperline = pix->width;
    pix->sizeimage = pix->width * pix->height;
    pix->colorspace = V4L2_COLORSPACE_SRGB;
    pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
    pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
    pix->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static int vpipe_querycap(struct file *file,
                          void *priv,
                          struct v4l2_capability *cap)
{
    strscpy(cap->driver, VPIPE_NAME, sizeof(cap->driver));
    strscpy(cap->card, "vpipe mem2mem", sizeof(cap->card));
    strscpy(cap->bus_info, "platform:vpipe", sizeof(cap->bus_info));
    cap->device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
    cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

    return 0;
}

static int vpipe_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
    if (f->index)
        return -EINVAL;

    f->pixelformat = V4L2_PIX_FMT_GREY;
    return 0;
}

static int vpipe_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
    struct vpipe_ctx *ctx = priv;
    struct vpipe_q_data *q_data = vpipe_get_q_data(ctx, f->type);

    vpipe_fill_pix_format(&f->fmt.pix, q_data->width, q_data->height);
    return 0;
}

static int vpipe_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
    if (f->fmt.pix.pixelformat != V4L2_PIX_FMT_GREY)
        f->fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;

    vpipe_fill_pix_format(&f->fmt.pix, f->fmt.pix.width, f->fmt.pix.height);
    return 0;
}

static int vpipe_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
    struct vpipe_ctx *ctx = priv;
    struct vpipe_q_data *q_data;
    struct vb2_queue *vq;

    vq = v4l2_m2m_get_vq(ctx->m2m_ctx, f->type);
    if (vb2_is_busy(vq))
        return -EBUSY;

    vpipe_try_fmt(file, priv, f);
    q_data = vpipe_get_q_data(ctx, f->type);
    q_data->width = f->fmt.pix.width;
    q_data->height = f->fmt.pix.height;
    q_data->bytesperline = f->fmt.pix.bytesperline;
    q_data->sizeimage = f->fmt.pix.sizeimage;
    q_data->pixelformat = f->fmt.pix.pixelformat;

    if (V4L2_TYPE_IS_OUTPUT(f->type)) {
        /* Threshold currently preserves geometry, so mirror OUTPUT to CAPTURE.
         */
        ctx->cap_q = *q_data;
    } else {
        ctx->out_q = *q_data;
    }

    return 0;
}

static int vpipe_queue_setup(struct vb2_queue *vq,
                             unsigned int *nbufs,
                             unsigned int *nplanes,
                             unsigned int sizes[],
                             struct device *alloc_devs[])
{
    struct vpipe_ctx *ctx = vb2_get_drv_priv(vq);
    struct vpipe_q_data *q_data = vpipe_get_q_data(ctx, vq->type);

    if (*nplanes) {
        if (sizes[0] < q_data->sizeimage)
            return -EINVAL;
    } else {
        *nplanes = 1;
        sizes[0] = q_data->sizeimage;
    }

    if (*nbufs < 2)
        *nbufs = 2;
    alloc_devs[0] = NULL;

    return 0;
}

static int vpipe_buf_prepare(struct vb2_buffer *vb)
{
    struct vpipe_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
    struct vpipe_q_data *q_data = vpipe_get_q_data(ctx, vb->vb2_queue->type);

    if (vb2_plane_size(vb, 0) < q_data->sizeimage)
        return -EINVAL;

    vb2_set_plane_payload(vb, 0, q_data->sizeimage);
    return 0;
}

static void vpipe_buf_queue(struct vb2_buffer *vb)
{
    struct vpipe_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
    struct vpipe_buffer *buf = vb_to_vpipe_buffer(vb);

    if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
        /* Snapshot per-buffer controls now so queued work is self-contained. */
        buf->src_sequence = ctx->src_sequence;
        buf->algo_id = ctx->algo_id;
        buf->threshold = ctx->threshold;
        buf->roi = ctx->roi;
    }

    v4l2_m2m_buf_queue(ctx->m2m_ctx, &buf->m2m_buf.vb);
}

static int vpipe_start_streaming(struct vb2_queue *q, unsigned int count)
{
    return 0;
}

static void vpipe_return_all_buffers(struct vpipe_ctx *ctx,
                                     enum v4l2_buf_type type)
{
    struct vb2_v4l2_buffer *vbuf;

    for (;;) {
        if (V4L2_TYPE_IS_OUTPUT(type))
            vbuf = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
        else
            vbuf = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);

        if (!vbuf)
            break;

        v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
    }
}

static void vpipe_stop_streaming(struct vb2_queue *q)
{
    struct vpipe_ctx *ctx = vb2_get_drv_priv(q);

    vpipe_return_all_buffers(ctx, q->type);
}

static const struct vb2_ops vpipe_qops = {
    .queue_setup = vpipe_queue_setup,
    .buf_prepare = vpipe_buf_prepare,
    .buf_queue = vpipe_buf_queue,
    .start_streaming = vpipe_start_streaming,
    .stop_streaming = vpipe_stop_streaming,
    .wait_prepare = vb2_ops_wait_prepare,
    .wait_finish = vb2_ops_wait_finish,
};

static int vpipe_queue_init(void *priv,
                            struct vb2_queue *src_vq,
                            struct vb2_queue *dst_vq)
{
    struct vpipe_ctx *ctx = priv;
    int ret;

    src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
    src_vq->drv_priv = ctx;
    src_vq->buf_struct_size = sizeof(struct vpipe_buffer);
    src_vq->ops = &vpipe_qops;
    src_vq->mem_ops = &vb2_vmalloc_memops;
    src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    src_vq->lock = &ctx->dev->dev_lock;
    src_vq->dev = ctx->dev->v4l2_dev.dev;
    ret = vb2_queue_init(src_vq);
    if (ret)
        return ret;

    dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
    dst_vq->drv_priv = ctx;
    dst_vq->buf_struct_size = sizeof(struct vpipe_buffer);
    dst_vq->ops = &vpipe_qops;
    dst_vq->mem_ops = &vb2_vmalloc_memops;
    dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    dst_vq->lock = &ctx->dev->dev_lock;
    dst_vq->dev = ctx->dev->v4l2_dev.dev;
    return vb2_queue_init(dst_vq);
}

static int vpipe_s_ctrl(struct v4l2_ctrl *ctrl)
{
    struct vpipe_ctx *ctx =
        container_of(ctrl->handler, struct vpipe_ctx, ctrl_handler);

    switch (ctrl->id) {
    case VPIPE_CID_SRC_SEQUENCE:
        ctx->src_sequence = ctrl->val;
        return 0;
    case VPIPE_CID_ALGO:
        ctx->algo_id = ctrl->val;
        return 0;
    case VPIPE_CID_ROI_LEFT:
        ctx->roi.left = ctrl->val;
        return 0;
    case VPIPE_CID_ROI_TOP:
        ctx->roi.top = ctrl->val;
        return 0;
    case VPIPE_CID_ROI_WIDTH:
        ctx->roi.width = ctrl->val;
        return 0;
    case VPIPE_CID_ROI_HEIGHT:
        ctx->roi.height = ctrl->val;
        return 0;
    case VPIPE_CID_THRESHOLD:
        ctx->threshold = ctrl->val;
        return 0;
    default:
        return -EINVAL;
    }
}

static const struct v4l2_ctrl_ops vpipe_ctrl_ops = {
    .s_ctrl = vpipe_s_ctrl,
};

static const char *const vpipe_algo_qmenu[] = {
    "none",
    "threshold",
};

static const struct v4l2_ctrl_config vpipe_ctrl_src_sequence = {
    .ops = &vpipe_ctrl_ops,
    .id = VPIPE_CID_SRC_SEQUENCE,
    .name = "Source V4L2 Sequence",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = INT_MAX,
    .step = 1,
    .def = 0,
};

static const struct v4l2_ctrl_config vpipe_ctrl_algo = {
    .ops = &vpipe_ctrl_ops,
    .id = VPIPE_CID_ALGO,
    .name = "Algorithm",
    .type = V4L2_CTRL_TYPE_MENU,
    .min = VPIPE_ALGO_NONE,
    .max = VPIPE_ALGO_THRESHOLD,
    .def = VPIPE_ALGO_NONE,
    .qmenu = vpipe_algo_qmenu,
};

static const struct v4l2_ctrl_config vpipe_ctrl_roi_left = {
    .ops = &vpipe_ctrl_ops,
    .id = VPIPE_CID_ROI_LEFT,
    .name = "ROI Left",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 4096,
    .step = 1,
    .def = 0,
};

static const struct v4l2_ctrl_config vpipe_ctrl_roi_top = {
    .ops = &vpipe_ctrl_ops,
    .id = VPIPE_CID_ROI_TOP,
    .name = "ROI Top",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 4096,
    .step = 1,
    .def = 0,
};

static const struct v4l2_ctrl_config vpipe_ctrl_roi_width = {
    .ops = &vpipe_ctrl_ops,
    .id = VPIPE_CID_ROI_WIDTH,
    .name = "ROI Width",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 4096,
    .step = 1,
    .def = VPIPE_DEFAULT_WIDTH,
};

static const struct v4l2_ctrl_config vpipe_ctrl_roi_height = {
    .ops = &vpipe_ctrl_ops,
    .id = VPIPE_CID_ROI_HEIGHT,
    .name = "ROI Height",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 4096,
    .step = 1,
    .def = VPIPE_DEFAULT_HEIGHT,
};

static const struct v4l2_ctrl_config vpipe_ctrl_threshold = {
    .ops = &vpipe_ctrl_ops,
    .id = VPIPE_CID_THRESHOLD,
    .name = "Threshold",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 255,
    .step = 1,
    .def = VPIPE_DEFAULT_THRESHOLD,
};

static void vpipe_device_run(void *priv)
{
    struct vpipe_ctx *ctx = priv;
    struct vb2_v4l2_buffer *src_vbuf;
    struct vb2_v4l2_buffer *dst_vbuf;
    struct vpipe_buffer *src_buf;
    struct vpipe_job_desc job;
    struct vpipe_cv_result cv_result;
    struct vpipe_meta_entry meta = {0};
    const u8 *src_addr;
    u8 *dst_addr;
    u64 seq;
    int ret;

    src_vbuf = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
    dst_vbuf = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
    if (!src_vbuf || !dst_vbuf)
        return;

    src_buf = container_of(src_vbuf, struct vpipe_buffer, m2m_buf.vb);
    src_addr = vb2_plane_vaddr(&src_vbuf->vb2_buf, 0);
    dst_addr = vb2_plane_vaddr(&dst_vbuf->vb2_buf, 0);

    if (!src_addr || !dst_addr) {
        src_vbuf = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
        dst_vbuf = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);
        v4l2_m2m_buf_done(src_vbuf, VB2_BUF_STATE_ERROR);
        v4l2_m2m_buf_done(dst_vbuf, VB2_BUF_STATE_ERROR);
        v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->m2m_ctx);
        return;
    }

    job.src = src_addr;
    job.dst = dst_addr;
    job.width = ctx->out_q.width;
    job.height = ctx->out_q.height;
    job.src_stride = ctx->out_q.bytesperline;
    job.dst_stride = ctx->cap_q.bytesperline;

    ret = vpipe_cv_process(&job, src_buf->algo_id, src_buf->threshold,
                           &src_buf->roi, &cv_result);

    /*
     * Remove the queued buffers only after processing. The m2m helpers keep the
     * current source/destination pairing stable between next_*_buf() and the
     * final buf_done()/job_finish() sequence.
     */
    src_vbuf = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
    dst_vbuf = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);

    if (ret) {
        v4l2_m2m_buf_done(src_vbuf, VB2_BUF_STATE_ERROR);
        v4l2_m2m_buf_done(dst_vbuf, VB2_BUF_STATE_ERROR);
        v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->m2m_ctx);
        return;
    }

    seq = atomic64_inc_return(&ctx->dev->sequence);
    vb2_set_plane_payload(&dst_vbuf->vb2_buf, 0, ctx->cap_q.sizeimage);
    dst_vbuf->field = V4L2_FIELD_NONE;
    dst_vbuf->vb2_buf.timestamp = ktime_get_ns();
    dst_vbuf->flags = src_vbuf->flags & V4L2_BUF_FLAG_TIMECODE;
    dst_vbuf->sequence = (u32) seq;

    /* Sideband metadata mirrors the completed CAPTURE buffer, not the source.
     */
    meta.seq = seq;
    meta.src_v4l2_sequence = src_buf->src_sequence;
    meta.buffer_index = dst_vbuf->vb2_buf.index;
    meta.timestamp_ns = dst_vbuf->vb2_buf.timestamp;
    meta.bytesused = ctx->cap_q.sizeimage;
    meta.crc32 = crc32_le(~0, dst_addr, ctx->cap_q.sizeimage);
    meta.algo_id = src_buf->algo_id;
    meta.algo_status = cv_result.algo_status;
    meta.roi_left = cv_result.roi.left;
    meta.roi_top = cv_result.roi.top;
    meta.roi_width = cv_result.roi.width;
    meta.roi_height = cv_result.roi.height;
    meta.algo_value0 = cv_result.value0;
    meta.algo_value1 = cv_result.value1;

    vpipe_meta_publish(&meta);
    v4l2_m2m_buf_done(src_vbuf, VB2_BUF_STATE_DONE);
    v4l2_m2m_buf_done(dst_vbuf, VB2_BUF_STATE_DONE);
    v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->m2m_ctx);
}

static void vpipe_job_abort(void *priv)
{
    struct vpipe_ctx *ctx = priv;

    v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->m2m_ctx);
}

static const struct v4l2_m2m_ops vpipe_m2m_ops = {
    .device_run = vpipe_device_run,
    .job_abort = vpipe_job_abort,
};

static int vpipe_open(struct file *file)
{
    struct vpipe_dev *dev = video_drvdata(file);
    struct vpipe_ctx *ctx;

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    ctx->dev = dev;
    ctx->src_sequence = 0;
    ctx->algo_id = VPIPE_ALGO_NONE;
    ctx->threshold = VPIPE_DEFAULT_THRESHOLD;
    ctx->roi.left = 0;
    ctx->roi.top = 0;
    ctx->roi.width = VPIPE_DEFAULT_WIDTH;
    ctx->roi.height = VPIPE_DEFAULT_HEIGHT;
    ctx->out_q.width = VPIPE_DEFAULT_WIDTH;
    ctx->out_q.height = VPIPE_DEFAULT_HEIGHT;
    ctx->out_q.bytesperline = VPIPE_DEFAULT_WIDTH;
    ctx->out_q.sizeimage = VPIPE_DEFAULT_WIDTH * VPIPE_DEFAULT_HEIGHT;
    ctx->out_q.pixelformat = V4L2_PIX_FMT_GREY;
    ctx->cap_q = ctx->out_q;

    v4l2_ctrl_handler_init(&ctx->ctrl_handler, 7);
    v4l2_ctrl_new_custom(&ctx->ctrl_handler, &vpipe_ctrl_src_sequence, NULL);
    v4l2_ctrl_new_custom(&ctx->ctrl_handler, &vpipe_ctrl_algo, NULL);
    v4l2_ctrl_new_custom(&ctx->ctrl_handler, &vpipe_ctrl_roi_left, NULL);
    v4l2_ctrl_new_custom(&ctx->ctrl_handler, &vpipe_ctrl_roi_top, NULL);
    v4l2_ctrl_new_custom(&ctx->ctrl_handler, &vpipe_ctrl_roi_width, NULL);
    v4l2_ctrl_new_custom(&ctx->ctrl_handler, &vpipe_ctrl_roi_height, NULL);
    v4l2_ctrl_new_custom(&ctx->ctrl_handler, &vpipe_ctrl_threshold, NULL);
    if (ctx->ctrl_handler.error) {
        int err = ctx->ctrl_handler.error;

        v4l2_ctrl_handler_free(&ctx->ctrl_handler);
        kfree(ctx);
        return err;
    }

    v4l2_fh_init(&ctx->fh, &dev->vfd);
    ctx->fh.ctrl_handler = &ctx->ctrl_handler;
    file->private_data = &ctx->fh;
    v4l2_fh_add(&ctx->fh);

    ctx->m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx, vpipe_queue_init);
    if (IS_ERR(ctx->m2m_ctx)) {
        int err = PTR_ERR(ctx->m2m_ctx);

        v4l2_fh_del(&ctx->fh);
        v4l2_fh_exit(&ctx->fh);
        v4l2_ctrl_handler_free(&ctx->ctrl_handler);
        kfree(ctx);
        return err;
    }

    ctx->fh.m2m_ctx = ctx->m2m_ctx;

    return 0;
}

static int vpipe_release(struct file *file)
{
    struct v4l2_fh *fh = file->private_data;
    struct vpipe_ctx *ctx = fh_to_ctx(fh);

    v4l2_m2m_ctx_release(ctx->m2m_ctx);
    v4l2_ctrl_handler_free(&ctx->ctrl_handler);
    v4l2_fh_del(&ctx->fh);
    v4l2_fh_exit(&ctx->fh);
    kfree(ctx);

    return 0;
}

static const struct v4l2_ioctl_ops vpipe_ioctl_ops = {
    .vidioc_querycap = vpipe_querycap,
    .vidioc_enum_fmt_vid_cap = vpipe_enum_fmt,
    .vidioc_enum_fmt_vid_out = vpipe_enum_fmt,
    .vidioc_g_fmt_vid_cap = vpipe_g_fmt,
    .vidioc_g_fmt_vid_out = vpipe_g_fmt,
    .vidioc_try_fmt_vid_cap = vpipe_try_fmt,
    .vidioc_try_fmt_vid_out = vpipe_try_fmt,
    .vidioc_s_fmt_vid_cap = vpipe_s_fmt,
    .vidioc_s_fmt_vid_out = vpipe_s_fmt,
    .vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
    .vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
    .vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
    .vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
    .vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
    .vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
    .vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
    .vidioc_streamon = v4l2_m2m_ioctl_streamon,
    .vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
    .vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
    .vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations vpipe_fops = {
    .owner = THIS_MODULE,
    .open = vpipe_open,
    .release = vpipe_release,
    .poll = v4l2_m2m_fop_poll,
    .unlocked_ioctl = video_ioctl2,
    .mmap = v4l2_m2m_fop_mmap,
};

static int __init vpipe_init(void)
{
    struct video_device *vfd = &g_vpipe_dev.vfd;
    int ret;

    ret = vpipe_meta_init();
    if (ret)
        return ret;

    mutex_init(&g_vpipe_dev.dev_lock);
    atomic64_set(&g_vpipe_dev.sequence, 0);

    v4l2_device_set_name(&g_vpipe_dev.v4l2_dev, VPIPE_NAME, &g_vpipe_instance);
    ret = v4l2_device_register(NULL, &g_vpipe_dev.v4l2_dev);
    if (ret)
        goto err_meta;

    g_vpipe_dev.m2m_dev = v4l2_m2m_init(&vpipe_m2m_ops);
    if (IS_ERR(g_vpipe_dev.m2m_dev)) {
        ret = PTR_ERR(g_vpipe_dev.m2m_dev);
        goto err_v4l2;
    }

    strscpy(vfd->name, VPIPE_NAME, sizeof(vfd->name));
    vfd->fops = &vpipe_fops;
    vfd->ioctl_ops = &vpipe_ioctl_ops;
    vfd->release = video_device_release_empty;
    vfd->lock = &g_vpipe_dev.dev_lock;
    vfd->v4l2_dev = &g_vpipe_dev.v4l2_dev;
    vfd->vfl_dir = VFL_DIR_M2M;
    vfd->device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
    video_set_drvdata(vfd, &g_vpipe_dev);

    ret = video_register_device(vfd, VFL_TYPE_VIDEO, -1);
    if (ret)
        goto err_m2m;

    pr_info("registered /dev/video%d and /dev/vpipe-meta\n", vfd->num);
    return 0;

err_m2m:
    v4l2_m2m_release(g_vpipe_dev.m2m_dev);
err_v4l2:
    v4l2_device_unregister(&g_vpipe_dev.v4l2_dev);
err_meta:
    vpipe_meta_exit();
    return ret;
}

static void __exit vpipe_exit(void)
{
    video_unregister_device(&g_vpipe_dev.vfd);
    v4l2_m2m_release(g_vpipe_dev.m2m_dev);
    v4l2_device_unregister(&g_vpipe_dev.v4l2_dev);
    vpipe_meta_exit();
}

module_init(vpipe_init);
module_exit(vpipe_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("vpipe mem2mem threshold pipeline");
MODULE_AUTHOR("OpenAI Codex");
