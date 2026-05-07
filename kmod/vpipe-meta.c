// SPDX-License-Identifier: MIT
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "vpipe-internal.h"

#define VPIPE_META_RING_ORDER 8
#define VPIPE_META_RING_SIZE (1U << VPIPE_META_RING_ORDER)
#define VPIPE_META_RING_MASK (VPIPE_META_RING_SIZE - 1)

struct vpipe_meta_dev {
    spinlock_t lock;
    wait_queue_head_t wait;
    struct miscdevice miscdev;
    struct vpipe_meta_entry ring[VPIPE_META_RING_SIZE];
    u64 head;
    atomic64_t published;
    atomic64_t dropped;
    bool dying;
};

struct vpipe_meta_file {
    u64 tail;
};

static struct vpipe_meta_dev g_meta;

void vpipe_meta_publish(const struct vpipe_meta_entry *src)
{
    struct vpipe_meta_entry *slot;
    unsigned long flags;
    u64 head;

    spin_lock_irqsave(&g_meta.lock, flags);
    head = g_meta.head++;
    slot = &g_meta.ring[head & VPIPE_META_RING_MASK];
    *slot = *src;
    /* The publisher owns versioning so readers always see normalized entries.
     */
    slot->version = VPIPE_META_VERSION;
    slot->entry_size = sizeof(*slot);
    atomic64_inc(&g_meta.published);
    if (g_meta.head > VPIPE_META_RING_SIZE)
        atomic64_set(&g_meta.dropped, g_meta.head - VPIPE_META_RING_SIZE);
    spin_unlock_irqrestore(&g_meta.lock, flags);

    wake_up_interruptible(&g_meta.wait);
}

static int vpipe_meta_open(struct inode *inode, struct file *file)
{
    struct vpipe_meta_file *ctx;

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    ctx->tail = READ_ONCE(g_meta.head);
    file->private_data = ctx;

    return nonseekable_open(inode, file);
}

static int vpipe_meta_release(struct inode *inode, struct file *file)
{
    kfree(file->private_data);
    return 0;
}

static ssize_t vpipe_meta_read(struct file *file,
                               char __user *buf,
                               size_t count,
                               loff_t *ppos)
{
    struct vpipe_meta_file *ctx = file->private_data;
    struct vpipe_meta_entry entry;
    unsigned long flags;
    u64 head;
    u64 oldest;
    bool overrun = false;

    if (count < sizeof(entry))
        return -EINVAL;

    if (!(file->f_flags & O_NONBLOCK)) {
        if (wait_event_interruptible(
                g_meta.wait,
                READ_ONCE(g_meta.dying) || READ_ONCE(g_meta.head) != ctx->tail))
            return -ERESTARTSYS;
    }

    spin_lock_irqsave(&g_meta.lock, flags);
    if (g_meta.dying) {
        spin_unlock_irqrestore(&g_meta.lock, flags);
        return 0;
    }

    head = g_meta.head;
    if (ctx->tail == head) {
        spin_unlock_irqrestore(&g_meta.lock, flags);
        return -EAGAIN;
    }

    oldest = head > VPIPE_META_RING_SIZE ? head - VPIPE_META_RING_SIZE : 0;
    if (ctx->tail < oldest) {
        /* Readers advance independently; catch this reader up on ring overrun.
         */
        ctx->tail = oldest;
        overrun = true;
    }

    entry = g_meta.ring[ctx->tail & VPIPE_META_RING_MASK];
    if (overrun)
        entry.flags |= VPIPE_META_F_OVERRUN;
    spin_unlock_irqrestore(&g_meta.lock, flags);

    /* Advance the per-reader cursor only after a successful copy so a bad
     * userspace pointer does not silently drop the entry from this reader.
     */
    if (copy_to_user(buf, &entry, sizeof(entry)))
        return -EFAULT;

    ctx->tail++;
    return sizeof(entry);
}

static long vpipe_meta_ioctl(struct file *file,
                             unsigned int cmd,
                             unsigned long arg)
{
    switch (cmd) {
    case VPIPE_META_IOC_QUERY: {
        struct vpipe_meta_info info = {0};

        info.version = VPIPE_META_VERSION;
        info.entry_size = sizeof(struct vpipe_meta_entry);
        info.ring_entries = VPIPE_META_RING_SIZE;
        return copy_to_user((void __user *) arg, &info, sizeof(info)) ? -EFAULT
                                                                      : 0;
    }
    case VPIPE_META_IOC_GET_STATS: {
        struct vpipe_meta_stats stats = {0};

        stats.published = atomic64_read(&g_meta.published);
        stats.dropped = atomic64_read(&g_meta.dropped);
        return copy_to_user((void __user *) arg, &stats, sizeof(stats))
                   ? -EFAULT
                   : 0;
    }
    default:
        return -ENOTTY;
    }
}

static __poll_t vpipe_meta_poll(struct file *file, poll_table *wait)
{
    struct vpipe_meta_file *ctx = file->private_data;

    poll_wait(file, &g_meta.wait, wait);
    if (READ_ONCE(g_meta.dying))
        return EPOLLHUP;
    if (READ_ONCE(g_meta.head) != ctx->tail)
        return EPOLLIN | EPOLLRDNORM;
    return 0;
}

static const struct file_operations vpipe_meta_fops = {
    .owner = THIS_MODULE,
    .open = vpipe_meta_open,
    .release = vpipe_meta_release,
    .read = vpipe_meta_read,
    .unlocked_ioctl = vpipe_meta_ioctl,
    .poll = vpipe_meta_poll,
    .llseek = noop_llseek,
};

int vpipe_meta_init(void)
{
    spin_lock_init(&g_meta.lock);
    init_waitqueue_head(&g_meta.wait);
    g_meta.miscdev.minor = MISC_DYNAMIC_MINOR;
    g_meta.miscdev.name = "vpipe-meta";
    g_meta.miscdev.fops = &vpipe_meta_fops;

    return misc_register(&g_meta.miscdev);
}

void vpipe_meta_exit(void)
{
    WRITE_ONCE(g_meta.dying, true);
    wake_up_interruptible(&g_meta.wait);
    misc_deregister(&g_meta.miscdev);
}
