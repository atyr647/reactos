// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI part 2, services amdgpu needs on top of the DRM core: kobject/sysfs/debugfs stand-ins, PCI extras,
 * runtime PM, ACPI (nothing present), hrtimers, kthreads, IRQ and firmware bridges, assorted lib helpers.
 * Anything that returns a neutral value is marked "stub"; see README.md for what is real.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/pm_runtime.h>
#include <linux/acpi.h>
#include <acpi/video.h>
#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/irq.h>
#include <linux/firmware.h>
#include <linux/iommu.h>
#include <linux/hwmon.h>
#include <linux/backlight.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/time64.h>
#include <linux/semaphore.h>
#include <linux/rwsem.h>
#include <linux/perf_event.h>
#include <linux/power_supply.h>
#include <linux/vgaarb.h>
#include <linux/dmi.h>
#include <linux/uio.h>
#include <linux/cpuhotplug.h>
#include <linux/mmu_notifier.h>
#include <linux/trace_events.h>
#include <linux/sched.h>
#include <linux/ratelimit.h>
#include <linux/sort.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/pagemap.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/fwnode.h>
#include <asm/hypervisor.h>
#include <asm/processor.h>

extern void nt_dbgprint(const char *s);
extern void nt_sleep_ms(unsigned int ms);
extern unsigned long long nt_ktime_ns(void);
extern int nt_thread_spawn(void (*fn)(void *), void *arg);

/* ---- plain data -------------------------------------------------------------------------- */
unsigned int __num_cores_per_package = 1;
struct cpuinfo_x86 cpu_info;
suspend_state_t pm_suspend_target_state;
enum x86_hypervisor_type x86_hyper_type;
static struct mem_section *kpi_section_roots[8192];
struct mem_section **mem_section = kpi_section_roots;      /* every root NULL: pfn_valid() is always false */
struct acpi_table_fadt acpi_gbl_FADT;
const struct trace_print_flags gfpflag_names[] = { { -1, NULL } };
const struct trace_print_flags pageflag_names[] = { { -1, NULL } };
const struct trace_print_flags vmaflag_names[] = { { -1, NULL } };
const u8 guid_index[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
const u8 uuid_index[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
const struct kernel_param_ops param_ops_bint, param_ops_charp, param_ops_hexint, param_ops_string, param_ops_uint, param_ops_ullong;
const struct sysfs_ops kobj_sysfs_ops;

/* ---- small lib helpers ---------------------------------------------------------------------- */
unsigned long gcd(unsigned long a, unsigned long b)
{
	while (b) { unsigned long t = a % b; a = b; b = t; }
	return a;
}
int get_cmdline(struct task_struct *task, char *buffer, int buflen) { return 0; }
int get_options_dummy(void) { return 0; }
char *get_options(const char *str, int nints, int *ints) { ints[0] = 0; return (char *)str; }
const char *errname(int err) { return NULL; }
void add_taint(unsigned flag, enum lockdep_ok lockdep_ok) { }
void emergency_restart(void) { }
void orderly_poweroff(bool force) { }
void ksys_sync_helper(void) { }
char *d_path(const struct path *path, char *buf, int buflen) { return strncpy(buf, "?", buflen); }
char *file_path(struct file *filp, char *buf, int buflen) { return strncpy(buf, "?", buflen); }
char *mangle_path(char *s, const char *p, const char *esc) { return NULL; }
void seq_hex_dump(struct seq_file *m, const char *prefix_str, int prefix_type, int rowsize, int groupsize, const void *buf, size_t len, bool ascii) { }
const char *trace_print_array_seq(struct trace_seq *p, const void *buf, int count, size_t el_size) { return NULL; }
char *__get_task_comm(char *to, size_t len, struct task_struct *tsk) { strscpy(to, "ReactOS", len); return to; }
void __put_task_struct(struct task_struct *tsk) { }
int __printk_ratelimit(const char *func) { return 1; }
int _printk_deferred(const char *fmt, ...) { return 0; }
void _dev_emerg(const struct device *dev, const char *fmt, ...) { }
void unpin_user_page(struct page *page) { }
void *__kmalloc_large_noprof(size_t size, gfp_t flags) { return __kmalloc_noprof(size, flags); }
void *vmalloc_noprof(unsigned long size) { return __kmalloc_noprof(size, GFP_KERNEL); }
void *kvrealloc_noprof(const void *p, size_t size, gfp_t flags) { return krealloc_noprof(p, size, flags); }
int dmi_get_system_info_dummy;
const char *dmi_get_system_info(int field) { return NULL; }
int __cpuhp_setup_state(enum cpuhp_state state, const char *name, bool invoke, int (*startup)(unsigned int), int (*teardown)(unsigned int), bool multi_instance) { return 0; }
void mmu_notifier_synchronize(void) { }
void rcu_barrier(void) { }
int do_wait_intr(wait_queue_head_t *wq, wait_queue_entry_t *wait) { schedule(); return 0; }
void __wake_up_locked(struct wait_queue_head *wq_head, unsigned int mode, int nr) { __wake_up(wq_head, mode, nr, NULL); }
bool cancel_work(struct work_struct *work) { return cancel_work_sync(work); }
unsigned int jiffies_to_usecs(const unsigned long j) { return j * (1000000 / HZ); }
unsigned long nsecs_to_jiffies(u64 n) { return nsecs_to_jiffies64(n); }
u64 ktime_get_mono_fast_ns(void) { return nt_ktime_ns(); }
time64_t ktime_get_real_seconds(void) { return nt_ktime_ns() / NSEC_PER_SEC; }
ktime_t ktime_get_with_offset(enum tk_offsets offs) { return nt_ktime_ns(); }
void time64_to_tm(time64_t totalsecs, int offset, struct tm *result)
{
	long days = totalsecs / 86400, rem = totalsecs % 86400;

	memset(result, 0, sizeof(*result));
	result->tm_hour = rem / 3600; result->tm_min = (rem % 3600) / 60; result->tm_sec = rem % 60;
	result->tm_wday = (4 + days) % 7;
	{ /* civil from days (Howard Hinnant) */
		long z = days + 719468, era = z / 146097, doe = z - era * 146097;
		long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
		long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
		result->tm_mday = doy - (153 * mp + 2) / 5 + 1;
		result->tm_mon = mp < 10 ? mp + 2 : mp - 10;
		result->tm_year = (result->tm_mon <= 1 ? y + 1 : y) - 1900;
	}
}
ssize_t iov_iter_advance_dummy;
void iov_iter_advance(struct iov_iter *i, size_t size) { }
ssize_t iov_iter_extract_pages(struct iov_iter *i, struct page ***pages, size_t maxsize, unsigned int maxpages, unsigned int extraction_flags, size_t *offset0) { return -ENOSYS; }

/* ---- semaphores and rwsem extras ------------------------------------------------------------- */
void __init_rwsem(struct rw_semaphore *sem, const char *name, struct lock_class_key *key) { atomic_long_set(&sem->count, 0); }
int down_read_trylock(struct rw_semaphore *sem)
{
	long c = atomic_long_read(&sem->count);
	return c >= 0 && atomic_long_cmpxchg(&sem->count, c, c + 1) == c;
}
int down_read_killable(struct rw_semaphore *sem) { down_read(sem); return 0; }
int down_write_killable(struct rw_semaphore *sem) { down_write(sem); return 0; }
void down(struct semaphore *sem)
{
	for (;;) {
		unsigned long flags;
		bool got = false;

		raw_spin_lock_irqsave(&sem->lock, flags);
		if (sem->count > 0) { sem->count--; got = true; }
		raw_spin_unlock_irqrestore(&sem->lock, flags);
		if (got) return;
		nt_sleep_ms(1);
	}
}
void up(struct semaphore *sem)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&sem->lock, flags);
	sem->count++;
	raw_spin_unlock_irqrestore(&sem->lock, flags);
}

/* ---- kobject / kset / sysfs: reference counted, nothing is exported ------------------------- */
void kobject_init(struct kobject *kobj, const struct kobj_type *ktype)
{
	kref_init(&kobj->kref);
	INIT_LIST_HEAD(&kobj->entry);
	kobj->ktype = ktype;
	kobj->state_initialized = 1;
}
int kobject_set_name(struct kobject *kobj, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	kfree_const(kobj->name);
	kobj->name = kvasprintf(GFP_KERNEL, fmt, ap);
	va_end(ap);
	return kobj->name ? 0 : -ENOMEM;
}
int kobject_add(struct kobject *kobj, struct kobject *parent, const char *fmt, ...)
{
	va_list ap;

	kobj->parent = parent;
	if (fmt) {
		va_start(ap, fmt);
		kfree_const(kobj->name);
		kobj->name = kvasprintf(GFP_KERNEL, fmt, ap);
		va_end(ap);
	}
	kobj->state_in_sysfs = 1;
	return 0;
}
int kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype, struct kobject *parent, const char *fmt, ...)
{
	va_list ap;

	kobject_init(kobj, ktype);
	kobj->parent = parent;
	if (fmt) {
		va_start(ap, fmt);
		kobj->name = kvasprintf(GFP_KERNEL, fmt, ap);
		va_end(ap);
	}
	kobj->state_in_sysfs = 1;
	return 0;
}
struct kobject *kobject_get(struct kobject *kobj) { if (kobj) kref_get(&kobj->kref); return kobj; }
static void kpi_kobject_release(struct kref *kref)
{
	struct kobject *kobj = container_of(kref, struct kobject, kref);

	if (kobj->ktype && kobj->ktype->release)
		kobj->ktype->release(kobj);
}
void kobject_put(struct kobject *kobj) { if (kobj) kref_put(&kobj->kref, kpi_kobject_release); }
int kset_register(struct kset *k) { kobject_init(&k->kobj, NULL); return 0; }
int sysfs_create_group(struct kobject *kobj, const struct attribute_group *grp) { return 0; }
int sysfs_create_file_ns(struct kobject *kobj, const struct attribute *attr, const void *ns) { return 0; }
int sysfs_create_files(struct kobject *kobj, const struct attribute * const *attr) { return 0; }
int sysfs_create_bin_file(struct kobject *kobj, const struct bin_attribute *attr) { return 0; }
int sysfs_add_file_to_group(struct kobject *kobj, const struct attribute *attr, const char *group) { return 0; }
void sysfs_remove_group(struct kobject *kobj, const struct attribute_group *grp) { }
void sysfs_remove_file_ns(struct kobject *kobj, const struct attribute *attr, const void *ns) { }
void sysfs_remove_files(struct kobject *kobj, const struct attribute * const *attr) { }
void sysfs_remove_bin_file(struct kobject *kobj, const struct bin_attribute *attr) { }
void sysfs_remove_file_from_group(struct kobject *kobj, const struct attribute *attr, const char *group) { }
int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vscnprintf(buf + at, PAGE_SIZE - at, fmt, ap);
	va_end(ap);
	return n;
}
int device_create_file(struct device *dev, const struct device_attribute *attr) { return 0; }
void device_remove_file(struct device *dev, const struct device_attribute *attr) { }
int devm_device_add_group(struct device *dev, const struct attribute_group *grp) { return 0; }

/* ---- debugfs / simple_attr / fwnode ------------------------------------------------------- */
bool debugfs_initialized(void) { return false; }
extern struct dentry *debugfs_create_dir(const char *name, struct dentry *parent);
void debugfs_create_u32(const char *name, umode_t mode, struct dentry *parent, u32 *value) { }
void debugfs_create_x32(const char *name, umode_t mode, struct dentry *parent, u32 *value) { }
void debugfs_create_bool(const char *name, umode_t mode, struct dentry *parent, bool *value) { }
void debugfs_create_file_size(const char *name, umode_t mode, struct dentry *parent, void *data, const struct file_operations *fops, loff_t file_size) { }
struct dentry *debugfs_create_blob(const char *name, umode_t mode, struct dentry *parent, struct debugfs_blob_wrapper *blob) { return debugfs_create_dir(name, parent); }
ssize_t debugfs_attr_read(struct file *file, char __user *buf, size_t len, loff_t *ppos) { return -ENOSYS; }
ssize_t debugfs_attr_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos) { return -ENOSYS; }
ssize_t debugfs_attr_write_signed(struct file *file, const char __user *buf, size_t len, loff_t *ppos) { return -ENOSYS; }
loff_t default_llseek(struct file *file, loff_t offset, int whence) { return -ESPIPE; }
int simple_attr_open(struct inode *inode, struct file *file, int (*get)(void *, u64 *), int (*set)(void *, u64), const char *fmt) { return -ENOSYS; }
int simple_attr_release(struct inode *inode, struct file *file) { return 0; }
unsigned int fwnode_count_parents(const struct fwnode_handle *fwnode) { return 0; }
const char *fwnode_get_name(const struct fwnode_handle *fwnode) { return NULL; }
const char *fwnode_get_name_prefix(const struct fwnode_handle *fwnode) { return ""; }
struct fwnode_handle *fwnode_get_nth_parent(struct fwnode_handle *fwnode, unsigned int depth) { return NULL; }

/* ---- runtime PM: the device is always "active" -------------------------------------------- */
void __pm_runtime_disable(struct device *dev, bool check_resume) { }
int __pm_runtime_resume(struct device *dev, int rpmflags) { return 0; }
int __pm_runtime_suspend(struct device *dev, int rpmflags) { return 0; }
void __pm_runtime_use_autosuspend(struct device *dev, bool use) { }
void pm_runtime_allow(struct device *dev) { }
void pm_runtime_forbid(struct device *dev) { }
void pm_runtime_enable(struct device *dev) { }
void pm_runtime_set_autosuspend_delay(struct device *dev, int delay) { }
u64 pm_runtime_autosuspend_expiration(struct device *dev) { return 0; }
int power_supply_is_system_supplied(void) { return 1; }

/* ---- ACPI: no firmware tables ------------------------------------------------------------------ */
const char *acpi_format_exception(acpi_status status) { return "AE_NOT_FOUND"; }
acpi_status acpi_get_handle(acpi_handle parent, const char *pathname, acpi_handle *ret_handle) { return AE_NOT_FOUND; }
acpi_status acpi_get_name(acpi_handle handle, u32 name_type, struct acpi_buffer *buffer) { return AE_NOT_FOUND; }
acpi_status acpi_get_table(char *signature, u32 instance, struct acpi_table_header **out_table) { return AE_NOT_FOUND; }
acpi_status acpi_evaluate_object(acpi_handle handle, acpi_string pathname, struct acpi_object_list *args, struct acpi_buffer *buffer) { return AE_NOT_FOUND; }
union acpi_object *acpi_evaluate_dsm(acpi_handle handle, const guid_t *guid, u64 rev, u64 func, union acpi_object *argv4) { return NULL; }
struct acpi_device *acpi_dev_get_first_match_dev(const char *hid, const char *uid, s64 hrv) { return NULL; }
int register_acpi_notifier(struct notifier_block *nb) { return 0; }
int unregister_acpi_notifier(struct notifier_block *nb) { return 0; }
void acpi_video_register_backlight(void) { }
enum acpi_backlight_type __acpi_video_get_backlight_type(bool native, bool *auto_detect) { if (auto_detect) *auto_detect = false; return acpi_backlight_none; }
struct backlight_device *backlight_device_register(const char *name, struct device *dev, void *devdata, const struct backlight_ops *ops, const struct backlight_properties *props) { return ERR_PTR(-ENODEV); }
void backlight_device_unregister(struct backlight_device *bd) { }
int backlight_device_set_brightness(struct backlight_device *bd, unsigned long brightness) { return -ENODEV; }
struct device *hwmon_device_register_with_groups(struct device *dev, const char *name, void *drvdata, const struct attribute_group **groups)
{
	struct device *hd = kzalloc(sizeof(*hd), GFP_KERNEL);   /* stub: hwmon is not exported */
	if (hd) device_initialize(hd);
	return hd ? hd : ERR_PTR(-ENOMEM);
}
void hwmon_device_unregister(struct device *dev) { kfree(dev); }
int i2c_add_adapter(struct i2c_adapter *adap) { return 0; }
int i2c_bit_add_bus(struct i2c_adapter *adap) { return 0; }
void i2c_del_adapter(struct i2c_adapter *adap) { }
int perf_pmu_register(struct pmu *pmu, const char *name, int type) { return 0; }
void perf_pmu_unregister(struct pmu *pmu) { }
void perf_event_update_userpage(struct perf_event *event) { }
int vga_client_register(struct pci_dev *pdev, unsigned int (*set_decode)(struct pci_dev *pdev, bool decode)) { return 0; }
void arch_io_free_memtype_wc(resource_size_t start, resource_size_t size) { }
int arch_io_reserve_memtype_wc(resource_size_t start, resource_size_t size) { return 0; }
int arch_phys_wc_add(unsigned long base, unsigned long size) { return 0; }
void arch_phys_wc_del(int handle) { }
struct iommu_domain *iommu_get_domain_for_dev(struct device *dev) { return NULL; }
phys_addr_t iommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova) { return iova; }

/* ---- DMA/dma-buf extras ----------------------------------------------------------------- */
bool dma_addressing_limited(struct device *dev) { return false; }
int dma_set_mask(struct device *dev, u64 mask) { return 0; }
int dma_set_coherent_mask(struct device *dev, u64 mask) { return 0; }
dma_addr_t dma_map_resource(struct device *dev, phys_addr_t phys_addr, size_t size, enum dma_data_direction dir, unsigned long attrs) { return phys_addr; }
void dma_unmap_resource(struct device *dev, dma_addr_t addr, size_t size, enum dma_data_direction dir, unsigned long attrs) { }
struct dma_buf_attachment *dma_buf_dynamic_attach(struct dma_buf *dmabuf, struct device *dev, const struct dma_buf_attach_ops *importer_ops, void *importer_priv) { return ERR_PTR(-ENOSYS); }
struct sg_table *dma_buf_map_attachment(struct dma_buf_attachment *attach, enum dma_data_direction dir) { return ERR_PTR(-ENOSYS); }
void dma_buf_unmap_attachment(struct dma_buf_attachment *attach, struct sg_table *sg, enum dma_data_direction dir) { }
void dma_buf_move_notify(struct dma_buf *dmabuf) { }
int dma_buf_pin(struct dma_buf_attachment *attach) { return -ENOSYS; }
void dma_buf_unpin(struct dma_buf_attachment *attach) { }

/* ---- PCI extras ------------------------------------------------------------------------------ */
static int kpi_pcie_cap(struct pci_dev *dev) { return pci_find_capability(dev, PCI_CAP_ID_EXP); }
u16 pci_find_ext_capability(struct pci_dev *dev, int cap)
{
	int pos = 0x100, guard = 64;
	u32 hdr;

	while (pos >= 0x100 && guard--) {
		if (pci_read_config_dword(dev, pos, &hdr) || !hdr || hdr == ~0u) return 0;
		if (PCI_EXT_CAP_ID(hdr) == cap) return pos;
		pos = PCI_EXT_CAP_NEXT(hdr);
	}
	return 0;
}
int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{
	int base = kpi_pcie_cap(dev);
	*val = 0;
	if (!base) return PCIBIOS_FUNC_NOT_SUPPORTED;
	return pci_read_config_word(dev, base + pos, val);
}
int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val)
{
	int base = kpi_pcie_cap(dev);
	*val = 0;
	if (!base) return PCIBIOS_FUNC_NOT_SUPPORTED;
	return pci_read_config_dword(dev, base + pos, val);
}
int pcie_capability_clear_and_set_word_unlocked(struct pci_dev *dev, int pos, u16 clear, u16 set)
{
	u16 v;
	int base = kpi_pcie_cap(dev);

	if (!base) return PCIBIOS_FUNC_NOT_SUPPORTED;
	pci_read_config_word(dev, base + pos, &v);
	v = (v & ~clear) | set;
	return pci_write_config_word(dev, base + pos, v);
}
int pcie_capability_clear_and_set_word_locked(struct pci_dev *dev, int pos, u16 clear, u16 set) { return pcie_capability_clear_and_set_word_unlocked(dev, pos, clear, set); }
int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags) { return 1; }
void pci_free_irq_vectors(struct pci_dev *dev) { }
int pci_irq_vector(struct pci_dev *dev, unsigned int nr) { return dev->irq; }
void pci_assign_unassigned_bus_resources(struct pci_bus *bus) { }
struct resource *pci_bus_resource_n(const struct pci_bus *bus, int n) { return NULL; }
bool pci_device_is_present(struct pci_dev *pdev) { u16 v; return !pci_read_config_word(pdev, 0, &v) && v != 0xffff; }
int pci_enable_atomic_ops_to_root(struct pci_dev *dev, u32 cap_mask) { return -EINVAL; }
struct pci_dev *pci_get_base_class(unsigned int class, struct pci_dev *from) { return NULL; }
struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn) { return NULL; }
void pci_ignore_hotplug(struct pci_dev *dev) { }
int pci_load_saved_state(struct pci_dev *dev, struct pci_saved_state *state) { return 0; }
void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size) { *size = 0; return NULL; }
void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom) { }
bool pci_pr3_present(struct pci_dev *pdev) { return false; }
u32 pci_rebar_get_possible_sizes(struct pci_dev *pdev, int bar) { return 0; }
void pci_release_resource(struct pci_dev *dev, int resno) { }
int pci_reset_function(struct pci_dev *dev) { return -ENOTSUPP; }
int pci_resize_resource(struct pci_dev *dev, int resno, int size) { return -ENOTSUPP; }
void pci_restore_msi_state(struct pci_dev *dev) { }
void pci_restore_state(struct pci_dev *dev) { }
int pci_save_state(struct pci_dev *dev) { return 0; }
int pci_set_power_state(struct pci_dev *dev, pci_power_t state) { return 0; }
struct pci_saved_state *pci_store_saved_state(struct pci_dev *dev) { return NULL; }
int pci_wait_for_pending_transaction(struct pci_dev *dev) { return 1; }
int pci_wake_from_d3(struct pci_dev *dev, bool enable) { return 0; }
bool pcie_aspm_enabled(struct pci_dev *pdev) { return false; }
u32 pcie_bandwidth_available(struct pci_dev *dev, struct pci_dev **limiting_dev, enum pci_bus_speed *speed, enum pcie_link_width *width)
{
	if (speed) *speed = PCIE_SPEED_16_0GT;
	if (width) *width = PCIE_LNK_X16;
	return 252000;
}
int pcie_get_mps(struct pci_dev *dev) { return 128; }
int pcie_get_readrq(struct pci_dev *dev) { return 512; }
int pcie_set_readrq(struct pci_dev *dev, int rq) { return 0; }
enum pci_bus_speed pcie_get_speed_cap(struct pci_dev *dev) { return PCIE_SPEED_16_0GT; }
enum pcie_link_width pcie_get_width_cap(struct pci_dev *dev) { return PCIE_LNK_X16; }

/* ---- IRQ domain stubs (amdgpu's own interrupt controller emulation) ------------------------------- */
void handle_simple_irq(struct irq_desc *desc) { }
int generic_handle_domain_irq(struct irq_domain *domain, unsigned int hwirq) { return 0; }
unsigned int irq_create_mapping_affinity(struct irq_domain *host, irq_hw_number_t hwirq, const struct irq_affinity_desc *affinity) { return hwirq; }
struct irq_domain *irq_domain_instantiate(const struct irq_domain_info *info) { return ERR_PTR(-ENOSYS); }
void irq_domain_remove(struct irq_domain *domain) { }
void irq_set_chip_and_handler_name(unsigned int irq, const struct irq_chip *chip, irq_flow_handler_t handle, const char *name) { }

/* ---- hrtimers: polled by one thread at 1 ms resolution --------------------------------------- */
struct kpi_hrt { struct list_head entry; struct hrtimer *timer; u64 expires; };
static LIST_HEAD(kpi_hrtimers);
static DEFINE_SPINLOCK(kpi_hrt_lock);
static int kpi_hrt_started;

static void kpi_hrt_thread(void *arg)
{
	for (;;) {
		struct kpi_hrt *fire = NULL, *h;
		unsigned long flags;

		spin_lock_irqsave(&kpi_hrt_lock, flags);
		list_for_each_entry(h, &kpi_hrtimers, entry) {
			if (nt_ktime_ns() >= h->expires) { list_del(&h->entry); fire = h; break; }
		}
		spin_unlock_irqrestore(&kpi_hrt_lock, flags);
		if (fire) {
			struct hrtimer *t = fire->timer;
			enum hrtimer_restart r;

			t->state = 0;
			kfree(fire);
			r = t->function(t);
			if (r == HRTIMER_RESTART) {
				struct kpi_hrt *n = kzalloc(sizeof(*n), GFP_ATOMIC);
				if (n) {
					n->timer = t;
					n->expires = ktime_to_ns(hrtimer_get_expires(t));
					spin_lock_irqsave(&kpi_hrt_lock, flags);
					list_add(&n->entry, &kpi_hrtimers);
					t->state = 1;
					spin_unlock_irqrestore(&kpi_hrt_lock, flags);
				}
			}
		} else {
			nt_sleep_ms(1);
		}
	}
}
void hrtimer_init(struct hrtimer *timer, clockid_t clock_id, enum hrtimer_mode mode) { memset(timer, 0, sizeof(*timer)); }
void hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim, u64 delta_ns, const enum hrtimer_mode mode)
{
	struct kpi_hrt *h = kzalloc(sizeof(*h), GFP_ATOMIC);
	unsigned long flags;

	if (!kpi_hrt_started) { kpi_hrt_started = 1; nt_thread_spawn(kpi_hrt_thread, NULL); }
	if (!h) return;
	h->timer = timer;
	h->expires = (mode & HRTIMER_MODE_REL) ? nt_ktime_ns() + tim : tim;
	hrtimer_set_expires(timer, h->expires);
	spin_lock_irqsave(&kpi_hrt_lock, flags);
	list_add(&h->entry, &kpi_hrtimers);
	timer->state = 1;
	spin_unlock_irqrestore(&kpi_hrt_lock, flags);
}
int hrtimer_try_to_cancel(struct hrtimer *timer)
{
	struct kpi_hrt *h, *tmp;
	unsigned long flags;
	int active = 0;

	spin_lock_irqsave(&kpi_hrt_lock, flags);
	list_for_each_entry_safe(h, tmp, &kpi_hrtimers, entry) {
		if (h->timer == timer) { list_del(&h->entry); kfree(h); active = 1; }
	}
	timer->state = 0;
	spin_unlock_irqrestore(&kpi_hrt_lock, flags);
	return active;
}
int hrtimer_cancel(struct hrtimer *timer) { return hrtimer_try_to_cancel(timer); }
u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval)
{
	u64 orun = 1;
	ktime_t delta = now - hrtimer_get_expires(timer);

	if (delta < 0) return 0;
	if (delta >= interval) {
		orun = (delta / interval) + 1;
		hrtimer_add_expires_ns(timer, interval * orun);
	} else {
		hrtimer_add_expires_ns(timer, interval);
	}
	return orun;
}

/* ---- kthreads on NT system threads --------------------------------------------------------------- */
struct kpi_kthread { struct task_struct *task; int (*fn)(void *); void *data; volatile int stop, done; int result; };
static void kpi_kthread_main(void *arg)
{
	struct kpi_kthread *k = arg;
	k->result = k->fn(k->data);
	k->done = 1;
}
struct task_struct *kthread_create_on_node(int (*threadfn)(void *data), void *data, int node, const char namefmt[], ...)
{
	/* returns a task_struct whose "private" pointer is our record; the thread starts on wake_up_process() */
	struct task_struct *t = kzalloc(sizeof(*t), GFP_KERNEL);
	struct kpi_kthread *k = kzalloc(sizeof(*k), GFP_KERNEL);

	if (!t || !k) { kfree(t); kfree(k); return ERR_PTR(-ENOMEM); }
	k->task = t; k->fn = threadfn; k->data = data;
	t->worker_private = k;
	nt_thread_spawn(kpi_kthread_main, k);
	return t;
}
bool kthread_should_stop(void) { return false; }
int kthread_stop(struct task_struct *k)
{
	struct kpi_kthread *kt = k->worker_private;
	int r;

	kt->stop = 1;
	while (!kt->done) nt_sleep_ms(1);
	r = kt->result;
	kfree(kt); kfree(k);
	return r;
}

/* ---- last few ----------------------------------------------------------------------------- */
#include <linux/backlight.h>
#include <linux/random.h>
#include <linux/uuid.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
struct backlight_device *devm_backlight_device_register(struct device *dev, const char *name, struct device *parent, void *devdata, const struct backlight_ops *ops, const struct backlight_properties *props) { return ERR_PTR(-ENODEV); }
void *devres_open_group(struct device *dev, void *id, gfp_t gfp) { return id ? id : (void *)1; }
int devres_release_group(struct device *dev, void *id) { return 0; }
void get_random_bytes(void *buf, size_t len)
{
	static u64 s = 0x9E3779B97F4A7C15ULL;
	u8 *p = buf;
	while (len-- > 0) { s ^= nt_ktime_ns(); s = s * 6364136223846793005ULL + 1442695040888963407ULL; *p++ = s >> 33; }
}
const guid_t guid_null;
void guid_gen(guid_t *lu)
{
	get_random_bytes(lu, 16);
	lu->b[7] = (lu->b[7] & 0x0F) | 0x40;
	lu->b[8] = (lu->b[8] & 0x3F) | 0x80;
}
struct platform_device *platform_device_register_full(const struct platform_device_info *pdevinfo)
{
	struct platform_device *pd = kzalloc(sizeof(*pd), GFP_KERNEL);

	if (!pd) return ERR_PTR(-ENOMEM);
	pd->name = pdevinfo->name;
	pd->id = pdevinfo->id;
	device_initialize(&pd->dev);
	pd->dev.parent = pdevinfo->parent;
	return pd;
}
void platform_device_unregister(struct platform_device *pdev) { if (pdev) put_device(&pdev->dev); }
int request_firmware_direct(const struct firmware **fw, const char *name, struct device *device) { return request_firmware(fw, name, device); }
