// SPDX-License-Identifier: GPL-2.0
/*
 * Entry points the host driver uses to give user mode a DRM device node: open/ioctl/close on a minor, plus
 * a few helpers. The Linux side creates the struct inode / struct file that drm_open() expects; ioctl
 * arguments are passed as kernel pointers (copy_from_user/copy_to_user are memcpy here).
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_drv.h>
#include <drm/drm_device.h>

static const struct file_operations kpi_dummy_fops;   /* drm_open_helper() reads filp->f_op->fop_flags */

struct kpi_drm_handle {
	struct inode inode;
	struct file file;
};

/* Returns an opaque handle, or NULL (minor not registered). */
void *kpi_drm_open(int minor)
{
	struct kpi_drm_handle *h = kzalloc(sizeof(*h), GFP_KERNEL);
	int r;

	if (!h)
		return NULL;
	h->inode.i_rdev = MKDEV(226, minor);
	h->inode.i_mode = S_IFCHR | 0666;
	h->inode.i_mapping = &h->inode.i_data;
	h->file.f_inode = &h->inode;
	h->file.f_mapping = &h->inode.i_data;
	h->file.f_mode = FMODE_READ | FMODE_WRITE;
	h->file.f_flags = O_RDWR;
	h->file.f_op = &kpi_dummy_fops;
	r = drm_open(&h->inode, &h->file);
	if (r) {
		pr_err("kpi2_drm: drm_open(minor %d) failed: %d\n", minor, r);
		kfree(h);
		return NULL;
	}
	h->file.f_op = ((struct drm_file *)h->file.private_data)->minor->dev->driver->fops;
	return h;
}

/* arg is a kernel buffer of _IOC_SIZE(cmd) bytes (in/out). */
long kpi_drm_ioctl(void *handle, unsigned int cmd, void *arg)
{
	struct kpi_drm_handle *h = handle;

	if (h->file.f_op && h->file.f_op->unlocked_ioctl)
		return h->file.f_op->unlocked_ioctl(&h->file, cmd, (unsigned long)arg);
	return drm_ioctl(&h->file, cmd, (unsigned long)arg);
}

void kpi_drm_close(void *handle)
{
	struct kpi_drm_handle *h = handle;

	drm_release(&h->inode, &h->file);
	kfree(h);
}
