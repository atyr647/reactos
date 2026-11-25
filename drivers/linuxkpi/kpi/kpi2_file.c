// SPDX-License-Identifier: GPL-2.0
/*
 * Files and file descriptors for the Linux modules: what dma-buf, sync_file, DRM syncobjs and shmem-backed GEM objects
 * need from the VFS, so that buffers and fences can be shared through fds as on Linux.
 *
 * A Linux fd handed to user mode is an NT handle, in the calling process, to a \Device\LkpiFile file object that holds
 * one reference to the struct file (drivers/linuxkpi/drmdev.c): get_unused_fd_flags() creates the handle, fd_install()
 * puts the file into it, fget() resolves a handle of the current process, and closing the last handle drops the
 * reference. NT handles can be duplicated into other processes (or inherited), which is how buffers and fences are
 * shared between processes.
 *
 * Also here: pseudo file systems (dma-buf's), anonymous files (sync files, syncobj fds), a page cache for shmem files
 * (GEM objects of shmem-helper drivers), and poll() for user mode (IOCTL_LKPI_FILE_POLL).
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fs_context.h>
#include <linux/pseudo_fs.h>
#include <linux/mount.h>
#include <linux/dcache.h>
#include <linux/slab.h>
#include <linux/shmem_fs.h>
#include <linux/pagemap.h>
#include <linux/poll.h>
#include <linux/xarray.h>
#include <linux/workqueue.h>
#include <linux/llist.h>
#include <linux/completion.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

extern int nt_fd_reserve(void);
extern void nt_fd_unreserve(int fd);
extern int nt_fd_install(int fd, struct file *file);
extern struct file *nt_fd_get(int fd);
extern unsigned char nt_raise_dispatch(void);
extern void nt_lower_irql(unsigned char old);

static struct inode kpi_anon_inode;		/* shared by all anonymous files, as Linux's anon_inode_inode */
static struct super_block kpi_anon_sb;

/* ---- inodes, dentries, files ------------------------------------------------------------------------------- */

static void kpi_inode_init(struct inode *inode, struct super_block *sb)
{
	inode->i_sb = sb;
	inode->i_mode = S_IRUSR | S_IWUSR;
	inode->i_mapping = &inode->i_data;
	inode->i_data.host = inode;
	xa_init_flags(&inode->i_data.i_pages, XA_FLAGS_LOCK_IRQ);
	mapping_set_gfp_mask(&inode->i_data, GFP_HIGHUSER_MOVABLE);
	spin_lock_init(&inode->i_lock);
	atomic_set(&inode->i_count, 1);
}

struct inode *alloc_anon_inode(struct super_block *s)
{
	struct inode *inode = kzalloc(sizeof(*inode), GFP_KERNEL);

	if (!inode)
		return ERR_PTR(-ENOMEM);
	kpi_inode_init(inode, s);
	return inode;
}
EXPORT_SYMBOL(alloc_anon_inode);

/* drop the page cache of a mapping (shmem files) */
static void kpi_mapping_free(struct address_space *mapping, pgoff_t first, pgoff_t last)
{
	struct folio *folio;
	unsigned long index;

	xa_for_each_range(&mapping->i_pages, index, folio, first, last) {
		xa_erase(&mapping->i_pages, index);
		folio_put(folio);		/* the cache's reference */
	}
}

void iput(struct inode *inode)
{
	if (!inode || inode == &kpi_anon_inode || !atomic_dec_and_test(&inode->i_count))
		return;
	kpi_mapping_free(&inode->i_data, 0, ULONG_MAX);
	kfree(inode);
}
EXPORT_SYMBOL(iput);

void dput(struct dentry *dentry)
{
	if (!dentry)
		return;
	if (dentry->d_op && dentry->d_op->d_release)
		dentry->d_op->d_release(dentry);	/* dma-buf frees the buffer here */
	kfree(dentry);
}
EXPORT_SYMBOL(dput);

static struct dentry *kpi_dentry_alloc(struct super_block *sb, struct inode *inode, const char *name)
{
	struct dentry *d = kzalloc(sizeof(*d), GFP_KERNEL);

	if (!d)
		return NULL;
	d->d_inode = inode;
	d->d_sb = sb;
	d->d_op = sb ? sb->s_d_op : NULL;
	d->d_name.name = (const unsigned char *)name;
	d->d_name.len = strlen(name);
	spin_lock_init(&d->d_lock);
	return d;
}

static struct file *kpi_file_alloc(struct inode *inode, struct dentry *dentry, struct vfsmount *mnt, int flags,
				   const struct file_operations *fops, void *priv)
{
	struct file *f = kzalloc(sizeof(*f), GFP_KERNEL);

	if (!f)
		return ERR_PTR(-ENOMEM);
	atomic_long_set(&f->f_count, 1);
	spin_lock_init(&f->f_lock);
	mutex_init(&f->f_pos_lock);
	f->f_inode = inode;
	f->f_mapping = inode->i_mapping;
	f->f_path.dentry = dentry;
	f->f_path.mnt = mnt;
	f->f_flags = flags & (O_ACCMODE | O_NONBLOCK | O_CLOEXEC);
	f->f_mode = OPEN_FMODE(flags) | FMODE_OPENED;
	f->f_op = fops;
	f->private_data = priv;
	return f;
}

struct file *alloc_file_pseudo(struct inode *inode, struct vfsmount *mnt, const char *name, int flags,
			       const struct file_operations *fops)
{
	struct dentry *d = kpi_dentry_alloc(mnt ? mnt->mnt_sb : NULL, inode, name);
	struct file *f;

	if (!d)
		return ERR_PTR(-ENOMEM);
	f = kpi_file_alloc(inode, d, mnt, flags, fops, NULL);
	if (IS_ERR(f))
		kfree(d);		/* the caller keeps the inode */
	return f;
}
EXPORT_SYMBOL(alloc_file_pseudo);

struct file *anon_inode_getfile(const char *name, const struct file_operations *fops, void *priv, int flags)
{
	struct dentry *d = kpi_dentry_alloc(&kpi_anon_sb, &kpi_anon_inode, name);
	struct file *f;

	if (!d)
		return ERR_PTR(-ENOMEM);
	/* no module references: the host never unloads modules */
	f = kpi_file_alloc(&kpi_anon_inode, d, NULL, flags, fops, priv);
	if (IS_ERR(f))
		kfree(d);
	return f;
}
EXPORT_SYMBOL(anon_inode_getfile);

/* the last reference is gone: release, then the dentry (dma-buf frees its buffer there), then the inode */
static void kpi_file_free(struct file *f)
{
	struct dentry *d = f->f_path.dentry;
	struct inode *inode = f->f_inode;

	if (f->f_op && f->f_op->release)
		f->f_op->release(inode, f);
	dput(d);
	iput(inode);
	kfree(f);
}

/* as Linux's delayed_fput: a file whose last reference goes in atomic context is released from a worker */
static LLIST_HEAD(kpi_fput_list);

static void kpi_fput_work(struct work_struct *w)
{
	struct llist_node *n = llist_del_all(&kpi_fput_list);
	struct file *f, *t;

	llist_for_each_entry_safe(f, t, n, f_llist)
		kpi_file_free(f);
}
static DECLARE_WORK(kpi_fput_worker, kpi_fput_work);

void fput(struct file *f)
{
	unsigned char irql;

	if (!f || !atomic_long_dec_and_test(&f->f_count))
		return;
	irql = nt_raise_dispatch();
	nt_lower_irql(irql);
	if (irql >= 2 /* DISPATCH_LEVEL */ || irqs_disabled()) {
		if (llist_add(&f->f_llist, &kpi_fput_list))
			schedule_work(&kpi_fput_worker);
		return;
	}
	kpi_file_free(f);
}
EXPORT_SYMBOL(fput);

void inode_set_bytes(struct inode *inode, loff_t bytes)
{
	inode->i_blocks = bytes >> 9;
	inode->i_bytes = bytes & 511;
}
EXPORT_SYMBOL(inode_set_bytes);

/* a dentry's name as d_path() would print it (dma-buf's d_dname): at the end of buffer */
char *dynamic_dname(char *buffer, int buflen, const char *fmt, ...)
{
	va_list args;
	char temp[64];
	int sz;

	va_start(args, fmt);
	sz = vsnprintf(temp, sizeof(temp), fmt, args) + 1;
	va_end(args);
	if (sz > sizeof(temp) || sz > buflen)
		return ERR_PTR(-ENAMETOOLONG);
	buffer += buflen - sz;
	return memcpy(buffer, temp, sz);
}
EXPORT_SYMBOL(dynamic_dname);

/* mmap of a dma-buf by an importer: the mapping now refers to the dma-buf's file */
void vma_set_file(struct vm_area_struct *vma, struct file *file)
{
	get_file(file);
	swap(vma->vm_file, file);
	if (file)
		fput(file);
}
EXPORT_SYMBOL(vma_set_file);

/* a NUL-terminated string of at most n bytes from user memory (DMA_BUF_SET_NAME) */
char *strndup_user(const char __user *s, long n)
{
	char *p = kmalloc(n, GFP_KERNEL);
	long i;

	if (!p)
		return ERR_PTR(-ENOMEM);
	for (i = 0; i < n; i++) {
		if (copy_from_user(&p[i], s + i, 1)) {
			kfree(p);
			return ERR_PTR(-EFAULT);
		}
		if (!p[i])
			return p;
	}
	kfree(p);
	return ERR_PTR(-EINVAL);
}
EXPORT_SYMBOL(strndup_user);

/* ---- pseudo file systems (dma-buf's) --------------------------------------------------------------------- */

struct pseudo_fs_context *init_pseudo(struct fs_context *fc, unsigned long magic)
{
	struct pseudo_fs_context *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);

	if (ctx) {
		ctx->magic = magic;
		fc->fs_private = ctx;
	}
	return ctx;
}
EXPORT_SYMBOL(init_pseudo);

/* a mount is just a super block carrying the file system's dentry operations */
struct vfsmount *kern_mount(struct file_system_type *type)
{
	struct fs_context fc = { .fs_type = type, .purpose = FS_CONTEXT_FOR_MOUNT };
	struct pseudo_fs_context *ctx;
	struct super_block *sb;
	struct vfsmount *mnt;
	int r;

	r = type->init_fs_context ? type->init_fs_context(&fc) : -ENOSYS;
	if (r)
		return ERR_PTR(r);
	ctx = fc.fs_private;
	sb = kzalloc(sizeof(*sb), GFP_KERNEL);
	mnt = kzalloc(sizeof(*mnt), GFP_KERNEL);
	if (!sb || !mnt) {
		kfree(sb);
		kfree(mnt);
		kfree(ctx);
		return ERR_PTR(-ENOMEM);
	}
	sb->s_type = type;
	if (ctx) {
		sb->s_magic = ctx->magic;
		sb->s_op = ctx->ops;
		sb->s_d_op = ctx->dops;
	}
	mnt->mnt_sb = sb;
	kfree(ctx);
	return mnt;
}
EXPORT_SYMBOL(kern_mount);

void kern_unmount(struct vfsmount *mnt)
{
	/* the host never unloads: nothing to do */
}
EXPORT_SYMBOL(kern_unmount);

void kill_anon_super(struct super_block *sb)
{
}
EXPORT_SYMBOL(kill_anon_super);

/* ---- file descriptors: NT handles to \Device\LkpiFile objects (drivers/linuxkpi/drmdev.c) ---------------- */

int get_unused_fd_flags(unsigned flags)
{
	return nt_fd_reserve();		/* a handle value in the calling process, or -EMFILE */
}
EXPORT_SYMBOL(get_unused_fd_flags);

void put_unused_fd(unsigned int fd)
{
	nt_fd_unreserve(fd);
}
EXPORT_SYMBOL(put_unused_fd);

/* takes over the caller's reference, as on Linux */
void fd_install(unsigned int fd, struct file *file)
{
	if (nt_fd_install(fd, file)) {
		pr_err("kpi2_file: fd_install(%u): not a reserved fd\n", fd);
		fput(file);
	}
}
EXPORT_SYMBOL(fd_install);

struct file *fget(unsigned int fd)
{
	return nt_fd_get(fd);		/* with a reference, or NULL */
}
EXPORT_SYMBOL(fget);

struct fd fdget(unsigned int fd)
{
	struct file *f = nt_fd_get(fd);

	return f ? CLONED_FD(f) : EMPTY_FD;
}
EXPORT_SYMBOL(fdget);

/* ---- shmem files: a page cache in the mapping's xarray (GEM objects of shmem-helper drivers) ------------- */

struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags)
{
	struct inode *inode = alloc_anon_inode(&kpi_anon_sb);
	struct file *f;

	if (IS_ERR(inode))
		return ERR_CAST(inode);
	inode->i_size = size;
	f = kpi_file_alloc(inode, NULL, NULL, O_RDWR, NULL, NULL);
	if (IS_ERR(f))
		iput(inode);
	return f;
}
EXPORT_SYMBOL(shmem_file_setup);

/* the page at index (allocated zeroed on first use), with a reference for the caller */
struct folio *shmem_read_folio_gfp(struct address_space *mapping, pgoff_t index, gfp_t gfp)
{
	struct folio *folio, *old;
	struct page *page;

	folio = xa_load(&mapping->i_pages, index);
	if (folio) {
		folio_get(folio);
		return folio;
	}
	page = alloc_page((gfp & ~__GFP_HIGHMEM) | __GFP_ZERO);
	if (!page)
		return ERR_PTR(-ENOMEM);
	folio = page_folio(page);		/* its reference is the cache's */
	old = xa_cmpxchg(&mapping->i_pages, index, NULL, folio, GFP_KERNEL);
	if (old) {				/* lost a race: use the other page */
		folio_put(folio);
		if (xa_is_err(old))
			return ERR_PTR(xa_err(old));
		folio = old;
	}
	folio_get(folio);
	return folio;
}
EXPORT_SYMBOL(shmem_read_folio_gfp);

struct page *shmem_read_mapping_page_gfp(struct address_space *mapping, pgoff_t index, gfp_t gfp)
{
	struct folio *folio = shmem_read_folio_gfp(mapping, index, gfp);

	return IS_ERR(folio) ? ERR_CAST(folio) : folio_file_page(folio, index);
}
EXPORT_SYMBOL(shmem_read_mapping_page_gfp);

void shmem_truncate_range(struct inode *inode, loff_t start, loff_t end)
{
	kpi_mapping_free(inode->i_mapping, start >> PAGE_SHIFT, end == (loff_t)-1 ? ULONG_MAX : end >> PAGE_SHIFT);
}
EXPORT_SYMBOL(shmem_truncate_range);

unsigned long invalidate_mapping_pages(struct address_space *mapping, pgoff_t start, pgoff_t end)
{
	return 0;			/* no reclaim: pages stay until truncated or freed with the file */
}
EXPORT_SYMBOL(invalidate_mapping_pages);

/* ---- for the host: ioctl, poll and references on files reached through LkpiFile handles ---------------- */

void kpi_file_get(struct file *f)
{
	get_file(f);
}
EXPORT_SYMBOL(kpi_file_get);

void kpi_file_put(struct file *f)
{
	fput(f);
}
EXPORT_SYMBOL(kpi_file_put);

/* arg: a kernel copy of _IOC_SIZE(cmd) bytes (see kpi_drm_ioctl) */
long kpi_file_ioctl(struct file *f, unsigned int cmd, void *arg)
{
	if (!f->f_op || !f->f_op->unlocked_ioctl)
		return -ENOTTY;
	return f->f_op->unlocked_ioctl(f, cmd, (unsigned long)arg);
}
EXPORT_SYMBOL(kpi_file_ioctl);

struct kpi_poll {
	poll_table pt;
	struct completion woken;
	int n;
	struct {
		wait_queue_entry_t wait;
		wait_queue_head_t *head;
	} e[4];
};

static int kpi_poll_wake(wait_queue_entry_t *wait, unsigned int mode, int sync, void *key)
{
	struct kpi_poll *p = wait->private;

	complete(&p->woken);
	return 1;
}

static void kpi_poll_queue(struct file *f, wait_queue_head_t *head, poll_table *pt)
{
	struct kpi_poll *p = container_of(pt, struct kpi_poll, pt);

	if (p->n >= ARRAY_SIZE(p->e))
		return;
	init_waitqueue_func_entry(&p->e[p->n].wait, kpi_poll_wake);
	p->e[p->n].wait.private = p;
	p->e[p->n].head = head;
	add_wait_queue(head, &p->e[p->n].wait);
	p->n++;
}

/*
 * PASSIVE_LEVEL. Wait until the file is ready for any of events (EPOLLIN, ...) or timeout_ms passes (< 0: forever;
 * 0: just look). Returns the ready events (EPOLLERR/EPOLLHUP always count), 0 on timeout.
 */
unsigned int kpi_file_poll(struct file *f, unsigned int events, int timeout_ms)
{
	struct kpi_poll *p;
	unsigned long deadline = jiffies + (timeout_ms > 0 ? msecs_to_jiffies(timeout_ms) : 0);
	__poll_t mask, want = (__poll_t)events | EPOLLERR | EPOLLHUP;
	bool first = true;
	int i;

	if (!f->f_op || !f->f_op->poll)
		return DEFAULT_POLLMASK & want;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return EPOLLERR;
	init_poll_funcptr(&p->pt, kpi_poll_queue);
	p->pt._key = want;
	init_completion(&p->woken);
	for (;;) {
		mask = f->f_op->poll(f, first ? &p->pt : NULL) & want;	/* register on the wait queues once */
		first = false;
		if (mask || !timeout_ms)
			break;
		if (timeout_ms < 0) {
			wait_for_completion(&p->woken);
		} else {
			long left = (long)(deadline - jiffies);

			if (left <= 0 || !wait_for_completion_timeout(&p->woken, left))
				break;		/* timed out: nothing ready (mask is 0) */
		}
		reinit_completion(&p->woken);
	}
	for (i = 0; i < p->n; i++)
		remove_wait_queue(p->e[i].head, &p->e[i].wait);
	kfree(p);
	return mask;
}
EXPORT_SYMBOL(kpi_file_poll);

extern int kpi_dma_buf_init(void);	/* drivers/dma-buf/dma-buf.c's initcall (build2.sh) */

int kpi_file_init(void)
{
	kpi_inode_init(&kpi_anon_inode, &kpi_anon_sb);
	return kpi_dma_buf_init();
}
