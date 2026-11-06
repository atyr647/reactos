// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI for ReactOS: core services (locks, memory, time, printk, waiting).
 *
 * This file is compiled as a *Linux module* against the Linux headers, so every structure it touches
 * has the exact layout the (unmodified) driver modules expect. It reaches the ReactOS kernel only
 * through the nt_* bridge functions exported by linuxkpi.sys.
 *
 * Execution model for now: the Linux code is treated as uniprocessor (CONFIG_SMP=n). Locks are real
 * (atomic) so ReactOS' SMP scheduling cannot break them. Sleeping is polling based and every wait
 * loop in Linux re-checks its condition, so spurious wakeups are fine.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/rwlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/uaccess.h>
#include <linux/completion.h>
#include <linux/string.h>
#include <linux/percpu.h>
#include <asm/current.h>
#include <linux/list_lru.h>

/* ---- bridge to linuxkpi.sys (implemented in the ReactOS driver) --------------------------- */
extern void *nt_alloc(unsigned long size, int zero);
extern void nt_free(void *p);
extern unsigned char nt_raise_dispatch(void);   /* returns the previous IRQL */
extern void nt_lower_irql(unsigned char old);
extern void nt_sleep_ms(unsigned int ms);       /* 0 = yield */
extern void nt_stall_us(unsigned int us);
extern unsigned long long nt_time_ms(void);     /* monotonic milliseconds */
extern void nt_dbgprint(const char *s);
extern void nt_register_tick(void (*fn)(void));

/* ---- time ------------------------------------------------------------------------------ */
unsigned long volatile jiffies;
u64 jiffies_64;

static void kpi_tick(void)
{
	jiffies_64 = nt_time_ms() * HZ / 1000;
	jiffies = (unsigned long)jiffies_64;
}

unsigned long __msecs_to_jiffies(const unsigned int m)
{
	return (m * HZ + 999) / 1000;
}

unsigned int jiffies_to_msecs(const unsigned long j)
{
	return (unsigned int)(j * 1000 / HZ);
}

void msleep(unsigned int msecs) { nt_sleep_ms(msecs ? msecs : 1); }
unsigned long msleep_interruptible(unsigned int msecs) { nt_sleep_ms(msecs ? msecs : 1); return 0; }
void usleep_range_state(unsigned long min, unsigned long max, unsigned int state) { nt_sleep_ms(min / 1000 ? min / 1000 : 1); }
void __const_udelay(unsigned long xloops) { nt_stall_us((unsigned int)(xloops / 4295 + 1)); }
void __udelay(unsigned long usecs) { nt_stall_us((unsigned int)usecs); }
void __ndelay(unsigned long nsecs) { nt_stall_us((unsigned int)(nsecs / 1000 + 1)); }
EXPORT_SYMBOL(jiffies);
EXPORT_SYMBOL(jiffies_64);

/* ---- printk ---------------------------------------------------------------------------- */
int vprintk(const char *fmt, va_list args)
{
	char buf[512];
	const char *f = fmt;
	int n;

	if (f[0] == '\001' && f[1])
		f += 2;                       /* KERN_<level> */
	n = vscnprintf(buf, sizeof(buf), f, args);
	nt_dbgprint(buf);
	return n;
}

int _printk(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vprintk(fmt, ap);
	va_end(ap);
	return n;
}

void __warn_printk(const char *fmt, ...)
{
	va_list ap;

	nt_dbgprint("WARNING: ");
	va_start(ap, fmt);
	vprintk(fmt, ap);
	va_end(ap);
}

/* ---- memory ---------------------------------------------------------------------------- */
void *__kmalloc_noprof(size_t size, gfp_t flags)
{
	return nt_alloc(size, !!(flags & __GFP_ZERO));
}

void *__kmalloc_node_track_caller_noprof(size_t size, gfp_t flags, int node, unsigned long caller)
{
	return nt_alloc(size, !!(flags & __GFP_ZERO));
}

void *__kvmalloc_node_noprof(size_t size, gfp_t flags, int node)
{
	return nt_alloc(size, !!(flags & __GFP_ZERO));
}

void *__vmalloc_noprof(unsigned long size, gfp_t flags)
{
	return nt_alloc(size, !!(flags & __GFP_ZERO));
}

void *vzalloc_noprof(unsigned long size) { return nt_alloc(size, 1); }
void kfree(const void *p) { if (p) nt_free((void *)p); }
void kvfree(const void *p) { if (p) nt_free((void *)p); }
void vfree(const void *p) { if (p) nt_free((void *)p); }
void kfree_sensitive(const void *p) { if (p) nt_free((void *)p); }

/* ---- uaccess: user and kernel space are the same for now ---------------------------------- */
unsigned long _copy_from_user(void *to, const void __user *from, unsigned long n) { memcpy(to, (const void __force *)from, n); return 0; }
unsigned long _copy_to_user(void __user *to, const void *from, unsigned long n) { memcpy((void __force *)to, from, n); return 0; }
void __copy_overflow(int size, unsigned long count) { nt_dbgprint("copy_overflow\n"); }

/* ---- spinlocks -------------------------------------------------------------------------- */
/* slock: 1 = unlocked, 0 = locked (see spinlock_types_up.h). The previous IRQL is kept in owner_cpu. */
void __raw_spin_lock_init(raw_spinlock_t *lock, const char *name, struct lock_class_key *key, short inner)
{
	lock->raw_lock.slock = 1;
	lock->magic = SPINLOCK_MAGIC;
	lock->owner_cpu = -1;
	lock->owner = SPINLOCK_OWNER_INIT;
}

static inline void kpi_spin_acquire(raw_spinlock_t *lock)
{
	unsigned char old = nt_raise_dispatch();

	while (__sync_lock_test_and_set(&lock->raw_lock.slock, 0) == 0)
		while (lock->raw_lock.slock == 0)
			__builtin_ia32_pause();
	lock->owner_cpu = old;
}

static inline void kpi_spin_release(raw_spinlock_t *lock)
{
	unsigned char old = (unsigned char)lock->owner_cpu;

	__sync_synchronize();
	lock->raw_lock.slock = 1;
	nt_lower_irql(old);
}

void _raw_spin_lock(raw_spinlock_t *lock) { kpi_spin_acquire(lock); }
void _raw_spin_unlock(raw_spinlock_t *lock) { kpi_spin_release(lock); }
/* The _irq/_irqsave variants must really mask interrupts: a device ISR (xHCI, ...) takes the same
 * locks and would otherwise spin forever on the CPU that interrupted the lock holder. */
static inline unsigned long kpi_irq_save(void)
{
	unsigned long f;
	asm volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
	return f;
}
static inline void kpi_irq_restore(unsigned long f)
{
	if (f & 0x200)
		asm volatile("sti" ::: "memory");
}
void _raw_spin_lock_irq(raw_spinlock_t *lock) { kpi_irq_save(); kpi_spin_acquire(lock); }
void _raw_spin_unlock_irq(raw_spinlock_t *lock) { kpi_spin_release(lock); asm volatile("sti" ::: "memory"); }
void _raw_spin_lock_bh(raw_spinlock_t *lock) { kpi_spin_acquire(lock); }
void _raw_spin_unlock_bh(raw_spinlock_t *lock) { kpi_spin_release(lock); }
unsigned long _raw_spin_lock_irqsave(raw_spinlock_t *lock) { unsigned long f = kpi_irq_save(); kpi_spin_acquire(lock); return f; }
void _raw_spin_unlock_irqrestore(raw_spinlock_t *lock, unsigned long flags) { kpi_spin_release(lock); kpi_irq_restore(flags); }

int _raw_spin_trylock(raw_spinlock_t *lock)
{
	unsigned char old = nt_raise_dispatch();

	if (__sync_lock_test_and_set(&lock->raw_lock.slock, 0) == 0) {
		nt_lower_irql(old);
		return 0;
	}
	lock->owner_cpu = old;
	return 1;
}

EXPORT_SYMBOL(_raw_spin_lock);
EXPORT_SYMBOL(_raw_spin_unlock);
EXPORT_SYMBOL(_raw_spin_lock_irq);
EXPORT_SYMBOL(_raw_spin_unlock_irq);
EXPORT_SYMBOL(_raw_spin_lock_bh);
EXPORT_SYMBOL(_raw_spin_unlock_bh);
EXPORT_SYMBOL(_raw_spin_lock_irqsave);
EXPORT_SYMBOL(_raw_spin_unlock_irqrestore);
EXPORT_SYMBOL(_raw_spin_trylock);
EXPORT_SYMBOL(__raw_spin_lock_init);

/* rwlock: owner_cpu = number of readers, or 0x80000000 when a writer holds it (arch_rwlock_t is empty on UP) */
void __rwlock_init(rwlock_t *lock, const char *name, struct lock_class_key *key)
{
	lock->magic = RWLOCK_MAGIC;
	lock->owner_cpu = 0;
	lock->owner = SPINLOCK_OWNER_INIT;
}

#define RW_WRITER 0x80000000u
void _raw_read_lock(rwlock_t *lock)
{
	for (;;) {
		unsigned int v = lock->owner_cpu;
		if (!(v & RW_WRITER) && __sync_bool_compare_and_swap(&lock->owner_cpu, v, v + 1))
			return;
		__builtin_ia32_pause();
	}
}
void _raw_read_unlock(rwlock_t *lock) { __sync_fetch_and_sub(&lock->owner_cpu, 1); }
void _raw_write_lock(rwlock_t *lock)
{
	while (!__sync_bool_compare_and_swap(&lock->owner_cpu, 0, RW_WRITER))
		__builtin_ia32_pause();
}
void _raw_write_unlock(rwlock_t *lock) { __sync_synchronize(); lock->owner_cpu = 0; }
EXPORT_SYMBOL(__rwlock_init);
EXPORT_SYMBOL(_raw_read_lock);
EXPORT_SYMBOL(_raw_read_unlock);
EXPORT_SYMBOL(_raw_write_lock);
EXPORT_SYMBOL(_raw_write_unlock);

/* ---- scheduling / waiting (polling) ------------------------------------------------------ */
void schedule(void) { nt_sleep_ms(0); }
signed long schedule_timeout(signed long timeout)
{
	unsigned long end = jiffies + (timeout > 0 ? timeout : 0);

	if (timeout == MAX_SCHEDULE_TIMEOUT) { nt_sleep_ms(1); return MAX_SCHEDULE_TIMEOUT; }
	nt_sleep_ms(1);
	kpi_tick();
	return time_before(jiffies, end) ? (long)(end - jiffies) : 0;
}
int __cond_resched(void) { nt_sleep_ms(0); return 0; }
EXPORT_SYMBOL(schedule);
EXPORT_SYMBOL(schedule_timeout);

void __init_waitqueue_head(struct wait_queue_head *wq_head, const char *name, struct lock_class_key *key)
{
	spin_lock_init(&wq_head->lock);
	INIT_LIST_HEAD(&wq_head->head);
}

int __wake_up(struct wait_queue_head *wq_head, unsigned int mode, int nr_exclusive, void *key)
{
	struct wait_queue_entry *curr, *next;

	list_for_each_entry_safe(curr, next, &wq_head->head, entry)
		if (curr->func)
			curr->func(curr, mode, 0, key);
	return 1;
}

int default_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key) { return 1; }
int autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key)
{
	list_del_init(&wq_entry->entry);
	return 1;
}

void add_wait_queue(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
	unsigned long flags;
	spin_lock_irqsave(&wq_head->lock, flags);
	list_add(&wq_entry->entry, &wq_head->head);
	spin_unlock_irqrestore(&wq_head->lock, flags);
}

void remove_wait_queue(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
	unsigned long flags;
	spin_lock_irqsave(&wq_head->lock, flags);
	list_del(&wq_entry->entry);
	spin_unlock_irqrestore(&wq_head->lock, flags);
}

void prepare_to_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state)
{
	unsigned long flags;
	wq_entry->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq_head->lock, flags);
	if (list_empty(&wq_entry->entry))
		list_add(&wq_entry->entry, &wq_head->head);
	spin_unlock_irqrestore(&wq_head->lock, flags);
}

long prepare_to_wait_event(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state)
{
	prepare_to_wait(wq_head, wq_entry, state);
	return 0;
}

void finish_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
	unsigned long flags;
	spin_lock_irqsave(&wq_head->lock, flags);
	if (!list_empty(&wq_entry->entry))
		list_del_init(&wq_entry->entry);
	spin_unlock_irqrestore(&wq_head->lock, flags);
}
EXPORT_SYMBOL(__init_waitqueue_head);
EXPORT_SYMBOL(__wake_up);
EXPORT_SYMBOL(add_wait_queue);
EXPORT_SYMBOL(remove_wait_queue);
EXPORT_SYMBOL(prepare_to_wait);
EXPORT_SYMBOL(prepare_to_wait_event);
EXPORT_SYMBOL(finish_wait);
EXPORT_SYMBOL(default_wake_function);
EXPORT_SYMBOL(autoremove_wake_function);

/* completions */
void complete(struct completion *x) { __sync_fetch_and_add(&x->done, 1); }
void complete_all(struct completion *x) { x->done = 0x7fffffff; }
void wait_for_completion(struct completion *x)
{
	unsigned int spins = 0;

	while (x->done == 0) {
		nt_sleep_ms(1);
		if (++spins == 3000) {
			char b[96];
			scnprintf(b, sizeof(b), "LKPI: wait_for_completion stuck 3s, caller %px", __builtin_return_address(0));
			nt_dbgprint(b);
		}
	}
	if (x->done != 0x7fffffff)
		__sync_fetch_and_sub(&x->done, 1);
}
unsigned long wait_for_completion_timeout(struct completion *x, unsigned long timeout)
{
	unsigned long end = jiffies + timeout;

	while (x->done == 0) {
		nt_sleep_ms(1);
		kpi_tick();
		if (!time_before(jiffies, end))
			return 0;
	}
	if (x->done != 0x7fffffff)
		__sync_fetch_and_sub(&x->done, 1);
	return 1;
}
EXPORT_SYMBOL(complete);
EXPORT_SYMBOL(complete_all);
EXPORT_SYMBOL(wait_for_completion);
EXPORT_SYMBOL(wait_for_completion_timeout);

/* ---- mutex ------------------------------------------------------------------------------ */
void __mutex_init(struct mutex *lock, const char *name, struct lock_class_key *key)
{
	atomic_long_set(&lock->owner, 0);
	raw_spin_lock_init(&lock->wait_lock);
	INIT_LIST_HEAD(&lock->wait_list);
#ifdef CONFIG_DEBUG_MUTEXES
	lock->magic = lock;
#endif
}

static inline unsigned long kpi_self(void)
{
	/* no per-thread task_struct yet: any non-zero token works for ownership tracking */
	return 1;
}

int mutex_trylock(struct mutex *lock)
{
	return atomic_long_cmpxchg(&lock->owner, 0, kpi_self()) == 0;
}

void mutex_lock(struct mutex *lock)
{
	unsigned long spins = 0;

	while (atomic_long_cmpxchg(&lock->owner, 0, kpi_self()) != 0) {
		nt_sleep_ms(0);
		if (++spins == 3000000) {
			char b[128];
			scnprintf(b, sizeof(b), "LKPI: mutex_lock stuck, lock %px owner %lx self %lx caller %px", lock, (unsigned long)atomic_long_read(&lock->owner), (unsigned long)kpi_self(), __builtin_return_address(0));
			nt_dbgprint(b);
		}
	}
}

int mutex_lock_interruptible(struct mutex *lock) { mutex_lock(lock); return 0; }
int mutex_lock_killable(struct mutex *lock) { mutex_lock(lock); return 0; }
void mutex_unlock(struct mutex *lock) { __sync_synchronize(); atomic_long_set(&lock->owner, 0); }
int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
	if (atomic_add_unless(cnt, -1, 1))
		return 0;
	mutex_lock(lock);
	if (!atomic_dec_and_test(cnt)) {
		mutex_unlock(lock);
		return 0;
	}
	return 1;
}
EXPORT_SYMBOL(__mutex_init);
EXPORT_SYMBOL(mutex_trylock);
EXPORT_SYMBOL(mutex_lock);
EXPORT_SYMBOL(mutex_lock_interruptible);
EXPORT_SYMBOL(mutex_lock_killable);
EXPORT_SYMBOL(mutex_unlock);
EXPORT_SYMBOL(atomic_dec_and_mutex_lock);


/* ---- libc-ish pieces that the kernel gets from arch code ----------------------------------- */
void *memcpy(void *d, const void *s, size_t n)
{
	void *r = d;
	asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	return r;
}
void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *dp = d; const unsigned char *sp = s;

	if (dp == sp || !n) return d;
	if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
	while (n--) dp[n] = sp[n];
	return d;
}
void *memset(void *d, int c, size_t n)
{
	void *r = d;
	asm volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
	return r;
}
EXPORT_SYMBOL(memcpy); EXPORT_SYMBOL(memmove); EXPORT_SYMBOL(memset);

/* ---- slab ----------------------------------------------------------------------------------- */
struct kmem_cache { unsigned int size; const char *name; };
static struct kmem_cache kpi_dummy_cache;
struct kmem_cache *kmalloc_caches[NR_KMALLOC_TYPES][KMALLOC_SHIFT_HIGH + 1] = { { &kpi_dummy_cache } };

void *__kmalloc_cache_noprof(struct kmem_cache *s, gfp_t flags, size_t size) { return nt_alloc(size, !!(flags & __GFP_ZERO)); }
void *__kmalloc_node_noprof(size_t size, gfp_t flags, int node) { return nt_alloc(size, !!(flags & __GFP_ZERO)); }

struct kmem_cache *__kmem_cache_create_args(const char *name, unsigned int size, struct kmem_cache_args *args, slab_flags_t flags)
{
	struct kmem_cache *c = nt_alloc(sizeof(*c), 1);
	if (c) { c->size = size; c->name = name; }
	return c;
}
void kmem_cache_destroy(struct kmem_cache *s) { nt_free(s); }
void *kmem_cache_alloc_noprof(struct kmem_cache *s, gfp_t flags) { return nt_alloc(s->size, !!(flags & __GFP_ZERO)); }
void *kmem_cache_alloc_lru_noprof(struct kmem_cache *s, struct list_lru *lru, gfp_t flags) { return nt_alloc(s->size, !!(flags & __GFP_ZERO)); }
void kmem_cache_free(struct kmem_cache *s, void *p) { nt_free(p); }
char *kstrdup(const char *s, gfp_t gfp)
{
	size_t n; char *p;
	if (!s) return NULL;
	n = strlen(s) + 1; p = nt_alloc(n, 0);
	if (p) memcpy(p, s, n);
	return p;
}
const char *kstrdup_const(const char *s, gfp_t gfp) { return kstrdup(s, gfp); }
void kfree_const(const void *p) { kfree(p); }
char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap)
{
	va_list aq; int n; char *p;
	va_copy(aq, ap); n = vsnprintf(NULL, 0, fmt, aq); va_end(aq);
	p = nt_alloc(n + 1, 0);
	if (p) vsnprintf(p, n + 1, fmt, ap);
	return p;
}
char *kasprintf(gfp_t gfp, const char *fmt, ...)
{
	va_list ap; char *p;
	va_start(ap, fmt); p = kvasprintf(gfp, fmt, ap); va_end(ap);
	return p;
}
EXPORT_SYMBOL(kmalloc_caches);

/* ---- globals the inline headers expect (no real memory map yet) ---------------------------- */
unsigned long page_offset_base;
unsigned long vmemmap_base;
unsigned long phys_base;
struct task_struct kpi_task;
static struct pid kpi_pid;
static struct signal_struct kpi_signal;
struct pcpu_hot pcpu_hot = { .current_task = &kpi_task };
extern const struct pcpu_hot const_pcpu_hot __attribute__((alias("pcpu_hot")));
EXPORT_SYMBOL(page_offset_base); EXPORT_SYMBOL(vmemmap_base); EXPORT_SYMBOL(phys_base);
EXPORT_SYMBOL(pcpu_hot); EXPORT_SYMBOL(const_pcpu_hot);

/* ---- module glue ------------------------------------------------------------------------ */
extern void radix_tree_init(void);

static int __init kpi_init(void)
{
	radix_tree_init();            /* the xarray/idr node cache is normally created by start_kernel() */
	refcount_set(&kpi_pid.count, 1);
	kpi_task.thread_pid = &kpi_pid;
	kpi_signal.pids[PIDTYPE_TGID] = kpi_signal.pids[PIDTYPE_PID] = kpi_signal.pids[PIDTYPE_PGID] = kpi_signal.pids[PIDTYPE_SID] = &kpi_pid;
	kpi_task.signal = &kpi_signal;
	kpi_task.pid = kpi_task.tgid = 4;
	kpi_task.group_leader = &kpi_task;
	strscpy(kpi_task.comm, "ReactOS", sizeof(kpi_task.comm));
	no_hash_pointers = true;      /* no boot-time random key available: print real pointers */
	kpi_tick();
	nt_register_tick(kpi_tick);
	printk("LinuxKPI core loaded (jiffies=%lu HZ=%d)\n", jiffies, HZ);
	return 0;
}

static void __exit kpi_exit(void) { }

module_init(kpi_init);
module_exit(kpi_exit);
MODULE_LICENSE("GPL");
