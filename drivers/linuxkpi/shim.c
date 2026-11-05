/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     The Linux kernel API surface (first slice). Every function here follows the
 *              System V ABI and Linux's LP64 types, because the module code does.
 */

#include "linuxkpi.h"

typedef unsigned long long u64;
typedef unsigned int u32;

/* ---- printk ---------------------------------------------------------------------- */

static int
LkFormat(char *Out, int Max, const char *Fmt, const lx_ulong *Args, int NumArgs)
{
    int o = 0, ai = 0;
    char tmp[24];

#define PUT(c) do { if (o < Max - 1) Out[o++] = (c); } while (0)
    while (*Fmt)
    {
        int longs = 0, zero = 0, width = 0;
        lx_ulong v;
        const char *s;
        int n, base, neg;

        if (*Fmt != '%') { PUT(*Fmt++); continue; }
        Fmt++;
        if (*Fmt == '0') { zero = 1; Fmt++; }
        while (*Fmt >= '0' && *Fmt <= '9') width = width * 10 + (*Fmt++ - '0');
        while (*Fmt == 'l' || *Fmt == 'z' || *Fmt == 't') { longs++; Fmt++; }
        switch (*Fmt)
        {
            case '%': PUT('%'); Fmt++; continue;
            case 'c': v = ai < NumArgs ? Args[ai++] : 0; PUT((char)v); Fmt++; continue;
            case 's':
                s = ai < NumArgs ? (const char *)(ULONG_PTR)Args[ai++] : "(null)";
                if (!s) s = "(null)";
                while (*s) PUT(*s++);
                Fmt++;
                continue;
            case 'p':
                v = ai < NumArgs ? Args[ai++] : 0;
                PUT('0'); PUT('x'); base = 16; neg = 0; width = 16; zero = 1; longs = 2;
                break;
            case 'd': case 'i':
                v = ai < NumArgs ? Args[ai++] : 0;
                if (longs) neg = (lx_long)v < 0; else { neg = (int)v < 0; v = (lx_ulong)(lx_long)(int)v; }
                if (neg) v = (lx_ulong)(-(lx_long)v);
                base = 10;
                break;
            case 'u': v = ai < NumArgs ? Args[ai++] : 0; if (!longs) v &= 0xffffffff; base = 10; neg = 0; break;
            case 'x': case 'X': v = ai < NumArgs ? Args[ai++] : 0; if (!longs) v &= 0xffffffff; base = 16; neg = 0; break;
            default: PUT('%'); PUT(*Fmt ? *Fmt : '?'); if (*Fmt) Fmt++; continue;
        }
        Fmt++;
        n = 0;
        do { int d = (int)(v % base); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v);
        if (neg) PUT('-');
        while (n < width - neg) { PUT(zero ? '0' : ' '); width--; }
        while (n) PUT(tmp[--n]);
    }
    Out[o] = 0;
    return o;
#undef PUT
}

int LX_ABI
lx_printk(const char *Fmt, lx_ulong a1, lx_ulong a2, lx_ulong a3, lx_ulong a4, lx_ulong a5)
{
    char Buf[256];
    lx_ulong Args[5] = { a1, a2, a3, a4, a5 };
    int n;

    if (Fmt[0] == '\001' && Fmt[1]) Fmt += 2;   /* KERN_<level> prefix */
    n = LkFormat(Buf, sizeof(Buf), Fmt, Args, 5);
    DbgPrint("LINUX: %s", Buf);
    return n;
}

/* ---- bridge for the Linux-compiled core module (kpi.ko) ----------------------------------- */

/* Allocations carry a 16-byte header with their size so ksize()/krealloc() work. */
typedef struct _LKPI_BLOCK { lx_ulong Size; lx_ulong Magic; } LKPI_BLOCK;
#define LKPI_BLOCK_MAGIC 0x4C4B5049424C4B31ULL

void * LX_ABI nt_alloc(lx_ulong Size, int Zero)
{
    LKPI_BLOCK *b = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)(Size ? Size : 1) + sizeof(LKPI_BLOCK), LKPI_TAG);
    if (!b) return NULL;
    b->Size = Size;
    b->Magic = LKPI_BLOCK_MAGIC;
    if (Zero) RtlZeroMemory(b + 1, (SIZE_T)Size);
    return b + 1;
}

void LX_ABI nt_free(void *p)
{
    LKPI_BLOCK * b;
    if (!p) return;
    b = (LKPI_BLOCK *)p - 1;
    if (b->Magic != LKPI_BLOCK_MAGIC)
    {
        DbgPrint("LKPI: kfree of a pointer not from nt_alloc: %p\n", p);
        return;
    }
    b->Magic = 0;
    ExFreePoolWithTag(b, LKPI_TAG);
}

lx_ulong LX_ABI nt_alloc_size(void *p)
{
    LKPI_BLOCK * b;
    if (!p) return 0;
    b = (LKPI_BLOCK *)p - 1;
    return b->Magic == LKPI_BLOCK_MAGIC ? b->Size : 0;
}

/* Kernel threads for workqueues and timers: fn(arg) runs on a system thread until it returns. */
typedef struct _LKPI_THREAD_START { void (LX_ABI *Fn)(void *); void *Arg; } LKPI_THREAD_START;

static VOID NTAPI LkpiThreadMain(PVOID Ctx)
{
    LKPI_THREAD_START Start = *(LKPI_THREAD_START *)Ctx;
    ExFreePoolWithTag(Ctx, LKPI_TAG);
    Start.Fn(Start.Arg);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

int LX_ABI nt_thread_spawn(void (LX_ABI *Fn)(void *), void *Arg)
{
    HANDLE Th;
    NTSTATUS Status;
    LKPI_THREAD_START *Start = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Start), LKPI_TAG);

    if (!Start) return -1;
    Start->Fn = Fn;
    Start->Arg = Arg;
    Status = PsCreateSystemThread(&Th, THREAD_ALL_ACCESS, NULL, NULL, NULL, LkpiThreadMain, Start);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Start, LKPI_TAG);
        return -1;
    }
    ZwClose(Th);
    return 0;
}


/* ---- memory bridge for the Linux page layer (kpi2_mm.c) ---------------------------------------- */

/* One physically contiguous region that backs all Linux "system memory" (alloc_pages, dma_alloc_*).
 * Linux's direct map then is simply va = pa + (va0 - pa0). Returns the kernel VA, fills *Phys. */
void * LX_ABI nt_contig_alloc(lx_ulong Size, lx_ulong *Phys)
{
    PHYSICAL_ADDRESS Low, High, Bound, Pa;
    PVOID Va;

    Low.QuadPart = 0; High.QuadPart = 0xFFFFFFFFFFFFULL; Bound.QuadPart = 0;
    Va = MmAllocateContiguousMemorySpecifyCache((SIZE_T)Size, Low, High, Bound, MmCached);
    if (!Va) return NULL;
    Pa = MmGetPhysicalAddress(Va);
    *Phys = (lx_ulong)Pa.QuadPart;
    RtlZeroMemory(Va, (SIZE_T)Size);
    return Va;
}

void LX_ABI nt_contig_free(void *Va, lx_ulong Size)
{
    MmFreeContiguousMemory(Va);
}

void * LX_ABI nt_ioremap(lx_ulong Phys, lx_ulong Size, int Cache)
{
    PHYSICAL_ADDRESS Pa;
    Pa.QuadPart = (LONGLONG)Phys;
    return MmMapIoSpace(Pa, (SIZE_T)Size, Cache == 2 ? MmWriteCombined : (Cache == 1 ? MmCached : MmNonCached));
}

void LX_ABI nt_iounmap(void *Va, lx_ulong Size) { MmUnmapIoSpace(Va, (SIZE_T)Size); }

/* Map an arbitrary list of physical page frames into one contiguous kernel VA range (vmap). */
void * LX_ABI nt_map_pfns(const lx_ulong *Pfns, ULONG Count, int Cache, void **MdlOut)
{
    PMDL Mdl = IoAllocateMdl(NULL, Count * PAGE_SIZE, FALSE, FALSE, NULL);
    PPFN_NUMBER Arr;
    PVOID Va;
    ULONG i;

    if (!Mdl) return NULL;
    Mdl->MdlFlags |= MDL_PAGES_LOCKED | MDL_PAGES_LOCKED;
    Mdl->ByteCount = Count * PAGE_SIZE;
    Mdl->ByteOffset = 0;
    Arr = MmGetMdlPfnArray(Mdl);
    for (i = 0; i < Count; i++) Arr[i] = (PFN_NUMBER)Pfns[i];
    Va = MmMapLockedPagesSpecifyCache(Mdl, KernelMode, Cache == 2 ? MmWriteCombined : MmCached, NULL, FALSE, NormalPagePriority);
    if (!Va) { IoFreeMdl(Mdl); return NULL; }
    *MdlOut = Mdl;
    return Va;
}

void LX_ABI nt_unmap_pfns(void *Va, void *MdlPtr)
{
    PMDL Mdl = MdlPtr;
    if (!Mdl) return;
    MmUnmapLockedPages(Va, Mdl);
    IoFreeMdl(Mdl);
}

lx_ulong LX_ABI nt_virt_to_phys(void *Va)
{
    return (lx_ulong)MmGetPhysicalAddress(Va).QuadPart;
}

unsigned long long LX_ABI nt_ktime_ns(void)
{
    return KeQueryInterruptTime() * 100ULL;
}

lx_ulong LX_ABI nt_current_tid(void) { return (lx_ulong)PsGetCurrentThreadId(); }

unsigned char LX_ABI nt_raise_dispatch(void)
{
    KIRQL Old = KeGetCurrentIrql();
    if (Old < DISPATCH_LEVEL) KeRaiseIrql(DISPATCH_LEVEL, &Old);
    return (unsigned char)Old;
}

void LX_ABI nt_lower_irql(unsigned char Old)
{
    if (KeGetCurrentIrql() > (KIRQL)Old) KeLowerIrql((KIRQL)Old);
}

void LX_ABI nt_sleep_ms(unsigned int Ms)
{
    LARGE_INTEGER Interval;

    if (KeGetCurrentIrql() >= DISPATCH_LEVEL)
    {
        KeStallExecutionProcessor(Ms ? Ms * 1000 : 10);
        return;
    }
    Interval.QuadPart = Ms ? -(LONGLONG)Ms * 10000 : -1000;   /* 0 = ~100 us */
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

void LX_ABI nt_stall_us(unsigned int Us) { KeStallExecutionProcessor(Us); }

unsigned long long LX_ABI nt_time_ms(void)
{
    LARGE_INTEGER Now;
    KeQueryTickCount(&Now);
    return (unsigned long long)Now.QuadPart * KeQueryTimeIncrement() / 10000;
}

void LX_ABI nt_dbgprint(const char *s) { DbgPrint("LINUX: %s", s); }

static void (LX_ABI *LkpiTickFn)(void);
static KTIMER LkpiTickTimer;
static KDPC LkpiTickDpc;

static VOID NTAPI LkpiTickDpcRoutine(PKDPC Dpc, PVOID Ctx, PVOID Arg1, PVOID Arg2)
{
    if (LkpiTickFn) LkpiTickFn();
}

void LX_ABI nt_register_tick(void (LX_ABI *Fn)(void))
{
    LARGE_INTEGER Due;

    LkpiTickFn = Fn;
    KeInitializeTimerEx(&LkpiTickTimer, SynchronizationTimer);
    KeInitializeDpc(&LkpiTickDpc, LkpiTickDpcRoutine, NULL);
    Due.QuadPart = -10000;   /* 1 ms */
    KeSetTimerEx(&LkpiTickTimer, Due, 1, &LkpiTickDpc);
}

/* ---- called when a module calls a Linux function that is not implemented yet ---------------- */
void * LX_ABI LkpiMissing(const char *Name)
{
    DbgPrint("LKPI: call to unimplemented Linux function '%s'\n", Name);
    return NULL;
}

/* ---- export table ----------------------------------------------------------------- */

const LKPI_EXPORT LkpiExports[] =
{
    { "nt_alloc", (PVOID)nt_alloc },
    { "nt_free", (PVOID)nt_free },
    { "nt_alloc_size", (PVOID)nt_alloc_size },
    { "nt_thread_spawn", (PVOID)nt_thread_spawn },
    { "nt_ktime_ns", (PVOID)nt_ktime_ns },
    { "nt_contig_alloc", (PVOID)nt_contig_alloc },
    { "nt_contig_free", (PVOID)nt_contig_free },
    { "nt_ioremap", (PVOID)nt_ioremap },
    { "nt_iounmap", (PVOID)nt_iounmap },
    { "nt_map_pfns", (PVOID)nt_map_pfns },
    { "nt_unmap_pfns", (PVOID)nt_unmap_pfns },
    { "nt_virt_to_phys", (PVOID)nt_virt_to_phys },
    { "nt_current_tid", (PVOID)nt_current_tid },
    { "nt_raise_dispatch", (PVOID)nt_raise_dispatch },
    { "nt_lower_irql", (PVOID)nt_lower_irql },
    { "nt_sleep_ms", (PVOID)nt_sleep_ms },
    { "nt_stall_us", (PVOID)nt_stall_us },
    { "nt_time_ms", (PVOID)nt_time_ms },
    { "nt_dbgprint", (PVOID)nt_dbgprint },
    { "nt_register_tick", (PVOID)nt_register_tick },
    { "_printk", (PVOID)lx_printk },
    { "printk", (PVOID)lx_printk },
};

const ULONG LkpiExportCount = RTL_NUMBER_OF(LkpiExports);
