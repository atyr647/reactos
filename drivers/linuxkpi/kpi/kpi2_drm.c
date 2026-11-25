// SPDX-License-Identifier: GPL-2.0
/*
 * Entry points the host driver uses to give user mode a DRM device node: open/ioctl/close on a minor, plus
 * a few helpers. The Linux side creates the struct inode / struct file that drm_open() expects. The ioctl
 * argument is a kernel copy that the host registers as the thread's user buffer; pointers nested inside it are
 * addresses in the calling process. copy_from_user/copy_to_user (kpi_core.c) accept only those two.
 */
#include <linux/module.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_drv.h>
#include <drm/drm_device.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/iosys-map.h>
#include <drm/drm_client.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <drm/drm_connector.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_resource.h>

static const struct file_operations kpi_dummy_fops = { .fop_flags = FOP_UNSIGNED_OFFSET };   /* drm_open_helper() checks this */

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
	/* the handle's reference; mappings take their own (kpi_drm_mmap), so it never reaches 0 through fput() */
	atomic_long_set(&h->file.f_count, 1);
	h->file.f_op = &kpi_dummy_fops;
	r = drm_open(&h->inode, &h->file);
	if (!r && !h->file.private_data)
		r = -ENODEV;		/* no drm_file: never hand out a handle that would be dereferenced as one */
	if (r) {
		if (r != -ENODEV)	/* no device on this minor: expected, libdrm opens every node to enumerate */
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

/* the struct file behind a DRM node handle (for kpi_drm_mmap) */
struct file *kpi_drm_handle_file(void *handle)
{
	return &((struct kpi_drm_handle *)handle)->file;
}
EXPORT_SYMBOL(kpi_drm_handle_file);

void kpi_drm_close(void *handle)
{
	struct kpi_drm_handle *h = handle;

	/* the host removes a handle's mappings before closing it: only the handle's own reference is left */
	WARN_ONCE(atomic_long_read(&h->file.f_count) != 1, "kpi2_drm: DRM file closed with %ld references\n",
		  atomic_long_read(&h->file.f_count));
	drm_release(&h->inode, &h->file);
	kfree(h);
}

/* IOCTL_LKPI_DRM_INFO: must match LKPI_DRM_INFO in lkpi_drm.h */
struct kpi_drm_info {
	int minor, primary_minor, render_minor, is_pci;
	u16 domain, vendor, device, subvendor, subdevice, reserved;
	u8 bus, dev, func, revision;
	char driver[32];
};

int kpi_drm_info(void *handle, struct kpi_drm_info *info)
{
	struct kpi_drm_handle *h = handle;
	struct drm_file *fp = h->file.private_data;
	struct drm_device *dev;

	BUILD_BUG_ON(sizeof(struct kpi_drm_info) != 64);
	if (!fp || !fp->minor || !fp->minor->dev)
		return -ENODEV;
	dev = fp->minor->dev;
	memset(info, 0, sizeof(*info));
	info->minor = fp->minor->index;
	info->primary_minor = dev->primary ? dev->primary->index : -1;
	info->render_minor = dev->render ? dev->render->index : -1;
	strscpy(info->driver, dev->driver->name, sizeof(info->driver));
	if (dev->dev && dev_is_pci(dev->dev)) {
		struct pci_dev *pdev = to_pci_dev(dev->dev);

		info->is_pci = 1;
		info->domain = pci_domain_nr(pdev->bus);
		info->bus = pdev->bus->number;
		info->dev = PCI_SLOT(pdev->devfn);
		info->func = PCI_FUNC(pdev->devfn);
		info->vendor = pdev->vendor;
		info->device = pdev->device;
		info->subvendor = pdev->subsystem_vendor;
		info->subdevice = pdev->subsystem_device;
		info->revision = pdev->revision;
	}
	return 0;
}
EXPORT_SYMBOL(kpi_drm_info);

/*
 * mmap of a DRM buffer into a user process.
 *
 * There are no Linux page tables: the driver's own mmap() runs on a vm_area_struct that is never installed, then its
 * fault handler is called for every page. The vmf_insert_pfn_prot() calls it makes are recorded here (page frame and
 * caching), and the host maps exactly those frames into the calling process with an MDL. Buffer placement, caching
 * and access checks therefore stay the driver's decisions (TTM for bochs and amdgpu, GEM helpers for others).
 *
 * Buffers move (a scanout buffer goes to VRAM, amdgpu evicts to GTT, ...). Before a move TTM calls
 * unmap_mapping_range() on the buffer's range; that queues a refresh which, once the move is done, faults every page
 * again and has the host rewrite the user PTEs in place (MmReplaceLockedPagesInUserSpace), so the address stays.
 * System-memory pages are counted while mapped (kpi_page_umap_get) and not reused before the mapping lets go: a write
 * that races with a move lands in memory nobody else owns and is lost, never in someone else's page (Linux would
 * block the writer instead). Page refcounts are left alone: high-order allocations count only their head page.
 */
struct kpi_vm {
	struct mm_struct mm;		/* vma.vm_mm points here: the recorder finds its state through it */
	struct vm_area_struct vma;
	struct list_head entry;		/* kpi_vms */
	struct address_space *mapping;	/* the device's (anon inode) mapping, as TTM names it in unmap_mapping_range() */
	unsigned long npages;
	unsigned long *pfn;		/* current frames, ~0UL = not inserted (yet) */
	unsigned long *rec;		/* where vmf_insert_pfn_prot() records: pfn, or a refresh's scratch array */
	unsigned int cache;		/* 0 write-back, 1 write-combined, 2 uncached */
	void *host;			/* the host's mapping, for kpi_drm_remap */
	struct work_struct refresh;
	struct mutex lock;		/* refresh vs unmap */
	bool dead;
};

#define KPI_VM_UNSET (~0UL)

static LIST_HEAD(kpi_vms);
static DEFINE_SPINLOCK(kpi_vms_lock);
static void (*kpi_drm_remap)(void *host, unsigned long *pfn, unsigned long npages, unsigned int cache);
extern void kpi_page_umap_get(unsigned long pfn);
extern void kpi_page_umap_put(unsigned long pfn);

void kpi_drm_set_remap(void (*fn)(void *host, unsigned long *pfn, unsigned long npages, unsigned int cache))
{
	kpi_drm_remap = fn;
}
EXPORT_SYMBOL(kpi_drm_set_remap);

vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, pgprot_t pgprot)
{
	struct kpi_vm *kv = container_of(vma->vm_mm, struct kpi_vm, mm);
	unsigned long idx = (addr - vma->vm_start) >> PAGE_SHIFT;
	unsigned long bits = pgprot_val(pgprot) & (_PAGE_PWT | _PAGE_PCD);

	if (vma != &kv->vma || addr < vma->vm_start || idx >= kv->npages)
		return VM_FAULT_SIGBUS;
	kv->rec[idx] = pfn;
	/* PAT index 1 (PWT) = WC; PCD set = UC/UC-; nothing = WB. One buffer, one caching mode. */
	kv->cache = bits == _PAGE_PWT ? 1 : (bits & _PAGE_PCD) ? 2 : 0;
	return VM_FAULT_NOPAGE;
}
EXPORT_SYMBOL(vmf_insert_pfn_prot);

vm_fault_t vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn)
{
	return vmf_insert_pfn_prot(vma, addr, pfn, vma->vm_page_prot);
}
EXPORT_SYMBOL(vmf_insert_pfn);

/* Fault every page into kv->rec[] (all set to KPI_VM_UNSET by the caller), then count the mappings */
static int kpi_vm_fault_all(struct kpi_vm *kv)
{
	unsigned long i;

	for (i = 0; i < kv->npages; i++) {
		if (kv->rec[i] == KPI_VM_UNSET) {
			struct vm_fault vmf = {
				.vma = &kv->vma,
				.address = kv->vma.vm_start + (i << PAGE_SHIFT),
				.pgoff = kv->vma.vm_pgoff + i,
				.gfp_mask = GFP_KERNEL,
				.flags = FAULT_FLAG_WRITE,
			};
			vm_fault_t f = kv->vma.vm_ops->fault(&vmf);

			if ((f & VM_FAULT_ERROR) || kv->rec[i] == KPI_VM_UNSET) {
				pr_err("kpi2_drm: mmap fault at page %lu of %lu: %x\n", i, kv->npages, f);
				return -EFAULT;		/* nothing counted yet */
			}
		}
	}
	for (i = 0; i < kv->npages; i++)
		kpi_page_umap_get(kv->rec[i]);
	return 0;
}

static void kpi_vm_put_pages(unsigned long *pfn, unsigned long npages)
{
	unsigned long i;

	for (i = 0; i < npages; i++)
		if (pfn[i] != KPI_VM_UNSET)
			kpi_page_umap_put(pfn[i]);
}

/* After a move: fault again, then the host points the user PTEs at the new frames, then the old pages go */
static void kpi_vm_refresh(struct work_struct *w)
{
	struct kpi_vm *kv = container_of(w, struct kpi_vm, refresh);
	unsigned long *fresh, i;

	mutex_lock(&kv->lock);
	if (kv->dead || !kpi_drm_remap)
		goto out;
	fresh = kvmalloc_array(kv->npages, sizeof(*fresh), GFP_KERNEL);
	if (!fresh)
		goto out;
	for (i = 0; i < kv->npages; i++)
		fresh[i] = KPI_VM_UNSET;
	kv->rec = fresh;
	if (kpi_vm_fault_all(kv)) {
		kv->rec = kv->pfn;
		kvfree(fresh);
		goto out;			/* the old frames stay mapped (and counted) */
	}
	kv->rec = kv->pfn;
	kpi_drm_remap(kv->host, fresh, kv->npages, kv->cache);
	kpi_vm_put_pages(kv->pfn, kv->npages);
	memcpy(kv->pfn, fresh, kv->npages * sizeof(*fresh));
	kvfree(fresh);
out:
	mutex_unlock(&kv->lock);
}

/* TTM (ttm_bo_unmap_virtual) and others: the buffer behind this range of the device file is about to move */
void unmap_mapping_range(struct address_space *mapping, loff_t const holebegin, loff_t const holelen, int even_cows)
{
	unsigned long first = holebegin >> PAGE_SHIFT;
	unsigned long last = holelen ? (holebegin + holelen - 1) >> PAGE_SHIFT : ULONG_MAX;
	struct kpi_vm *kv;
	unsigned long flags;

	spin_lock_irqsave(&kpi_vms_lock, flags);
	list_for_each_entry(kv, &kpi_vms, entry)
		if (kv->mapping == mapping && kv->vma.vm_pgoff <= last && kv->vma.vm_pgoff + kv->npages - 1 >= first)
			queue_work(system_long_wq, &kv->refresh);	/* runs once the mover released the buffer */
	spin_unlock_irqrestore(&kpi_vms_lock, flags);
}
EXPORT_SYMBOL(unmap_mapping_range);

/*
 * PASSIVE_LEVEL, in the context of the process the mapping is for. file: a DRM node's (kpi_drm_handle_file()) or a
 * dma-buf's (shared through an fd). offset: for a DRM node the fake mmap offset a driver ioctl returned
 * (DRM_IOCTL_MODE_MAP_DUMB, DRM_IOCTL_AMDGPU_GEM_MMAP, ...), for a dma-buf the offset into the buffer (usually 0).
 * size a multiple of PAGE_SIZE. pfn[] must stay valid until kpi_drm_munmap(); it is kept current across moves. host
 * is passed back to kpi_drm_remap.
 * Returns a cookie for kpi_drm_munmap(), or an ERR_PTR.
 */
void *kpi_drm_mmap(struct file *file, unsigned long long offset, unsigned long size, unsigned long *pfn, unsigned int *cache,
		   void *host)
{
	struct kpi_vm *kv;
	unsigned long i, flags;
	int ret;

	if (!size || (size & ~PAGE_MASK) || (offset & ~PAGE_MASK) || !file->f_op || !file->f_op->mmap)
		return ERR_PTR(-EINVAL);
	kv = kzalloc(sizeof(*kv), GFP_KERNEL);
	if (!kv)
		return ERR_PTR(-ENOMEM);
	kv->npages = size >> PAGE_SHIFT;
	kv->pfn = kv->rec = pfn;
	kv->host = host;
	mutex_init(&kv->lock);
	INIT_WORK(&kv->refresh, kpi_vm_refresh);
	INIT_LIST_HEAD(&kv->entry);
	for (i = 0; i < kv->npages; i++)
		pfn[i] = KPI_VM_UNSET;
	/* any page-aligned user-looking range: only offsets within it matter */
	kv->vma.vm_start = 0x100000000UL;
	kv->vma.vm_end = kv->vma.vm_start + size;
	kv->vma.vm_mm = &kv->mm;
	kv->vma.vm_pgoff = offset >> PAGE_SHIFT;
	/* the mapping's reference, as Linux's vma holds one: dma_buf_mmap() swaps in the dma-buf's file
	 * (vma_set_file()) when a driver maps an imported buffer, and drops this one */
	kv->vma.vm_file = get_file(file);
	vm_flags_init(&kv->vma, VM_READ | VM_WRITE | VM_SHARED | VM_MAYREAD | VM_MAYWRITE | VM_MAYSHARE);
	kv->vma.vm_page_prot = vm_get_page_prot(kv->vma.vm_flags);
	ret = file->f_op->mmap(file, &kv->vma);
	if (ret) {
		fput(kv->vma.vm_file);
		kfree(kv);
		return ERR_PTR(ret);
	}
	kv->mapping = file->f_mapping;	/* a DRM device's, also for its dma-bufs (drm_gem_dmabuf_export) */
	if (!kv->vma.vm_ops || !kv->vma.vm_ops->fault) {
		ret = -ENODEV;			/* remap_pfn_range()-style drivers: not supported */
		goto fail;
	}
	ret = kpi_vm_fault_all(kv);
	if (ret)
		goto fail;
	spin_lock_irqsave(&kpi_vms_lock, flags);
	list_add(&kv->entry, &kpi_vms);
	spin_unlock_irqrestore(&kpi_vms_lock, flags);
	*cache = kv->cache;
	pr_info_ratelimited("kpi2_drm: mmap offset %llx: %lu page(s), %s\n", offset, kv->npages,
			    kv->cache == 1 ? "write-combined" : kv->cache == 2 ? "uncached" : "cached");
	return kv;
fail:
	if (kv->vma.vm_ops && kv->vma.vm_ops->close)
		kv->vma.vm_ops->close(&kv->vma);
	fput(kv->vma.vm_file);
	kfree(kv);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(kpi_drm_mmap);

/* PASSIVE_LEVEL, after the host removed the user mapping */
void kpi_drm_munmap(void *cookie)
{
	struct kpi_vm *kv = cookie;
	unsigned long flags;

	spin_lock_irqsave(&kpi_vms_lock, flags);
	list_del_init(&kv->entry);
	spin_unlock_irqrestore(&kpi_vms_lock, flags);
	mutex_lock(&kv->lock);
	kv->dead = true;
	mutex_unlock(&kv->lock);
	cancel_work_sync(&kv->refresh);
	kpi_vm_put_pages(kv->pfn, kv->npages);
	if (kv->vma.vm_ops && kv->vma.vm_ops->close)
		kv->vma.vm_ops->close(&kv->vma);
	fput(kv->vma.vm_file);			/* as Linux's remove_vma() */
	kfree(kv);
}
EXPORT_SYMBOL(kpi_drm_munmap);

/*
 * The desktop on a Linux KMS driver (IOCTL_LKPI_DISPLAY_START, used by win32ss/drivers/miniport/lkpi): an in-kernel
 * DRM client, as Linux's own console has. It picks the first card with a connected monitor, takes each connector's
 * preferred mode, creates one XRGB8888 scanout buffer (pinned and mapped for good, so its address stays valid while the
 * driver moves other buffers), and commits. ReactOS's framebuf display driver then draws straight into that buffer.
 * When a monitor is plugged in, or a program that took the display over (a DRM master) exits, the mode is set again.
 */
struct kpi_display_info {	/* must match LKPI_DISPLAY_INFO in lkpi_drm.h */
	u64 vaddr;		/* kernel address of the scanout buffer */
	u32 width, height, pitch, bpp;
	u32 refresh, minor;
	char connector[32];
};

static struct drm_client_dev kpi_disp;
static struct drm_client_buffer *kpi_disp_buf;
static struct kpi_display_info kpi_disp_info;
static int kpi_disp_commit_result = -ENODEV;
static bool kpi_disp_on, kpi_disp_registered, kpi_disp_mapped;
static DEFINE_MUTEX(kpi_disp_lock);

static int kpi_disp_commit(struct drm_client_dev *client)
{
	struct drm_mode_set *ms;

	mutex_lock(&client->modeset_mutex);
	drm_client_for_each_modeset(ms, client)
		if (ms->mode)
			ms->fb = kpi_disp_buf->fb;
	mutex_unlock(&client->modeset_mutex);
	return drm_client_modeset_commit(client);
}

/* Where the desktop buffer is (TTM drivers): for the log; it must not move while the desktop draws into it */
static void kpi_disp_where(const char *when)
{
	struct ttm_buffer_object *bo;
	bool ttm;

	if (!kpi_disp_buf || !kpi_disp_buf->gem)
		return;
	/* only TTM drivers embed the GEM object in a TTM buffer object: VRAM-helper ones (bochs) and amdgpu */
	ttm = kpi_disp_buf->gem->dev->vram_mm || !strcmp(kpi_disp_buf->gem->dev->driver->name, "amdgpu");
	if (!ttm)
		return;
	bo = container_of(kpi_disp_buf->gem, struct ttm_buffer_object, base);
	if (bo->resource)
		pr_info("kpi2_drm: desktop buffer %s: memory type %u, page %llx, pinned %u\n", when,
			bo->resource->mem_type, (unsigned long long)bo->resource->start, bo->pin_count);
}

/* a DRM master went away: take the display back */
static int kpi_disp_restore(struct drm_client_dev *client)
{
	int r;

	if (!kpi_disp_buf)
		return 0;
	kpi_disp_where("before restore");
	r = kpi_disp_commit(client);
	kpi_disp_where("after restore");
	return r;
}

/* also called once by drm_client_register(): the first mode set */
static int kpi_disp_hotplug(struct drm_client_dev *client)
{
	int r;

	if (!kpi_disp_buf)
		return 0;
	/* modes no larger than the buffer (it was sized for the monitors present at start) */
	r = drm_client_modeset_probe(client, kpi_disp_buf->fb->width, kpi_disp_buf->fb->height);
	if (!r)
		r = kpi_disp_commit(client);
	kpi_disp_commit_result = r;
	if (r)
		pr_err("kpi2_drm: desktop mode set failed: %d\n", r);
	return r;
}

static void kpi_disp_unregister(struct drm_client_dev *client)
{
	pr_err("kpi2_drm: the desktop's DRM device went away\n");
	kpi_disp_on = false;
}

static const struct drm_client_funcs kpi_disp_funcs = {
	.owner = THIS_MODULE,
	.unregister = kpi_disp_unregister,
	.restore = kpi_disp_restore,
	.hotplug = kpi_disp_hotplug,
};

/* After the first successful mode set: map the (now pinned) scanout buffer, clear it, describe it in kpi_disp_info */
static int kpi_disp_map(void)
{
	struct kpi_display_info *info = &kpi_disp_info;
	struct iosys_map map;
	int r;

	r = drm_client_buffer_vmap(kpi_disp_buf, &map);
	if (r) {
		pr_err("kpi2_drm: desktop buffer cannot be mapped: %d\n", r);
		return r;
	}
	if (map.is_iomem)
		memset_io(map.vaddr_iomem, 0, (size_t)kpi_disp_buf->pitch * info->height);
	else
		memset(map.vaddr, 0, (size_t)kpi_disp_buf->pitch * info->height);
	info->vaddr = (u64)(uintptr_t)(map.is_iomem ? (void __force *)map.vaddr_iomem : map.vaddr);
	info->pitch = kpi_disp_buf->pitch;
	pr_info("kpi2_drm: desktop on minor %u (%s): %ux%u@%u, pitch %u, %s memory\n", info->minor, info->connector,
		info->width, info->height, info->refresh, info->pitch, map.is_iomem ? "device" : "system");
	kpi_disp_mapped = true;
	kpi_disp_where("mapped");
	return 0;
}

static int kpi_display_try(struct drm_device *dev, int minor)
{
	struct kpi_display_info *info = &kpi_disp_info;
	struct drm_mode_set *ms;
	u32 w = 0, h = 0, refresh = 0;
	const char *name = "?";
	int r;

	r = drm_client_init(dev, &kpi_disp, "lkpi-desktop", &kpi_disp_funcs);
	if (r)
		return r;
	r = drm_client_modeset_probe(&kpi_disp, 0, 0);
	if (r)
		goto release;
	mutex_lock(&kpi_disp.modeset_mutex);
	drm_client_for_each_modeset(ms, &kpi_disp) {
		if (!ms->mode)
			continue;
		w = max_t(u32, w, ms->x + ms->mode->hdisplay);
		h = max_t(u32, h, ms->y + ms->mode->vdisplay);
		if (!refresh) {
			refresh = drm_mode_vrefresh(ms->mode);
			if (ms->num_connectors)
				name = ms->connectors[0]->name;
		}
	}
	mutex_unlock(&kpi_disp.modeset_mutex);
	if (!w || !h) {
		r = -ENOENT;		/* no monitor connected */
		goto release;
	}
	kpi_disp_buf = drm_client_framebuffer_create(&kpi_disp, w, h, DRM_FORMAT_XRGB8888);
	if (IS_ERR(kpi_disp_buf)) {
		r = PTR_ERR(kpi_disp_buf);
		kpi_disp_buf = NULL;
		goto release;
	}
	memset(info, 0, sizeof(*info));
	info->width = w;
	info->height = h;
	info->bpp = 32;
	info->refresh = refresh;
	info->minor = minor;
	strscpy(info->connector, name, sizeof(info->connector));
	/*
	 * Set the mode first: the plane pins the buffer where the display scans out (VRAM for bochs and amdgpu); mapping
	 * then pins it again where it is. Mapped first, a VRAM-helper buffer would be pinned in system memory.
	 */
	kpi_disp_registered = true;		/* this device is the desktop's from now on, even if the mode set fails */
	drm_client_register(&kpi_disp);		/* runs kpi_disp_hotplug(): the mode set */
	if (kpi_disp_commit_result)
		return kpi_disp_commit_result;
	return kpi_disp_map();
release:
	drm_client_release(&kpi_disp);
	return r;
}

/* PASSIVE_LEVEL. Start the desktop display (once) and describe it. 0 or -errno. */
int kpi_display_start(struct kpi_display_info *info)
{
	int minor, r = -ENODEV;

	BUILD_BUG_ON(sizeof(struct kpi_display_info) != 64);
	mutex_lock(&kpi_disp_lock);
	if (kpi_disp_on) {
		*info = kpi_disp_info;
		mutex_unlock(&kpi_disp_lock);
		return 0;
	}
	if (kpi_disp_registered) {
		mutex_lock(&kpi_disp.dev->clientlist_mutex);	/* as DRM holds it around hotplug calls */
		r = kpi_disp_hotplug(&kpi_disp);	/* the mode set failed before: try again */
		mutex_unlock(&kpi_disp.dev->clientlist_mutex);
		if (!r && !kpi_disp_mapped)
			r = kpi_disp_map();	/* the mode set worked this time */
	} else {
		for (minor = 0; minor < 64 && r && !kpi_disp_registered; minor++) {
			struct kpi_drm_handle *h = kpi_drm_open(minor);
			struct drm_device *dev;

			if (!h)
				continue;
			dev = ((struct drm_file *)h->file.private_data)->minor->dev;
			drm_dev_get(dev);
			kpi_drm_close(h);
			if (drm_core_check_feature(dev, DRIVER_MODESET))
				r = kpi_display_try(dev, minor);
			drm_dev_put(dev);	/* the client keeps its own reference */
			if (r == -ENOENT)
				pr_info("kpi2_drm: minor %d: no monitor connected\n", minor);
		}
	}
	if (!r) {
		kpi_disp_on = true;
		*info = kpi_disp_info;
	}
	mutex_unlock(&kpi_disp_lock);
	return r;
}
EXPORT_SYMBOL(kpi_display_start);
