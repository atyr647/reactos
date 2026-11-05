// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI part 2, device model and miscellaneous kernel services: devres, struct device lifetime, classes,
 * debugfs/sysfs/seq_file stand-ins, ww_mutex, rw_semaphore, kthread workers, SRCU, and many small stubs.
 * Anything marked "stub" returns a neutral value; the driver paths that need real behaviour are listed in
 * README.md ("Still to do").
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/device/bus.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>
#include <linux/ww_mutex.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include <linux/swait.h>
#include <linux/kthread.h>
#include <linux/srcu.h>
#include <linux/uaccess.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/jiffies.h>
#include <linux/time64.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/moduleparam.h>
#include <linux/capability.h>
#include <linux/sysinfo.h>
#include <linux/i2c.h>
#include <linux/component.h>
#include <linux/hdmi.h>

extern void nt_dbgprint(const char *s);
extern void nt_sleep_ms(unsigned int ms);

/* ---- misc data ---------------------------------------------------------------------------- */
int oops_in_progress;
int overflowuid = 65534;
bool static_key_initialized = true;
struct resource iomem_resource = { .name = "PCI mem", .start = 0, .end = -1, .flags = IORESOURCE_MEM };
struct resource ioport_resource = { .name = "PCI IO", .start = 0, .end = 0xffff, .flags = IORESOURCE_IO };
const struct bus_type platform_bus_type = { .name = "platform" };
const struct bus_type pci_bus_type = { .name = "pci" };
const struct kernel_param_ops param_ops_int, param_ops_bool, param_ops_ulong;
struct static_key_false hugetlb_optimize_vmemmap_key;
unsigned long page_offset_base, vmemmap_base, phys_base;       /* see README: the memory layer defines these */
unsigned long __default_kernel_pte_mask = ~0UL;
kmem_buckets kmalloc_caches[NR_KMALLOC_TYPES];

void dump_stack(void) { nt_dbgprint("dump_stack()\n"); }
bool capable(int cap) { return true; }
int __request_module(bool wait, const char *fmt, ...) { return 0; }
void kernel_fpu_begin_mask(unsigned int mask) { }
void kernel_fpu_end(void) { }
void *memdup_user(const void __user *src, size_t len)
{
	void *p = kmalloc(len, GFP_KERNEL);
	if (!p) return ERR_PTR(-ENOMEM);
	memcpy(p, (const void __force *)src, len);
	return p;
}
void *memdup_user_nul(const void __user *src, size_t len)
{
	char *p = kmalloc(len + 1, GFP_KERNEL);
	if (!p) return ERR_PTR(-ENOMEM);
	memcpy(p, (const void __force *)src, len);
	p[len] = 0;
	return p;
}
void memcpy_fromio(void *to, const volatile void __iomem *from, size_t n) { memcpy(to, (const void *)from, n); }
void memcpy_toio(volatile void __iomem *to, const void *from, size_t n) { memcpy((void *)to, from, n); }
void memset_io(volatile void __iomem *a, int b, size_t c) { memset((void *)a, b, c); }
struct timespec64 ns_to_timespec64(const s64 nsec)
{
	struct timespec64 ts = { nsec / NSEC_PER_SEC, nsec % NSEC_PER_SEC };
	return ts;
}
u64 nsecs_to_jiffies64(u64 n) { return n / (NSEC_PER_SEC / HZ); }
bool is_vmalloc_addr(const void *x) { return false; }
loff_t noop_llseek(struct file *file, loff_t offset, int whence) { return file->f_pos; }
void si_meminfo(struct sysinfo *val) { memset(val, 0, sizeof(*val)); val->totalram = 1UL << 20; val->freeram = 1UL << 19; val->mem_unit = 4096; }
int __get_user_4(void) { return 0; }
void __put_user_4(void) { }
void __put_user_8(void) { }
void sched_set_fifo(struct task_struct *p) { }
struct task_struct *pid_task(struct pid *pid, enum pid_type type) { return NULL; }
pid_t pid_vnr(struct pid *pid) { return 0; }
void put_pid(struct pid *pid) { }
pid_t __task_pid_nr_ns(struct task_struct *task, enum pid_type type, struct pid_namespace *ns) { return 0; }
bool mutex_is_locked(struct mutex *lock) { return atomic_long_read(&lock->owner) != 0; }

/* ---- completion / waitqueue bits --------------------------------------------------------- */
void init_wait_entry(struct wait_queue_entry *wq_entry, int flags)
{
	wq_entry->flags = flags;
	wq_entry->private = current;
	wq_entry->func = autoremove_wake_function;
	INIT_LIST_HEAD(&wq_entry->entry);
}
void __init_swait_queue_head(struct swait_queue_head *q, const char *name, struct lock_class_key *key)
{
	raw_spin_lock_init(&q->lock);
	INIT_LIST_HEAD(&q->task_list);
}
bool try_wait_for_completion(struct completion *x)
{
	if (x->done > 0) { x->done--; return true; }
	return false;
}
int wait_for_completion_interruptible(struct completion *x) { wait_for_completion(x); return 0; }
long wait_for_completion_interruptible_timeout(struct completion *x, unsigned long timeout)
{
	return wait_for_completion_timeout(x, timeout);
}

/* ---- ww_mutex: no deadlock backoff, a plain mutex (uniprocessor model) ------------------- */
int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	if (ctx && lock->ctx == ctx)
		return -EALREADY;
	mutex_lock(&lock->base);
	lock->ctx = ctx;
	return 0;
}
int ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx) { return ww_mutex_lock(lock, ctx); }
int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	if (!mutex_trylock(&lock->base)) return 0;
	lock->ctx = ctx;
	return 1;
}
void ww_mutex_unlock(struct ww_mutex *lock)
{
	lock->ctx = NULL;
	mutex_unlock(&lock->base);
}

/* ---- rw_semaphore: spin/sleep on the count (0 free, >0 readers, -1 writer) ---------------- */
void down_read(struct rw_semaphore *sem)
{
	for (;;) {
		long c = atomic_long_read(&sem->count);
		if (c >= 0 && atomic_long_cmpxchg(&sem->count, c, c + 1) == c) return;
		nt_sleep_ms(0);
	}
}
void up_read(struct rw_semaphore *sem) { atomic_long_dec(&sem->count); }
void down_write(struct rw_semaphore *sem)
{
	while (atomic_long_cmpxchg(&sem->count, 0, -1) != 0)
		nt_sleep_ms(0);
}
void up_write(struct rw_semaphore *sem) { atomic_long_set(&sem->count, 0); }

/* ---- SRCU: grace periods are immediate in the uniprocessor model --------------------------- */
void __srcu_read_unlock(struct srcu_struct *ssp, int idx) { }
void synchronize_srcu(struct srcu_struct *ssp) { }
void srcu_drive_gp(struct work_struct *wp) { }

/* ---- kthread workers: one NT thread per worker, polling --------------------------------- */
extern int nt_thread_spawn(void (*fn)(void *), void *arg);
static void kpi_kworker_thread(void *arg)
{
	struct kthread_worker *w = arg;

	for (;;) {
		struct kthread_work *work = NULL;
		unsigned long flags;

		raw_spin_lock_irqsave(&w->lock, flags);
		if (!list_empty(&w->work_list)) {
			work = list_first_entry(&w->work_list, struct kthread_work, node);
			list_del_init(&work->node);
			w->current_work = work;
		} else if (w->flags & 0x80000000u) {
			raw_spin_unlock_irqrestore(&w->lock, flags);
			w->flags |= 0x40000000u;
			return;
		}
		raw_spin_unlock_irqrestore(&w->lock, flags);
		if (work) {
			work->func(work);
			w->current_work = NULL;
		} else {
			nt_sleep_ms(1);
		}
	}
}
struct kthread_worker *kthread_create_worker(unsigned int flags, const char namefmt[], ...)
{
	struct kthread_worker *w = kzalloc(sizeof(*w), GFP_KERNEL);

	if (!w) return ERR_PTR(-ENOMEM);
	raw_spin_lock_init(&w->lock);
	INIT_LIST_HEAD(&w->work_list);
	INIT_LIST_HEAD(&w->delayed_work_list);
	nt_thread_spawn(kpi_kworker_thread, w);
	return w;
}
bool kthread_queue_work(struct kthread_worker *worker, struct kthread_work *work)
{
	unsigned long flags;
	bool queued = false;

	raw_spin_lock_irqsave(&worker->lock, flags);
	if (list_empty(&work->node)) {
		list_add_tail(&work->node, &worker->work_list);
		work->worker = worker;
		queued = true;
	}
	raw_spin_unlock_irqrestore(&worker->lock, flags);
	return queued;
}
void kthread_flush_work(struct kthread_work *work)
{
	while (!list_empty(&work->node) || (work->worker && work->worker->current_work == work))
		nt_sleep_ms(1);
}
void kthread_flush_worker(struct kthread_worker *worker)
{
	while (!list_empty(&worker->work_list) || worker->current_work)
		nt_sleep_ms(1);
}
bool kthread_cancel_work_sync(struct kthread_work *work)
{
	unsigned long flags;
	bool pending = false;

	if (work->worker) {
		raw_spin_lock_irqsave(&work->worker->lock, flags);
		if (!list_empty(&work->node)) { list_del_init(&work->node); pending = true; }
		raw_spin_unlock_irqrestore(&work->worker->lock, flags);
	}
	kthread_flush_work(work);
	return pending;
}
void kthread_destroy_worker(struct kthread_worker *worker)
{
	kthread_flush_worker(worker);
	worker->flags |= 0x80000000u;
	while (!(worker->flags & 0x40000000u))
		nt_sleep_ms(1);
	kfree(worker);
}

/* ---- devres ------------------------------------------------------------------------------- */
struct kpi_devres {
	struct list_head entry;
	dr_release_t release;
	const char *name;
	size_t size;
	u8 data[] __aligned(16);
};

void *__devres_alloc_node(dr_release_t release, size_t size, gfp_t gfp, int nid, const char *name)
{
	struct kpi_devres *dr = kzalloc(sizeof(*dr) + size, gfp);

	if (!dr) return NULL;
	INIT_LIST_HEAD(&dr->entry);
	dr->release = release;
	dr->name = name;
	dr->size = size;
	return dr->data;
}
void devres_add(struct device *dev, void *res)
{
	struct kpi_devres *dr = container_of(res, struct kpi_devres, data);
	list_add_tail(&dr->entry, &dev->devres_head);
}
void devres_free(void *res) { if (res) kfree(container_of(res, struct kpi_devres, data)); }
static void kpi_devres_release_all(struct device *dev)
{
	while (!list_empty(&dev->devres_head)) {
		struct kpi_devres *dr = list_last_entry(&dev->devres_head, struct kpi_devres, entry);
		list_del(&dr->entry);
		if (dr->release) dr->release(dev, dr->data);
		kfree(dr);
	}
}
void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp)
{
	void *p = __devres_alloc_node(NULL, size, gfp, 0, "devm_kmalloc");
	if (p) devres_add(dev, p);
	return p;
}
void devm_kfree(struct device *dev, const void *p)
{
	struct kpi_devres *dr;

	if (!p) return;
	dr = container_of(p, struct kpi_devres, data);
	list_del(&dr->entry);
	kfree(dr);
}
struct action_devres { void *data; void (*action)(void *); };
static void kpi_action_release(struct device *dev, void *res)
{
	struct action_devres *a = res;
	a->action(a->data);
}
int __devm_add_action(struct device *dev, void (*action)(void *), void *data, const char *name)
{
	struct action_devres *a = __devres_alloc_node(kpi_action_release, sizeof(*a), GFP_KERNEL, 0, name);

	if (!a) return -ENOMEM;
	a->data = data;
	a->action = action;
	devres_add(dev, a);
	return 0;
}

/* ---- struct device ------------------------------------------------------------------------ */
void device_initialize(struct device *dev)
{
	INIT_LIST_HEAD(&dev->devres_head);
	kref_init(&dev->kobj.kref);
	INIT_LIST_HEAD(&dev->kobj.entry);
	dev->power.lock.rlock.raw_lock = (arch_spinlock_t){ };
}
int dev_set_name(struct device *dev, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	kfree_const(dev->kobj.name);
	dev->kobj.name = kvasprintf(GFP_KERNEL, fmt, ap);
	va_end(ap);
	return dev->kobj.name ? 0 : -ENOMEM;
}
int device_add(struct device *dev) { return 0; }
void device_del(struct device *dev) { kpi_devres_release_all(dev); }
int device_register(struct device *dev) { device_initialize(dev); return device_add(dev); }
void device_unregister(struct device *dev) { device_del(dev); put_device(dev); }
struct device *get_device(struct device *dev) { if (dev) kref_get(&dev->kobj.kref); return dev; }
static void kpi_device_free(struct kref *kref)
{
	struct device *dev = container_of(kref, struct device, kobj.kref);

	kpi_devres_release_all(dev);
	if (dev->release) dev->release(dev);
	else if (dev->type && dev->type->release) dev->type->release(dev);
}
void put_device(struct device *dev) { if (dev) kref_put(&dev->kobj.kref, kpi_device_free); }
const char *dev_driver_string(const struct device *dev) { return dev->driver ? dev->driver->name : ""; }
struct fwnode_handle *__dev_fwnode(struct device *dev) { return NULL; }
int dev_err_probe(const struct device *dev, int err, const char *fmt, ...) { return err; }
static void kpi_dev_print(const char *lvl, const struct device *dev, const char *fmt, va_list ap)
{
	char buf[200];
	int n = scnprintf(buf, sizeof(buf), "%s%s: ", lvl, dev && dev->kobj.name ? dev->kobj.name : "(null)");
	vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	nt_dbgprint(buf);
}
void _dev_printk(const char *level, const struct device *dev, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); kpi_dev_print("", dev, fmt, ap); va_end(ap); }
void _dev_err(const struct device *dev, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); kpi_dev_print("ERR ", dev, fmt, ap); va_end(ap); }
void _dev_warn(const struct device *dev, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); kpi_dev_print("WARN ", dev, fmt, ap); va_end(ap); }
void _dev_info(const struct device *dev, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); kpi_dev_print("", dev, fmt, ap); va_end(ap); }

/* ---- classes, sysfs, kobject uevents: no sysfs here ------------------------------------- */
struct class *class_create(const char *name)
{
	struct class *cls = kzalloc(sizeof(*cls), GFP_KERNEL);

	if (!cls) return ERR_PTR(-ENOMEM);
	cls->name = name;
	return cls;
}
void class_destroy(const struct class *cls) { kfree(cls); }
int class_create_file_ns(const struct class *cls, const struct class_attribute *attr, const void *ns) { return 0; }
void class_remove_file_ns(const struct class *cls, const struct class_attribute *attr, const void *ns) { }
ssize_t show_class_attr_string(const struct class *class, const struct class_attribute *attr, char *buf) { return 0; }
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action, char *envp[]) { return 0; }
int sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vscnprintf(buf, PAGE_SIZE, fmt, ap);
	va_end(ap);
	return n;
}
int sysfs_create_link(struct kobject *kobj, struct kobject *target, const char *name) { return 0; }
void sysfs_remove_link(struct kobject *kobj, const char *name) { }
int __register_chrdev(unsigned int major, unsigned int baseminor, unsigned int count, const char *name, const struct file_operations *fops) { return 0; }
void __unregister_chrdev(unsigned int major, unsigned int baseminor, unsigned int count, const char *name) { }
struct resource *__request_region(struct resource *parent, resource_size_t start, resource_size_t n, const char *name, int flags)
{
	static struct resource dummy;
	return &dummy;
}
void __release_region(struct resource *parent, resource_size_t start, resource_size_t n) { }

/* ---- debugfs / seq_file / simple_fs: nothing is exposed ------------------------------------ */
static struct dentry kpi_dummy_dentry;
struct dentry *debugfs_create_dir(const char *name, struct dentry *parent) { return &kpi_dummy_dentry; }
struct dentry *debugfs_create_file(const char *name, umode_t mode, struct dentry *parent, void *data, const struct file_operations *fops) { return &kpi_dummy_dentry; }
void debugfs_create_atomic_t(const char *name, umode_t mode, struct dentry *parent, atomic_t *value) { }
struct dentry *debugfs_create_symlink(const char *name, struct dentry *parent, const char *target) { return &kpi_dummy_dentry; }
struct dentry *debugfs_lookup(const char *name, struct dentry *parent) { return NULL; }
void debugfs_remove(struct dentry *dentry) { }
int simple_open(struct inode *inode, struct file *file) { return 0; }
ssize_t simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos, const void *from, size_t available)
{
	loff_t pos = *ppos;

	if (pos < 0) return -EINVAL;
	if (pos >= available || !count) return 0;
	if (count > available - pos) count = available - pos;
	memcpy((void __force *)to, (const char *)from + pos, count);
	*ppos = pos + count;
	return count;
}
int simple_pin_fs(struct file_system_type *type, struct vfsmount **mount, int *count) { return -ENODEV; }
void simple_release_fs(struct vfsmount **mount, int *count) { }
int single_open(struct file *file, int (*show)(struct seq_file *, void *), void *data) { return -ENOSYS; }
int single_release(struct inode *inode, struct file *file) { return 0; }
ssize_t seq_read(struct file *file, char __user *buf, size_t size, loff_t *ppos) { return -ENOSYS; }
loff_t seq_lseek(struct file *file, loff_t offset, int whence) { return -ENOSYS; }
int seq_write(struct seq_file *seq, const void *data, size_t len) { return -ENOSYS; }
void __seq_puts(struct seq_file *m, const char *s) { }
void seq_putc(struct seq_file *m, char c) { }

/* ---- file/fd/inode objects used for GEM mmap and dma-buf export: not supported yet --------- */
int get_unused_fd_flags(unsigned flags) { return -ENOSYS; }
void put_unused_fd(unsigned int fd) { }
void fd_install(unsigned int fd, struct file *file) { }
struct fd fdget(unsigned int fd) { struct fd f = { 0 }; return f; }
void fput(struct file *file) { }
void iput(struct inode *inode) { }
struct file *anon_inode_getfile(const char *name, const struct file_operations *fops, void *priv, int flags) { return ERR_PTR(-ENOSYS); }
struct inode *alloc_anon_inode(struct super_block *s) { return ERR_PTR(-ENOSYS); }
struct file *dentry_open(const struct path *path, int flags, const struct cred *cred) { return ERR_PTR(-ENOSYS); }
int init_pseudo(struct fs_context *fc, unsigned long magic) { return -ENOSYS; }
void kill_anon_super(struct super_block *sb) { }
struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags) { return ERR_PTR(-ENOSYS); }
void unmap_mapping_range(struct address_space *mapping, loff_t const holebegin, loff_t const holelen, int even_cows) { }

/* ---- firmware framebuffer / aperture, i2c, acpi, hdmi, components, video: nothing to do --------- */
int aperture_remove_conflicting_devices(resource_size_t base, resource_size_t size, const char *name) { return 0; }
int aperture_remove_conflicting_pci_devices(struct pci_dev *pdev, const char *name) { return 0; }
int devm_aperture_acquire_for_platform_device(struct platform_device *pdev, resource_size_t base, resource_size_t size) { return 0; }
struct i2c_client *i2c_new_client_device(struct i2c_adapter *adap, struct i2c_board_info const *info) { return ERR_PTR(-ENODEV); }
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num) { return -ENODEV; }
void i2c_unregister_device(struct i2c_client *client) { }
bool is_acpi_device_node(const struct fwnode_handle *fwnode) { return false; }
int register_acpi_bus_type(struct acpi_bus_type *type) { return 0; }
int unregister_acpi_bus_type(struct acpi_bus_type *type) { return 0; }
int component_add(struct device *dev, const struct component_ops *ops) { return 0; }
void component_del(struct device *dev, const struct component_ops *ops) { }
bool video_firmware_drivers_only(void) { return false; }
char *video_get_options(const char *name) { return NULL; }
struct backlight_device *devm_of_find_backlight(struct device *dev) { return NULL; }
int drm_get_panel_orientation_quirk(int width, int height) { return -1; }

/* ---- page cache attribute bits: filled in properly by the memory layer (README: Still to do) ---- */
unsigned long cachemode2protval(enum page_cache_mode pcm) { return 0; }
