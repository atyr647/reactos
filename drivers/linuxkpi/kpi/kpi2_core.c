// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI second core module: kernel services that the DRM stack needs beyond kpi.ko.
 * Workqueues, timers, RCU stand-ins, tracepoint stubs, allocator helpers. Self-contained Linux
 * sources (dma-fence, dma-resv, ...) are compiled in next to this file by build2.sh.
 *
 * Uniprocessor model: there is no preemption inside Linux code except at the points where it
 * sleeps; worker and timer threads run on NT system threads and poll (1 ms).
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/rcupdate.h>
#include <linux/irq_work.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/trace_events.h>

/* ---- bridge to linuxkpi.sys --------------------------------------------------------------- */
extern unsigned long nt_alloc_size(void *p);
extern int nt_thread_spawn(void (*fn)(void *), void *arg);
extern unsigned long long nt_ktime_ns(void);
extern unsigned long nt_current_tid(void);
extern void nt_dbgprint(const char *s);
extern void nt_sleep_ms(unsigned int ms);

/* ---- time --------------------------------------------------------------------------------- */
ktime_t ktime_get(void) { return (ktime_t)nt_ktime_ns(); }

/* ---- allocator helpers --------------------------------------------------------------------- */
size_t ksize(const void *p) { return nt_alloc_size((void *)p); }
size_t kmalloc_size_roundup(size_t size) { return size; }

void *krealloc_noprof(const void *p, size_t new_size, gfp_t flags)
{
	void *n;
	size_t old;

	if (!new_size) { kfree(p); return ZERO_SIZE_PTR; }
	if (!p || p == ZERO_SIZE_PTR) return __kmalloc_noprof(new_size, flags);
	old = nt_alloc_size((void *)p);
	if (new_size <= old) return (void *)p;
	n = __kmalloc_noprof(new_size, flags);
	if (n) { memcpy(n, p, old); kfree(p); }
	return n;
}

char *kstrdup(const char *s, gfp_t gfp)
{
	size_t len;
	char *buf;

	if (!s) return NULL;
	len = strlen(s) + 1;
	buf = __kmalloc_noprof(len, gfp);
	if (buf) memcpy(buf, s, len);
	return buf;
}

const char *kstrdup_const(const char *s, gfp_t gfp) { return kstrdup(s, gfp); }
void kfree_const(const void *x) { kfree(x); }

void *kmemdup_noprof(const void *src, size_t len, gfp_t gfp)
{
	void *p = __kmalloc_noprof(len, gfp);
	if (p) memcpy(p, src, len);
	return p;
}

char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap)
{
	va_list aq;
	unsigned int len;
	char *p;

	va_copy(aq, ap);
	len = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	p = __kmalloc_noprof(len + 1, gfp);
	if (!p) return NULL;
	vsnprintf(p, len + 1, fmt, ap);
	return p;
}

char *kasprintf(gfp_t gfp, const char *fmt, ...)
{
	va_list ap;
	char *p;

	va_start(ap, fmt);
	p = kvasprintf(gfp, fmt, ap);
	va_end(ap);
	return p;
}

/* ---- RCU / irq_work (uniprocessor stand-ins) ------------------------------------------------ */
void call_rcu(struct rcu_head *head, rcu_callback_t func) { func(head); }
void synchronize_rcu(void) { }
bool irq_work_queue(struct irq_work *work) { atomic_set(&work->node.a_flags, 0); work->func(work); return true; }
int wake_up_state(struct task_struct *p, unsigned int state) { return 0; }
int wake_up_process(struct task_struct *p) { return 0; }

/* ---- tracepoints: compiled in but never enabled -------------------------------------------- */
bool __trace_trigger_soft_disabled(struct trace_event_file *file) { return false; }
void *perf_trace_buf_alloc(int size, struct pt_regs **regs, int *rctxp) { return NULL; }
void perf_trace_run_bpf_submit(void *raw_data, int size, int rctx, struct trace_event_call *call, u64 count,
			       struct pt_regs *regs, struct hlist_head *head, struct task_struct *task) { }
void *trace_event_buffer_reserve(struct trace_event_buffer *fbuffer, struct trace_event_file *trace_file, unsigned long len) { return NULL; }
void trace_event_buffer_commit(struct trace_event_buffer *fbuffer) { }
void trace_event_printf(struct trace_iterator *iter, const char *fmt, ...) { }
int trace_event_raw_init(struct trace_event_call *call) { return 0; }
int trace_event_reg(struct trace_event_call *call, enum trace_reg type, void *data) { return 0; }
enum print_line_t trace_handle_return(struct trace_seq *s) { return TRACE_TYPE_HANDLED; }
int trace_raw_output_prep(struct trace_iterator *iter, struct trace_event *event) { return TRACE_TYPE_HANDLED; }

/* ---- seq_file: just enough for debugfs show callbacks -------------------------------------- */
void seq_printf(struct seq_file *m, const char *f, ...) { }

/* ---- timers -------------------------------------------------------------------------------- */
static HLIST_HEAD(kpi_timers);
static DEFINE_SPINLOCK(kpi_timer_lock);
static int kpi_timer_thread_started;

static void kpi_timer_thread(void *arg)
{
	for (;;) {
		struct timer_list *fired = NULL;
		unsigned long flags;
		struct hlist_node *n;
		struct timer_list *t;

		spin_lock_irqsave(&kpi_timer_lock, flags);
		hlist_for_each_entry_safe(t, n, &kpi_timers, entry) {
			if (time_after_eq(jiffies, t->expires)) {
				hlist_del_init(&t->entry);
				fired = t;
				break;
			}
		}
		spin_unlock_irqrestore(&kpi_timer_lock, flags);
		if (fired)
			fired->function(fired);
		else
			nt_sleep_ms(1);
	}
}

void init_timer_key(struct timer_list *timer, void (*func)(struct timer_list *), unsigned int flags,
		    const char *name, struct lock_class_key *key)
{
	timer->entry.pprev = NULL;
	timer->entry.next = NULL;
	timer->function = func;
	timer->flags = flags;
}

int mod_timer(struct timer_list *timer, unsigned long expires)
{
	unsigned long flags;
	int pending;

	if (!kpi_timer_thread_started) {
		kpi_timer_thread_started = 1;
		nt_thread_spawn(kpi_timer_thread, NULL);
	}
	spin_lock_irqsave(&kpi_timer_lock, flags);
	pending = !hlist_unhashed(&timer->entry);
	if (pending)
		hlist_del_init(&timer->entry);
	timer->expires = expires;
	hlist_add_head(&timer->entry, &kpi_timers);
	spin_unlock_irqrestore(&kpi_timer_lock, flags);
	return pending;
}

int timer_delete(struct timer_list *timer)
{
	unsigned long flags;
	int pending;

	spin_lock_irqsave(&kpi_timer_lock, flags);
	pending = !hlist_unhashed(&timer->entry);
	if (pending)
		hlist_del_init(&timer->entry);
	spin_unlock_irqrestore(&kpi_timer_lock, flags);
	return pending;
}

int timer_delete_sync(struct timer_list *timer) { return timer_delete(timer); }

/* ---- workqueues ---------------------------------------------------------------------------- */
struct workqueue_struct {
	struct list_head list;
	spinlock_t lock;
	int running;
	int stop;
	struct work_struct *current_work;
	char name[32];
};

static struct workqueue_struct kpi_system_wq, kpi_unbound_wq, kpi_long_wq;
struct workqueue_struct *system_wq = &kpi_system_wq;
struct workqueue_struct *system_unbound_wq = &kpi_unbound_wq;
struct workqueue_struct *system_long_wq = &kpi_long_wq;

static void kpi_wq_thread(void *arg)
{
	struct workqueue_struct *wq = arg;

	for (;;) {
		struct work_struct *w = NULL;
		unsigned long flags;

		spin_lock_irqsave(&wq->lock, flags);
		if (!list_empty(&wq->list)) {
			w = list_first_entry(&wq->list, struct work_struct, entry);
			list_del_init(&w->entry);
			clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w));
			wq->current_work = w;
		} else if (wq->stop) {
			spin_unlock_irqrestore(&wq->lock, flags);
			wq->running = 0;
			return;
		}
		spin_unlock_irqrestore(&wq->lock, flags);
		if (w) {
			w->func(w);
			wq->current_work = NULL;
		} else {
			nt_sleep_ms(1);
		}
	}
}

static void kpi_wq_init(struct workqueue_struct *wq, const char *name)
{
	INIT_LIST_HEAD(&wq->list);
	spin_lock_init(&wq->lock);
	wq->stop = 0;
	wq->running = 1;
	wq->current_work = NULL;
	strscpy(wq->name, name, sizeof(wq->name));
	nt_thread_spawn(kpi_wq_thread, wq);
}

static void kpi_system_wqs_init(void)
{
	kpi_wq_init(&kpi_system_wq, "events");
	kpi_wq_init(&kpi_unbound_wq, "events_unbound");
	kpi_wq_init(&kpi_long_wq, "events_long");
}

struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...)
{
	struct workqueue_struct *wq = kzalloc(sizeof(*wq), GFP_KERNEL);

	if (!wq) return NULL;
	kpi_wq_init(wq, fmt);
	return wq;
}

bool queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work)
{
	unsigned long flags;

	if (test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)))
		return false;
	atomic_long_set(&work->data, (long)wq | 1);	/* remember the queue for flush_work(), keep the pending bit */
	spin_lock_irqsave(&wq->lock, flags);
	list_add_tail(&work->entry, &wq->list);
	spin_unlock_irqrestore(&wq->lock, flags);
	return true;
}

bool queue_work_node(int node, struct workqueue_struct *wq, struct work_struct *work)
{
	return queue_work_on(0, wq, work);
}

void delayed_work_timer_fn(struct timer_list *t)
{
	struct delayed_work *dwork = from_timer(dwork, t, timer);

	/* the work was marked pending when the delay was armed; hand it to the queue now */
	clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&dwork->work));
	queue_work_on(0, dwork->wq, &dwork->work);
}

bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay)
{
	if (!delay)
		return queue_work_on(cpu, wq, &dwork->work);
	if (test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&dwork->work)))
		return false;
	dwork->wq = wq;
	dwork->cpu = cpu;
	atomic_long_set(&dwork->work.data, (long)wq | 1);
	mod_timer(&dwork->timer, jiffies + delay);
	return true;
}

bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay)
{
	bool was = timer_delete(&dwork->timer);
	unsigned long flags;

	/* also pull it back if it is already queued */
	spin_lock_irqsave(&wq->lock, flags);
	if (!list_empty(&dwork->work.entry)) {
		list_del_init(&dwork->work.entry);
		was = true;
	}
	spin_unlock_irqrestore(&wq->lock, flags);
	clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&dwork->work));
	queue_delayed_work_on(cpu, wq, dwork, delay);
	return was;
}

static bool kpi_cancel_work(struct work_struct *work, struct workqueue_struct *wq)
{
	bool pending = test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work));
	unsigned long flags;

	if (wq) {
		spin_lock_irqsave(&wq->lock, flags);
		if (!list_empty(&work->entry))
			list_del_init(&work->entry);
		spin_unlock_irqrestore(&wq->lock, flags);
	}
	clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work));
	return pending;
}

bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
	bool a = timer_delete_sync(&dwork->timer);
	bool b = kpi_cancel_work(&dwork->work, dwork->wq);

	return a || b;
}

bool cancel_delayed_work(struct delayed_work *dwork) { return cancel_delayed_work_sync(dwork); }

bool cancel_work_sync(struct work_struct *work) { return kpi_cancel_work(work, system_wq); }

bool flush_work(struct work_struct *work)
{
	bool waited = false;
	struct workqueue_struct *wq = (struct workqueue_struct *)(atomic_long_read(&work->data) & ~7UL);

	if (!wq) wq = system_wq;
	while (test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)) || wq->current_work == work) {
		waited = true;
		nt_sleep_ms(1);
	}
	return waited;
}

bool flush_delayed_work(struct delayed_work *dwork)
{
	if (timer_delete_sync(&dwork->timer))
		queue_work_on(0, dwork->wq, &dwork->work);
	return flush_work(&dwork->work);
}

void __flush_workqueue(struct workqueue_struct *wq)
{
	while (!list_empty(&wq->list) || wq->current_work)
		nt_sleep_ms(1);
}

void drain_workqueue(struct workqueue_struct *wq) { __flush_workqueue(wq); }

void destroy_workqueue(struct workqueue_struct *wq)
{
	__flush_workqueue(wq);
	wq->stop = 1;
	while (wq->running)
		nt_sleep_ms(1);
	kfree(wq);
}

struct work_struct *current_work(void) { return system_wq->current_work; }

static int __init kpi2_init(void)
{
	kpi_system_wqs_init();
	return 0;
}

static void __exit kpi2_exit(void) { }
module_init(kpi2_init);
module_exit(kpi2_exit);
MODULE_LICENSE("GPL");
