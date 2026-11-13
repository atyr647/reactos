// SPDX-License-Identifier: GPL-2.0
/* Exercises kpi.ko/kpi2.ko: workqueues, delayed work, timers, dma-fence, dma-resv, krealloc, devres, ww_mutex. */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/ww_mutex.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>

static int failures;
#define CHECK(c, what) do { if (c) pr_info("kpi2test: ok   %s\n", what); else { failures++; pr_err("kpi2test: FAIL %s\n", what); } } while (0)

static int ran;
static void work_fn(struct work_struct *w) { ran++; }
static DECLARE_WORK(w1, work_fn);
static DECLARE_DELAYED_WORK(dw1, work_fn);

static int timer_hits;
static void timer_fn(struct timer_list *t) { timer_hits++; }
static DEFINE_TIMER(t1, timer_fn);

static DEFINE_SPINLOCK(fence_lock);
static const char *fence_name(struct dma_fence *f) { return "kpi2test"; }
static const struct dma_fence_ops fence_ops = { .get_driver_name = fence_name, .get_timeline_name = fence_name };

static DEFINE_WW_CLASS(test_class);

static int __init kpi2test_init(void)
{
	struct dma_fence fence;
	struct device dev = { };
	char *p;
	struct ww_mutex wm;
	int i;

	pr_info("kpi2test: start\n");

	/* workqueues */
	queue_work(system_wq, &w1);
	for (i = 0; i < 100 && !ran; i++) msleep(1);
	CHECK(ran == 1, "queue_work runs on system_wq");
	queue_delayed_work(system_wq, &dw1, msecs_to_jiffies(20));
	for (i = 0; i < 200 && ran < 2; i++) msleep(1);
	CHECK(ran == 2, "delayed work fires");
	{
		struct workqueue_struct *wq = alloc_workqueue("kpi2test", 0, 1);
		struct work_struct w2;
		INIT_WORK(&w2, work_fn);
		queue_work(wq, &w2);
		flush_work(&w2);
		CHECK(ran == 3, "private workqueue + flush_work");
		destroy_workqueue(wq);
	}

	/* timers */
	mod_timer(&t1, jiffies + msecs_to_jiffies(10));
	for (i = 0; i < 200 && !timer_hits; i++) msleep(1);
	CHECK(timer_hits == 1, "timer fires once");

	/* dma-fence (real Linux code) */
	dma_fence_init(&fence, &fence_ops, &fence_lock, dma_fence_context_alloc(1), 1);
	CHECK(!dma_fence_is_signaled(&fence), "fence starts unsignaled");
	CHECK(dma_fence_wait_timeout(&fence, false, 5) == 0, "fence wait times out");
	CHECK(dma_fence_signal(&fence) == 0, "fence signal");
	CHECK(dma_fence_is_signaled(&fence), "fence signaled");
	CHECK(dma_fence_wait_timeout(&fence, false, 5) > 0, "fence wait returns when signaled");

	/* allocator */
	p = kmalloc(8, GFP_KERNEL);
	strcpy(p, "abcdefg");
	p = krealloc(p, 100, GFP_KERNEL);
	CHECK(p && !strcmp(p, "abcdefg") && ksize(p) >= 100, "krealloc keeps data, ksize");
	kfree(p);

	/* devres */
	device_initialize(&dev);
	p = devm_kmalloc(&dev, 16, GFP_KERNEL);
	CHECK(p != NULL, "devm_kmalloc");
	device_del(&dev);

	/* ww_mutex */
	ww_mutex_init(&wm, &test_class);
	CHECK(ww_mutex_lock(&wm, NULL) == 0, "ww_mutex_lock");
	CHECK(ww_mutex_trylock(&wm, NULL) == 0, "ww_mutex_trylock fails when held");
	ww_mutex_unlock(&wm);

	/* memory layer */
	{
		struct page *pg = alloc_pages(GFP_KERNEL | __GFP_ZERO, 2);
		unsigned char *a;
		struct page *three[3], *pgs[2];
		dma_addr_t dma;
		void *cpu, *vm;

		CHECK(pg != NULL, "alloc_pages(order 2)");
		if (pg) {
			a = page_address(pg);
			CHECK(a[0] == 0 && a[4 * 4096 - 1] == 0, "pages are zeroed");
			memset(a, 0xA5, 4 * 4096);
			CHECK(virt_to_page(a + 8192) == pg + 2, "virt_to_page(page_address()) round trip");
			CHECK(page_to_phys(pg) == virt_to_phys(a), "page_to_phys == virt_to_phys");
			__free_pages(pg, 2);
		}
		three[0] = alloc_page(GFP_KERNEL); three[1] = alloc_page(GFP_KERNEL); three[2] = alloc_page(GFP_KERNEL);
		__free_page(three[1]);
		pgs[0] = three[0]; pgs[1] = three[2];
		*(int *)page_address(three[0]) = 0x1111; *(int *)page_address(three[2]) = 0x2222;
		vm = vmap(pgs, 2, VM_MAP, PAGE_KERNEL);
		CHECK(vm != NULL, "vmap of two pages");
		if (vm) {
			CHECK(*(int *)vm == 0x1111 && *(int *)((char *)vm + 4096) == 0x2222, "vmap contents match");
			vunmap(vm);
		}
		__free_page(three[0]); __free_page(three[2]);
		cpu = dma_alloc_coherent(&dev, 8192, &dma, GFP_KERNEL);
		CHECK(cpu != NULL && dma != 0, "dma_alloc_coherent");
		if (cpu) {
			CHECK(dma == virt_to_phys(cpu), "dma address == physical address");
			dma_free_coherent(&dev, 8192, cpu, dma);
		}
	}

	pr_info("kpi2test: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return 0;
}
module_init(kpi2test_init);
MODULE_LICENSE("GPL");
