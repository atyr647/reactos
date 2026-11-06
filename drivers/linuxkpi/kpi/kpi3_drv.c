// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI part 3: a real (minimal) driver core, plus the odds and ends the USB/HID/xHCI stack needs.
 *
 * kpi2_dev.c's device_add() used to be a no-op, which is fine for DRM (its devices have no bus), but usbcore
 * registers a bus_type and binds devices to drivers by calling the bus match/probe callbacks. This file keeps
 * a registry of buses, devices and drivers and does that binding. No sysfs, no uevents, no hotplug thread:
 * binding is synchronous from device_add()/driver_register().
 */
#include <linux/module.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/kfifo.h>
#include <linux/input.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/pm_qos.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/semaphore.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/utsname.h>
#include <linux/nls.h>
#include <linux/debugfs.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/uuid.h>
#include <linux/kernfs.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/genalloc.h>
#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/sched/debug.h>

extern void nt_dbgprint(const char *s);
extern void *nt_ioremap(unsigned long long phys, unsigned long len);

/* ================================================================================================
 * driver core
 * ============================================================================================== */
struct kpi_bus {
	struct list_head node;
	const struct bus_type *bus;
	struct list_head devs;		/* struct device via kobj.entry */
	struct list_head drvs;		/* struct kpi_drv */
	struct blocking_notifier_head nb;
};
struct kpi_drv {
	struct list_head node;
	struct device_driver *drv;
};

static LIST_HEAD(kpi_buses);
static DEFINE_MUTEX(kpi_bus_lock);	/* only protects the lists, never held across probe() */

static struct kpi_bus *kpi_bus_get(const struct bus_type *bus)
{
	struct kpi_bus *b;

	list_for_each_entry(b, &kpi_buses, node)
		if (b->bus == bus)
			return b;
	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return NULL;
	b->bus = bus;
	INIT_LIST_HEAD(&b->devs);
	INIT_LIST_HEAD(&b->drvs);
	BLOCKING_INIT_NOTIFIER_HEAD(&b->nb);
	list_add_tail(&b->node, &kpi_buses);
	return b;
}

int bus_register(const struct bus_type *bus)
{
	return kpi_bus_get(bus) ? 0 : -ENOMEM;
}
void bus_unregister(const struct bus_type *bus) { }
int bus_register_notifier(const struct bus_type *bus, struct notifier_block *nb)
{
	struct kpi_bus *b = kpi_bus_get(bus);
	return b ? blocking_notifier_chain_register(&b->nb, nb) : -ENOMEM;
}
int bus_unregister_notifier(const struct bus_type *bus, struct notifier_block *nb)
{
	struct kpi_bus *b = kpi_bus_get(bus);
	return b ? blocking_notifier_chain_unregister(&b->nb, nb) : -ENOMEM;
}

static int kpi_try_bind(struct device *dev, struct device_driver *drv)
{
	const struct bus_type *bus = dev->bus;
	int ret;

	if (dev->driver)
		return 0;
	if (bus && bus->match) {
		ret = bus->match(dev, drv);
		if (ret <= 0)
			return 0;
	}
	dev->driver = drv;
	if (bus && bus->probe)
		ret = bus->probe(dev);
	else if (drv->probe)
		ret = drv->probe(dev);
	else
		ret = 0;
	if (ret) {
		dev->driver = NULL;
		return 0;
	}
	return 1;
}

int device_attach(struct device *dev)
{
	struct kpi_bus *b;
	struct kpi_drv *d;
	struct device_driver **cand;
	int i, n = 0;

	if (!dev->bus || !(b = kpi_bus_get(dev->bus)))
		return 0;
	if (dev->driver)
		return 1;
	/* snapshot the driver list: probe() may register further drivers */
	mutex_lock(&kpi_bus_lock);
	list_for_each_entry(d, &b->drvs, node)
		n++;
	cand = kcalloc(n ? n : 1, sizeof(*cand), GFP_KERNEL);
	if (cand) {
		i = 0;
		list_for_each_entry(d, &b->drvs, node)
			cand[i++] = d->drv;
	}
	mutex_unlock(&kpi_bus_lock);
	if (!cand)
		return 0;
	for (i = 0; i < n && !dev->driver; i++)
		kpi_try_bind(dev, cand[i]);
	kfree(cand);
	return dev->driver != NULL;
}

int driver_register(struct device_driver *drv)
{
	struct kpi_bus *b;
	struct kpi_drv *d;
	struct list_head *pos;
	struct device **cand;
	int i, n = 0;

	if (!drv->bus || !(b = kpi_bus_get(drv->bus)))
		return -EINVAL;
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->drv = drv;
	drv->p = (struct driver_private *)d;
	mutex_lock(&kpi_bus_lock);
	list_add_tail(&d->node, &b->drvs);
	list_for_each(pos, &b->devs)
		n++;
	cand = kcalloc(n ? n : 1, sizeof(*cand), GFP_KERNEL);
	if (cand) {
		i = 0;
		list_for_each(pos, &b->devs)
			cand[i++] = get_device(container_of(pos, struct device, kobj.entry));
	}
	mutex_unlock(&kpi_bus_lock);
	if (!cand)
		return 0;
	for (i = 0; i < n; i++) {
		kpi_try_bind(cand[i], drv);
		put_device(cand[i]);
	}
	kfree(cand);
	return 0;
}
EXPORT_SYMBOL_GPL(driver_register);

int driver_attach(const struct device_driver *drv)
{
	struct kpi_bus *b = drv->bus ? kpi_bus_get(drv->bus) : NULL;
	struct list_head *pos;
	int n = 0;

	if (!b)
		return 0;
	list_for_each(pos, &b->devs)
		n += kpi_try_bind(container_of(pos, struct device, kobj.entry), (struct device_driver *)drv);
	return n;
}

static void kpi_unbind(struct device *dev)
{
	struct device_driver *drv = dev->driver;

	if (!drv)
		return;
	if (dev->bus && dev->bus->remove)
		dev->bus->remove(dev);
	else if (drv->remove)
		drv->remove(dev);
	dev->driver = NULL;
	dev_set_drvdata(dev, NULL);
}
void device_release_driver(struct device *dev) { kpi_unbind(dev); }

void driver_unregister(struct device_driver *drv)
{
	struct kpi_bus *b = drv->bus ? kpi_bus_get(drv->bus) : NULL;
	struct kpi_drv *d = (struct kpi_drv *)drv->p;
	struct list_head *pos, *n;

	if (!b)
		return;
	list_for_each_safe(pos, n, &b->devs) {
		struct device *dev = container_of(pos, struct device, kobj.entry);
		if (dev->driver == drv)
			kpi_unbind(dev);
	}
	if (d) {
		mutex_lock(&kpi_bus_lock);
		list_del(&d->node);
		mutex_unlock(&kpi_bus_lock);
		kfree(d);
		drv->p = NULL;
	}
}
int device_bind_driver(struct device *dev) { return 0; }
int device_reprobe(struct device *dev)
{
	kpi_unbind(dev);
	return device_attach(dev) ? 0 : -ENODEV;
}
int bus_rescan_devices(const struct bus_type *bus)
{
	struct kpi_bus *b = kpi_bus_get(bus);
	struct list_head *pos;

	if (!b)
		return -ENOMEM;
	list_for_each(pos, &b->devs)
		device_attach(container_of(pos, struct device, kobj.entry));
	return 0;
}
int bus_for_each_dev(const struct bus_type *bus, struct device *start, void *data, int (*fn)(struct device *, void *))
{
	struct kpi_bus *b = kpi_bus_get(bus);
	struct list_head *pos, *n;
	int err = 0;

	if (!b)
		return -ENOMEM;
	list_for_each_safe(pos, n, &b->devs) {
		err = fn(container_of(pos, struct device, kobj.entry), data);
		if (err)
			break;
	}
	return err;
}
int bus_for_each_drv(const struct bus_type *bus, struct device_driver *start, void *data, int (*fn)(struct device_driver *, void *))
{
	struct kpi_bus *b = kpi_bus_get(bus);
	struct kpi_drv *d, *n;
	int err = 0;

	if (!b)
		return -ENOMEM;
	list_for_each_entry_safe(d, n, &b->drvs, node) {
		err = fn(d->drv, data);
		if (err)
			break;
	}
	return err;
}
struct device *bus_find_device(const struct bus_type *bus, struct device *start, const void *data, device_match_t match)
{
	struct kpi_bus *b = kpi_bus_get(bus);
	struct list_head *pos;

	if (!b)
		return NULL;
	list_for_each(pos, &b->devs) {
		struct device *dev = container_of(pos, struct device, kobj.entry);
		if (match(dev, data))
			return get_device(dev);
	}
	return NULL;
}

/* device_add/del replace the no-op versions in kpi2_dev.c (which must not define them) */
int device_add(struct device *dev)
{
	struct kpi_bus *b;

	if (dev->bus && (b = kpi_bus_get(dev->bus))) {
		get_device(dev);
		mutex_lock(&kpi_bus_lock);
		list_add_tail(&dev->kobj.entry, &b->devs);
		mutex_unlock(&kpi_bus_lock);
		blocking_notifier_call_chain(&b->nb, BUS_NOTIFY_ADD_DEVICE, dev);
		device_attach(dev);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(device_add);

extern void kpi_devres_release_all(struct device *dev);
void device_del(struct device *dev)
{
	struct kpi_bus *b;

	if (dev->bus && (b = kpi_bus_get(dev->bus))) {
		blocking_notifier_call_chain(&b->nb, BUS_NOTIFY_DEL_DEVICE, dev);
		kpi_unbind(dev);
		mutex_lock(&kpi_bus_lock);
		if (!list_empty(&dev->kobj.entry) && dev->kobj.entry.next) {
			list_del_init(&dev->kobj.entry);
			mutex_unlock(&kpi_bus_lock);
			put_device(dev);
			return;
		}
		mutex_unlock(&kpi_bus_lock);
	}
	kpi_devres_release_all(dev);
}
EXPORT_SYMBOL_GPL(device_del);

int driver_create_file(const struct device_driver *drv, const struct driver_attribute *attr) { return 0; }
void driver_remove_file(const struct device_driver *drv, const struct driver_attribute *attr) { }
int class_register(const struct class *cls) { return 0; }
void class_unregister(const struct class *cls) { }
struct device *device_create(const struct class *cls, struct device *parent, dev_t devt, void *drvdata, const char *fmt, ...)
{
	struct device *dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	va_list ap;

	if (!dev)
		return ERR_PTR(-ENOMEM);
	device_initialize(dev);
	dev->class = cls;
	dev->parent = parent;
	dev->devt = devt;
	dev_set_drvdata(dev, drvdata);
	va_start(ap, fmt);
	kfree_const(dev->kobj.name);
	dev->kobj.name = kvasprintf(GFP_KERNEL, fmt, ap);
	va_end(ap);
	return dev;
}
void device_destroy(const struct class *cls, dev_t devt) { }
int device_match_devt(struct device *dev, const void *pdevt) { return dev->devt == *(const dev_t *)pdevt; }
struct device_link *device_link_add(struct device *consumer, struct device *supplier, u32 flags)
{
	static struct device_link dummy;
	return &dummy;
}
int device_pm_wait_for_dev(struct device *sub, struct device *dev) { return 0; }
void device_set_node(struct device *dev, struct fwnode_handle *fwnode) { }
void set_primary_fwnode(struct device *dev, struct fwnode_handle *fwnode) { }
void device_set_of_node_from_dev(struct device *dev, const struct device *dev2) { }
int device_create_managed_software_node(struct device *dev, const struct property_entry *properties, const struct software_node *parent) { return 0; }
const struct fwnode_handle *__dev_fwnode_const(const struct device *dev) { return NULL; }
int device_property_read_string(const struct device *dev, const char *propname, const char **val) { return -EINVAL; }
struct fwnode_handle *fwnode_find_reference(const struct fwnode_handle *fwnode, const char *name, unsigned int index) { return ERR_PTR(-ENOENT); }
int fwnode_property_match_property_string(const struct fwnode_handle *fwnode, const char *propname, const char * const *array, size_t n) { return -ENOENT; }

void _dev_notice(const struct device *dev, const char *fmt, ...)
{
	char buf[200];
	va_list ap;
	int n = scnprintf(buf, sizeof(buf), "%s: ", dev && dev->kobj.name ? dev->kobj.name : "(null)");

	va_start(ap, fmt);
	vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	nt_dbgprint(buf);
}

/* ---- power management: runtime PM is always "active", wakeup is off, PM QoS ignored ------------- */
int __pm_runtime_idle(struct device *dev, int rpmflags) { return 0; }
int __pm_runtime_set_status(struct device *dev, unsigned int status) { return 0; }
int pm_runtime_barrier(struct device *dev) { return 0; }
void pm_runtime_no_callbacks(struct device *dev) { }
void device_set_wakeup_capable(struct device *dev, bool capable) { }
int device_set_wakeup_enable(struct device *dev, bool enable) { return 0; }
int device_wakeup_enable(struct device *dev) { return 0; }
void device_wakeup_disable(struct device *dev) { }
void pm_wakeup_dev_event(struct device *dev, unsigned int msec, bool hard) { }
int dev_pm_qos_add_request(struct device *dev, struct dev_pm_qos_request *req, enum dev_pm_qos_req_type type, s32 value) { return 0; }
int dev_pm_qos_remove_request(struct dev_pm_qos_request *req) { return 0; }
enum pm_qos_flags_status dev_pm_qos_flags(struct device *dev, s32 mask) { return PM_QOS_FLAGS_UNDEFINED; }
int dev_pm_qos_expose_flags(struct device *dev, s32 value) { return 0; }
void __suspend_report_result(const char *function, struct device *dev, void *fn, int ret) { }
struct workqueue_struct *pm_wq;
const char power_group_name[] = "power";

/* ---- sysfs / kernfs / kobject: nothing is exposed ----------------------------------------------- */
int sysfs_merge_group(struct kobject *kobj, const struct attribute_group *grp) { return 0; }
void sysfs_unmerge_group(struct kobject *kobj, const struct attribute_group *grp) { }
int sysfs_update_group(struct kobject *kobj, const struct attribute_group *grp) { return 0; }
struct kernfs_node *sysfs_break_active_protection(struct kobject *kobj, const struct attribute *attr) { return NULL; }
void sysfs_unbreak_active_protection(struct kernfs_node *kn) { }
void sysfs_notify(struct kobject *kobj, const char *dir, const char *attr) { }
/* there is no sysfs, but callers treat a missing attribute node as fatal: hand out one shared dummy */
static char kpi_kn_dummy[256];
struct kernfs_node *kernfs_find_and_get_ns(struct kernfs_node *parent, const char *name, const void *ns) { return (struct kernfs_node *)kpi_kn_dummy; }
void kernfs_notify(struct kernfs_node *kn) { }
void kernfs_put(struct kernfs_node *kn) { }
int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...) { return 0; }
int kobject_uevent(struct kobject *kobj, enum kobject_action action) { return 0; }
char *kobject_get_path(const struct kobject *kobj, gfp_t gfp_mask) { return kstrdup(kobj->name ? kobj->name : "", gfp_mask); }
void debugfs_create_regset32(const char *name, umode_t mode, struct dentry *parent, struct debugfs_regset32 *regset) { }
void debugfs_lookup_and_remove(const char *name, struct dentry *parent) { }
void add_device_randomness(const void *buf, size_t len) { }

/* ---- char devices ------------------------------------------------------------------------------ */
static unsigned int kpi_next_major = 240;
int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count, const char *name)
{
	*dev = MKDEV(kpi_next_major++, baseminor);
	return 0;
}
int register_chrdev_region(dev_t from, unsigned count, const char *name) { return 0; }
void unregister_chrdev_region(dev_t from, unsigned count) { }
void cdev_init(struct cdev *cdev, const struct file_operations *fops) { memset(cdev, 0, sizeof(*cdev)); cdev->ops = fops; }
int cdev_add(struct cdev *p, dev_t dev, unsigned count) { p->dev = dev; p->count = count; return 0; }
void cdev_del(struct cdev *p) { }
int fasync_helper(int fd, struct file *filp, int on, struct fasync_struct **fapp) { return 0; }
void kill_fasync(struct fasync_struct **fp, int sig, int band) { }
int kill_pid_usb_asyncio(int sig, int errno, sigval_t addr, struct pid *pid, const struct cred *cred) { return 0; }
loff_t no_seek_end_llseek(struct file *file, loff_t offset, int whence) { return -ESPIPE; }
long compat_ptr_ioctl(struct file *file, unsigned int cmd, unsigned long arg) { return -ENOIOCTLCMD; }
int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t prot) { return -ENODEV; }
int dma_mmap_attrs(struct device *dev, struct vm_area_struct *vma, void *cpu_addr, dma_addr_t dma_addr, size_t size, unsigned long attrs) { return -ENODEV; }
void __put_cred(struct cred *cred) { }
struct uts_namespace init_uts_ns;
struct timespec64 current_time(struct inode *inode)
{
	struct timespec64 ts = { .tv_sec = ktime_get_real_seconds() };
	return ts;
}
struct timespec64 inode_set_ctime_current(struct inode *inode)
{
	return current_time(inode);
}

/* ---- scheduler / sync odds and ends ------------------------------------------------------------- */
struct workqueue_struct *system_bh_wq, *system_bh_highpri_wq, *system_power_efficient_wq;
void kpi3_init_wqs(void)
{
	extern struct workqueue_struct *system_wq;
	/* Linux runs BH work in softirq context, independent of the normal workers: URB completions
	 * (usb_giveback_urb_bh) are queued from, and waited for by, work running on system_wq. Sharing a
	 * thread deadlocks, so BH gets its own workers. */
	system_bh_wq = alloc_workqueue("bh", 0, 1);
	system_bh_highpri_wq = alloc_workqueue("bh_highpri", 0, 1);
	system_power_efficient_wq = system_wq;
	pm_wq = system_wq;
}
void __sched yield(void) { schedule_timeout_uninterruptible(1); }
signed long __sched schedule_timeout_uninterruptible(signed long timeout) { msleep(jiffies_to_msecs(timeout)); return 0; }
long __sched wait_for_completion_killable_timeout(struct completion *x, unsigned long timeout) { return wait_for_completion_timeout(x, timeout); }
void add_timer(struct timer_list *timer) { mod_timer(timer, timer->expires); }

/* counting semaphore: poll with a short sleep, the callers (usbcore) hold it only briefly */
int __sched down_interruptible(struct semaphore *sem)
{
	for (;;) {
		unsigned long flags;
		int got = 0;

		raw_spin_lock_irqsave(&sem->lock, flags);
		if (sem->count > 0) { sem->count--; got = 1; }
		raw_spin_unlock_irqrestore(&sem->lock, flags);
		if (got)
			return 0;
		msleep(1);
	}
}
int __sched down_trylock(struct semaphore *sem)
{
	unsigned long flags;
	int fail = 1;

	raw_spin_lock_irqsave(&sem->lock, flags);
	if (sem->count > 0) { sem->count--; fail = 0; }
	raw_spin_unlock_irqrestore(&sem->lock, flags);
	return fail;
}
EXPORT_SYMBOL(down_trylock);

/* ---- notifier chains --------------------------------------------------------------------------- */
int blocking_notifier_chain_register(struct blocking_notifier_head *nh, struct notifier_block *n)
{
	struct notifier_block **p = &nh->head;

	while (*p && (*p)->priority >= n->priority)
		p = &(*p)->next;
	n->next = *p;
	*p = n;
	return 0;
}
int blocking_notifier_chain_unregister(struct blocking_notifier_head *nh, struct notifier_block *n)
{
	struct notifier_block **p;

	for (p = &nh->head; *p; p = &(*p)->next)
		if (*p == n) { *p = n->next; return 0; }
	return -ENOENT;
}
int blocking_notifier_call_chain(struct blocking_notifier_head *nh, unsigned long val, void *v)
{
	struct notifier_block *n;
	int ret = NOTIFY_DONE;

	for (n = nh->head; n; n = n->next) {
		ret = n->notifier_call(n, val, v);
		if (ret & NOTIFY_STOP_MASK)
			break;
	}
	return ret;
}

/* ---- module parameters ------------------------------------------------------------------------- */
static int kpi_param_nop_set(const char *val, const struct kernel_param *kp) { return 0; }
static int kpi_param_nop_get(char *buffer, const struct kernel_param *kp) { return 0; }
const struct kernel_param_ops param_array_ops = { .set = kpi_param_nop_set, .get = kpi_param_nop_get };
int param_set_copystring(const char *val, const struct kernel_param *kp)
{
	const struct kparam_string *kps = kp->str;

	strscpy(kps->string, val, kps->maxlen);
	return 0;
}
int param_get_string(char *buffer, const struct kernel_param *kp)
{
	const struct kparam_string *kps = kp->str;

	return scnprintf(buffer, PAGE_SIZE, "%s\n", kps->string);
}

/* ---- DMA: pools are one coherent allocation per object, scatterlist mapping is the identity ------ */
struct dma_pool {
	struct device *dev;
	size_t size;
};
struct dma_pool *dma_pool_create(const char *name, struct device *dev, size_t size, size_t align, size_t boundary)
{
	struct dma_pool *p = kzalloc(sizeof(*p), GFP_KERNEL);

	if (!p)
		return NULL;
	p->dev = dev;
	p->size = size;
	return p;
}
void dma_pool_destroy(struct dma_pool *pool) { kfree(pool); }
void *dma_pool_alloc(struct dma_pool *pool, gfp_t mem_flags, dma_addr_t *handle)
{
	void *v = dma_alloc_coherent(pool->dev, pool->size, handle, mem_flags);

	if (v && (mem_flags & __GFP_ZERO))
		memset(v, 0, pool->size);
	return v;
}
void dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t addr) { dma_free_coherent(pool->dev, pool->size, vaddr, addr); }
void *dmam_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs)
{
	return dma_alloc_attrs(dev, size, dma_handle, gfp, attrs);
}
extern phys_addr_t kpi_page_phys(struct page *page, size_t offset);
unsigned int dma_map_sg_attrs(struct device *dev, struct scatterlist *sgl, int nents, enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i) {
		sg_dma_address(sg) = kpi_page_phys(sg_page(sg), sg->offset);
		sg_dma_len(sg) = sg->length;
	}
	return nents;
}
void __dma_sync_single_for_cpu_stub(void) { }

/* ---- misc library ------------------------------------------------------------------------------ */
bool uuid_is_valid(const char *uuid)
{
	static const u8 dash[] = { 8, 13, 18, 23 };
	int i;

	for (i = 0; i < UUID_STRING_LEN; i++) {
		if (i == dash[0] || i == dash[1] || i == dash[2] || i == dash[3]) {
			if (uuid[i] != '-')
				return false;
		} else if (!isxdigit(uuid[i])) {
			return false;
		}
	}
	return uuid[UUID_STRING_LEN] == '\0';
}
int guid_parse(const char *uuid, guid_t *u)
{
	static const u8 si[16] = { 6, 4, 2, 0, 11, 9, 16, 14, 19, 21, 24, 26, 28, 30, 32, 34 };
	int i;

	if (!uuid_is_valid(uuid))
		return -EINVAL;
	for (i = 0; i < 16; i++)
		u->b[i] = (hex_to_bin(uuid[si[i]]) << 4) | hex_to_bin(uuid[si[i] + 1]);
	return 0;
}
int utf16s_to_utf8s(const wchar_t *pwcs, int inlen, enum utf16_endian endian, u8 *s, int maxout)
{
	u8 *op = s;
	int i;

	for (i = 0; i < inlen && pwcs[i]; i++) {
		unsigned int c = pwcs[i];

		if (endian == UTF16_BIG_ENDIAN)
			c = ((c & 0xff) << 8) | (c >> 8);
		if (c < 0x80) {
			if (op - s + 1 > maxout) break;
			*op++ = c;
		} else if (c < 0x800) {
			if (op - s + 2 > maxout) break;
			*op++ = 0xc0 | (c >> 6);
			*op++ = 0x80 | (c & 0x3f);
		} else {
			if (op - s + 3 > maxout) break;
			*op++ = 0xe0 | (c >> 12);
			*op++ = 0x80 | ((c >> 6) & 0x3f);
			*op++ = 0x80 | (c & 0x3f);
		}
	}
	return op - s;
}
void *__kmalloc_cache_node_noprof(struct kmem_cache *s, gfp_t gfpflags, int node, size_t size) { return kmalloc(size, gfpflags); }
const char *trace_print_symbols_seq(struct trace_seq *p, unsigned long val, const struct trace_print_flags *symbol_array) { return ""; }
void *devm_ioremap(struct device *dev, resource_size_t offset, resource_size_t size) { return nt_ioremap(offset, size); }
void *devm_memremap(struct device *dev, resource_size_t offset, size_t size, unsigned long flags) { return NULL; }
struct resource *__devm_request_region(struct device *dev, struct resource *parent, resource_size_t start, resource_size_t n, const char *name)
{
	static struct resource dummy;
	return &dummy;
}

/* ---- PCI odds and ends ------------------------------------------------------------------------- */
pci_power_t pci_choose_state(struct pci_dev *pdev, pm_message_t state) { return PCI_D0; }
void pci_d3cold_disable(struct pci_dev *dev) { }
bool pci_dev_run_wake(struct pci_dev *dev) { return false; }
struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from) { return NULL; }
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids, struct pci_dev *dev)
{
	for (; ids->vendor || ids->subvendor || ids->class_mask; ids++)
		if ((ids->vendor == PCI_ANY_ID || ids->vendor == dev->vendor) &&
		    (ids->device == PCI_ANY_ID || ids->device == dev->device) &&
		    !((ids->class ^ dev->class) & ids->class_mask))
			return ids;
	return NULL;
}
int pci_prepare_to_sleep(struct pci_dev *dev) { return 0; }
int pci_set_mwi(struct pci_dev *dev) { return 0; }
void synchronize_irq(unsigned int irq) { }
struct platform_device *platform_device_alloc(const char *name, int id) { return NULL; }
int platform_device_add(struct platform_device *pdev) { return -ENODEV; }
int platform_device_add_resources(struct platform_device *pdev, const struct resource *res, unsigned int num) { return -ENODEV; }
void platform_device_put(struct platform_device *pdev) { }

/* ---- ACPI: no ACPI devices are visible to the shim ---------------------------------------------- */
#ifdef CONFIG_ACPI
bool acpi_bus_power_manageable(acpi_handle handle) { return false; }
int acpi_bus_set_power(acpi_handle handle, int state) { return -ENODEV; }
bool acpi_check_dsm(acpi_handle handle, const guid_t *guid, u64 rev, u64 funcs) { return false; }
struct acpi_device *acpi_fetch_acpi_dev(acpi_handle handle) { return NULL; }
struct acpi_device *acpi_find_child_device(struct acpi_device *parent, u64 address, bool check_children) { return NULL; }
struct acpi_device *acpi_find_child_by_adr(struct acpi_device *adev, u64 adr) { return NULL; }
acpi_status acpi_get_physical_device_location(acpi_handle handle, struct acpi_pld_info **pld) { return AE_NOT_FOUND; }
#endif

/* ================================================================================================
 * input core: events are logged and forwarded to an NT-side hook once the keyboard/mouse bridge exists
 * ============================================================================================== */
typedef void (*kpi_input_hook_t)(const char *name, unsigned int type, unsigned int code, int value);
kpi_input_hook_t kpi_input_hook;
EXPORT_SYMBOL_GPL(kpi_input_hook);

struct input_dev *input_allocate_device(void)
{
	struct input_dev *d = kzalloc(sizeof(*d), GFP_KERNEL);

	if (d) {
		mutex_init(&d->mutex);
		spin_lock_init(&d->event_lock);
		device_initialize(&d->dev);
		INIT_LIST_HEAD(&d->h_list);
		INIT_LIST_HEAD(&d->node);
	}
	return d;
}
void input_free_device(struct input_dev *d) { kfree(d); }
int input_register_device(struct input_dev *d)
{
	pr_info("kpi3_input: registered '%s' (bus %04x vendor %04x product %04x)\n",
		d->name ? d->name : "?", d->id.bustype, d->id.vendor, d->id.product);
	if (d->open)
		d->open(d);
	return 0;
}
void input_unregister_device(struct input_dev *d)
{
	if (d->close)
		d->close(d);
}
void input_event(struct input_dev *d, unsigned int type, unsigned int code, int value)
{
	if (type == EV_KEY && kpi_input_hook == NULL)
		pr_info("kpi3_input: %s key %u %s\n", d->name ? d->name : "?", code, value ? "down" : "up");
	if (kpi_input_hook)
		kpi_input_hook(d->name, type, code, value);
}
void input_set_abs_params(struct input_dev *d, unsigned int axis, int min, int max, int fuzz, int flat)
{
	if (d->absinfo) {
		d->absinfo[axis].minimum = min;
		d->absinfo[axis].maximum = max;
		d->absinfo[axis].fuzz = fuzz;
		d->absinfo[axis].flat = flat;
	}
	__set_bit(EV_ABS, d->evbit);
	__set_bit(axis, d->absbit);
}
void input_alloc_absinfo(struct input_dev *d)
{
	if (!d->absinfo)
		d->absinfo = kcalloc(ABS_CNT, sizeof(*d->absinfo), GFP_KERNEL);
}
int input_ff_create(struct input_dev *d, unsigned int max_effects) { return -ENOSYS; }
int input_ff_event(struct input_dev *d, unsigned int type, unsigned int code, int value) { return -ENOSYS; }
int input_scancode_to_scalar(const struct input_keymap_entry *ke, unsigned int *scancode)
{
	if (ke->len == 1) { *scancode = *(const u8 *)ke->scancode; return 0; }
	if (ke->len == 2) { *scancode = *(const u16 *)ke->scancode; return 0; }
	if (ke->len == 4) { *scancode = *(const u32 *)ke->scancode; return 0; }
	return -EINVAL;
}
