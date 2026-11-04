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

void * LX_ABI nt_alloc(lx_ulong Size, int Zero)
{
    PVOID p = ExAllocatePoolWithTag(NonPagedPool, Size ? (SIZE_T)Size : 1, LKPI_TAG);
    if (p && Zero) RtlZeroMemory(p, (SIZE_T)Size);
    return p;
}

void LX_ABI nt_free(void *p) { if (p) ExFreePoolWithTag(p, LKPI_TAG); }

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
