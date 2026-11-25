// SPDX-License-Identifier: GPL-2.0
/*
 * Exercises kernel_fpu_begin()/kernel_fpu_end() as amdgpu's display core uses them: several threads run SSE code
 * inside FP sections long enough for clock and device interrupts to land in them, and check that
 *  - all 16 XMM registers survive (patterns loaded, ~2 ms spin, read back),
 *  - double-precision results are bit-identical to a reference computed once,
 *  - the section runs with the default MXCSR, and the thread's own MXCSR comes back afterwards.
 * Built with the kernel's FP flags (Kbuild). Prints "fputest: ... PASS" or "FAIL".
 */
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <asm/fpu/api.h>

#define THREADS 3
#define SECTIONS 400
#define SPIN_US 2000

static u64 tsc_per_us;
static double reference;
static DECLARE_COMPLETION(done);
static atomic_t running = ATOMIC_INIT(THREADS);
static atomic_t reg_bad, math_bad, csr_in_bad, csr_out_bad, sections;

static inline u64 rdtsc_now(void)
{
	u32 lo, hi;

	asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
}

static inline unsigned int get_csr(void)
{
	unsigned int v;

	asm volatile("stmxcsr %0" : "=m"(v));
	return v;
}

static inline void set_csr(unsigned int v)
{
	asm volatile("ldmxcsr %0" : : "m"(v));
}

static noinline double compute(void)
{
	double acc = 0.0, x = 1.0;
	int i;

	for (i = 1; i <= 20000; i++) {
		x = x * 1.0000001 + 1.0 / (i + 0.5);
		acc += x / (i * 3.25 + 1.0);
	}
	return acc;
}

/* load xmm0..15 from in[], spin for ticks TSC cycles, store them to out[] */
static noinline void hold_registers(const u64 *in, u64 *out, u64 ticks)
{
	asm volatile(
		"movdqu   0(%0), %%xmm0\n\t"  "movdqu  16(%0), %%xmm1\n\t"
		"movdqu  32(%0), %%xmm2\n\t"  "movdqu  48(%0), %%xmm3\n\t"
		"movdqu  64(%0), %%xmm4\n\t"  "movdqu  80(%0), %%xmm5\n\t"
		"movdqu  96(%0), %%xmm6\n\t"  "movdqu 112(%0), %%xmm7\n\t"
		"movdqu 128(%0), %%xmm8\n\t"  "movdqu 144(%0), %%xmm9\n\t"
		"movdqu 160(%0), %%xmm10\n\t" "movdqu 176(%0), %%xmm11\n\t"
		"movdqu 192(%0), %%xmm12\n\t" "movdqu 208(%0), %%xmm13\n\t"
		"movdqu 224(%0), %%xmm14\n\t" "movdqu 240(%0), %%xmm15\n\t"
		"rdtsc\n\t" "shl $32, %%rdx\n\t" "or %%rdx, %%rax\n\t" "mov %%rax, %%r8\n"
		"1:\n\t" "pause\n\t" "rdtsc\n\t" "shl $32, %%rdx\n\t" "or %%rdx, %%rax\n\t" "sub %%r8, %%rax\n\t"
		"cmp %2, %%rax\n\t" "jb 1b\n\t"
		"movdqu %%xmm0,    0(%1)\n\t" "movdqu %%xmm1,   16(%1)\n\t"
		"movdqu %%xmm2,   32(%1)\n\t" "movdqu %%xmm3,   48(%1)\n\t"
		"movdqu %%xmm4,   64(%1)\n\t" "movdqu %%xmm5,   80(%1)\n\t"
		"movdqu %%xmm6,   96(%1)\n\t" "movdqu %%xmm7,  112(%1)\n\t"
		"movdqu %%xmm8,  128(%1)\n\t" "movdqu %%xmm9,  144(%1)\n\t"
		"movdqu %%xmm10, 160(%1)\n\t" "movdqu %%xmm11, 176(%1)\n\t"
		"movdqu %%xmm12, 192(%1)\n\t" "movdqu %%xmm13, 208(%1)\n\t"
		"movdqu %%xmm14, 224(%1)\n\t" "movdqu %%xmm15, 240(%1)\n\t"
		: : "r"(in), "r"(out), "r"(ticks)
		: "rax", "rdx", "r8", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
		  "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

static int fp_thread(void *arg)
{
	unsigned long id = (unsigned long)arg;
	unsigned int own_csr = 0x7F80;		/* round toward zero, all exceptions masked */
	u64 in[32], out[32];
	int s, i;

	set_csr(own_csr);
	for (s = 0; s < SECTIONS; s++) {
		double r;

		for (i = 0; i < 32; i++)
			in[i] = (id << 56) ^ ((u64)s << 32) ^ (0x9E3779B97F4A7C15ULL * (i + 1));
		kernel_fpu_begin();
		if (get_csr() != 0x1F80)
			atomic_inc(&csr_in_bad);
		hold_registers(in, out, tsc_per_us * SPIN_US);
		r = compute();
		kernel_fpu_end();
		if (get_csr() != own_csr)
			atomic_inc(&csr_out_bad);
		if (memcmp(in, out, sizeof(in)))
			atomic_inc(&reg_bad);
		if (memcmp(&r, &reference, sizeof(r)))
			atomic_inc(&math_bad);
		atomic_inc(&sections);
		if (s % 4 == 3)
			msleep(1);
	}
	set_csr(0x1F80);
	if (atomic_dec_and_test(&running))
		complete(&done);
	return 0;
}

static int __init fputest_init(void)
{
	u64 t0, c0;
	unsigned long i;
	ktime_t start = ktime_get();

	c0 = rdtsc_now();
	t0 = ktime_get_ns();
	msleep(20);
	tsc_per_us = (rdtsc_now() - c0) / max_t(u64, (ktime_get_ns() - t0) / 1000, 1);
	if (!tsc_per_us)
		tsc_per_us = 1;
	kernel_fpu_begin();
	reference = compute();
	kernel_fpu_end();
	for (i = 0; i < THREADS; i++)
		kthread_run(fp_thread, (void *)i, "fputest%lu", i);
	wait_for_completion(&done);
	pr_info("fputest: %d sections of %d us in %d threads (%lld ms, %llu TSC ticks/us): %d register, %d result, %d+%d MXCSR (in+after) mismatches -> %s\n",
		atomic_read(&sections), SPIN_US, THREADS, ktime_ms_delta(ktime_get(), start), tsc_per_us,
		atomic_read(&reg_bad), atomic_read(&math_bad), atomic_read(&csr_in_bad), atomic_read(&csr_out_bad),
		atomic_read(&reg_bad) + atomic_read(&math_bad) + atomic_read(&csr_in_bad) + atomic_read(&csr_out_bad) ?
		"FAIL" : "PASS");
	return 0;
}
module_init(fputest_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LinuxKPI kernel_fpu_begin/end test");
