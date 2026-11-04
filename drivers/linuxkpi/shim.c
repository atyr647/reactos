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

/* ---- memory ---------------------------------------------------------------------- */

static void * LX_ABI lx_kmalloc(lx_ulong Size, u32 Flags)
{
    return ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)Size, LKPI_TAG);
}

static void * LX_ABI lx_kzalloc(lx_ulong Size, u32 Flags)
{
    void *p = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)Size, LKPI_TAG);
    if (p) RtlZeroMemory(p, (SIZE_T)Size);
    return p;
}

static void LX_ABI lx_kfree(const void *p)
{
    if (p) ExFreePoolWithTag((PVOID)p, LKPI_TAG);
}

static void * LX_ABI lx_memset(void *d, int c, lx_ulong n) { RtlFillMemory(d, (SIZE_T)n, (UCHAR)c); return d; }
static void * LX_ABI lx_memcpy(void *d, const void *s, lx_ulong n) { RtlCopyMemory(d, s, (SIZE_T)n); return d; }
static void * LX_ABI lx_memmove(void *d, const void *s, lx_ulong n) { RtlMoveMemory(d, s, (SIZE_T)n); return d; }
static int LX_ABI lx_memcmp(const void *a, const void *b, lx_ulong n) { return memcmp(a, b, (SIZE_T)n); }
static lx_ulong LX_ABI lx_strlen(const char *s) { return strlen(s); }
static int LX_ABI lx_strcmp(const char *a, const char *b) { return strcmp(a, b); }

/* ---- export table ----------------------------------------------------------------- */

const LKPI_EXPORT LkpiExports[] =
{
    { "_printk", (PVOID)lx_printk },
    { "printk", (PVOID)lx_printk },
    { "__kmalloc_noprof", (PVOID)lx_kmalloc },
    { "kmalloc", (PVOID)lx_kmalloc },
    { "kzalloc", (PVOID)lx_kzalloc },
    { "kfree", (PVOID)lx_kfree },
    { "memset", (PVOID)lx_memset },
    { "memcpy", (PVOID)lx_memcpy },
    { "memmove", (PVOID)lx_memmove },
    { "memcmp", (PVOID)lx_memcmp },
    { "strlen", (PVOID)lx_strlen },
    { "strcmp", (PVOID)lx_strcmp },
};

const ULONG LkpiExportCount = RTL_NUMBER_OF(LkpiExports);
