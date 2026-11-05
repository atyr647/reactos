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

/*
 * Allocator. Linux code allocates at any IRQL (GFP_ATOMIC under spinlocks); ReactOS's NonPagedPool cannot
 * expand at DISPATCH_LEVEL, so small blocks come from a pre-allocated arena with per-size-class free lists.
 * Every block has a 16-byte header: [0] requested size, [8] magic | class (0xFF = pool allocation).
 */
typedef struct _LKPI_BLOCK { lx_ulong Size; lx_ulong Tag; } LKPI_BLOCK;
#define LKPI_BLOCK_MAGIC 0x4C4B504900000000ULL
#define LKPI_NUM_CLASSES 12            /* 32 .. 64 KiB payload */
#define LKPI_ARENA_MB    48

static PUCHAR LkpiArena, LkpiArenaPtr, LkpiArenaEnd;
static LKPI_BLOCK *LkpiFree[LKPI_NUM_CLASSES];
static KSPIN_LOCK LkpiAllocLock;

VOID LkpiInitAllocator(VOID)
{
    SIZE_T Size = (SIZE_T)LKPI_ARENA_MB << 20;

    KeInitializeSpinLock(&LkpiAllocLock);
    while (!LkpiArena && Size >= (4 << 20))
    {
        LkpiArena = ExAllocatePoolWithTag(NonPagedPool, Size, LKPI_TAG);
        if (!LkpiArena) Size >>= 1;
    }
    LkpiArenaPtr = LkpiArena;
    LkpiArenaEnd = LkpiArena ? LkpiArena + Size : NULL;
    DbgPrint("LKPI: allocator arena %lu KiB at %p\n", (ULONG)(Size >> 10), LkpiArena);
}

static ULONG LkpiClassOf(lx_ulong Size, lx_ulong *Payload)
{
    ULONG c = 0;
    lx_ulong p = 32;

    while (p < Size && c < LKPI_NUM_CLASSES) { p <<= 1; c++; }
    *Payload = p;
    return c;
}

void * LX_ABI nt_alloc(lx_ulong Size, int Zero)
{
    LKPI_BLOCK *b = NULL;
    lx_ulong Payload;
    ULONG c = LkpiClassOf(Size ? Size : 1, &Payload);
    KIRQL Old;

    if (c < LKPI_NUM_CLASSES)
    {
        KeAcquireSpinLock(&LkpiAllocLock, &Old);
        if (LkpiFree[c])
        {
            b = LkpiFree[c];
            LkpiFree[c] = *(LKPI_BLOCK **)(b + 1);
        }
        else if (LkpiArenaPtr && LkpiArenaPtr + sizeof(LKPI_BLOCK) + Payload <= LkpiArenaEnd)
        {
            b = (LKPI_BLOCK *)LkpiArenaPtr;
            LkpiArenaPtr += sizeof(LKPI_BLOCK) + Payload;
        }
        KeReleaseSpinLock(&LkpiAllocLock, Old);
        if (b) b->Tag = LKPI_BLOCK_MAGIC | c;
    }
    if (!b)
    {
        /* large or arena exhausted: general pool (only valid at <= DISPATCH, and may fail when expanding there) */
        if (KeGetCurrentIrql() > APC_LEVEL)
            DbgPrint("LKPI: pool allocation of %lu bytes at IRQL %u (arena %s)\n", (ULONG)Size, (unsigned)KeGetCurrentIrql(), LkpiArena ? "exhausted/too small" : "missing");
        b = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)Size + sizeof(LKPI_BLOCK), LKPI_TAG);
        if (!b) return NULL;
        b->Tag = LKPI_BLOCK_MAGIC | 0xFF;
    }
    b->Size = Size;
    if (Zero) RtlZeroMemory(b + 1, (SIZE_T)Size);
    return b + 1;
}

void LX_ABI nt_free(void *p)
{
    LKPI_BLOCK *b;
    KIRQL Old;
    ULONG c;

    if (!p) return;
    b = (LKPI_BLOCK *)p - 1;
    if ((b->Tag & 0xFFFFFFFF00000000ULL) != LKPI_BLOCK_MAGIC)
    {
        DbgPrint("LKPI: kfree of a pointer not from nt_alloc: %p\n", p);
        return;
    }
    c = (ULONG)(b->Tag & 0xFF);
    if (c == 0xFF)
    {
        b->Tag = 0;
        ExFreePoolWithTag(b, LKPI_TAG);
        return;
    }
    KeAcquireSpinLock(&LkpiAllocLock, &Old);
    *(LKPI_BLOCK **)(b + 1) = LkpiFree[c];
    LkpiFree[c] = b;
    KeReleaseSpinLock(&LkpiAllocLock, Old);
}

lx_ulong LX_ABI nt_alloc_size(void *p)
{
    LKPI_BLOCK * b;
    if (!p) return 0;
    b = (LKPI_BLOCK *)p - 1;
    return (b->Tag & 0xFFFFFFFF00000000ULL) == LKPI_BLOCK_MAGIC ? b->Size : 0;
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


#define LKPI_CHECK_IRQL(fn) do { if (KeGetCurrentIrql() > APC_LEVEL) DbgPrint("LKPI: " fn " called at IRQL %u\n", (unsigned)KeGetCurrentIrql()); } while (0)

/* ---- memory bridge for the Linux page layer (kpi2_mm.c) ---------------------------------------- */

/* One physically contiguous region that backs all Linux "system memory" (alloc_pages, dma_alloc_*).
 * Linux's direct map then is simply va = pa + (va0 - pa0). Returns the kernel VA, fills *Phys. */
void * LX_ABI nt_contig_alloc(lx_ulong Size, lx_ulong *Phys)
{
    LKPI_CHECK_IRQL("nt_contig_alloc");
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
    LKPI_CHECK_IRQL("nt_ioremap");
    PHYSICAL_ADDRESS Pa;
    Pa.QuadPart = (LONGLONG)Phys;
    return MmMapIoSpace(Pa, (SIZE_T)Size, Cache == 2 ? MmWriteCombined : (Cache == 1 ? MmCached : MmNonCached));
}

void LX_ABI nt_iounmap(void *Va, lx_ulong Size) { MmUnmapIoSpace(Va, (SIZE_T)Size); }

/* Map an arbitrary list of physical page frames into one contiguous kernel VA range (vmap). */
void * LX_ABI nt_map_pfns(const lx_ulong *Pfns, ULONG Count, int Cache, void **MdlOut)
{
    LKPI_CHECK_IRQL("nt_map_pfns");
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
    LKPI_CHECK_IRQL("nt_virt_to_phys");
    return (lx_ulong)MmGetPhysicalAddress(Va).QuadPart;
}


/* ---- PCI configuration space through the HAL (bus numbers/devfn as Linux sees them) ---------- */

int LX_ABI nt_pci_cfg_read(unsigned int Bus, unsigned int DevFn, unsigned int Off, unsigned int Len, unsigned int *Val)
{
    PCI_SLOT_NUMBER Slot;
    ULONG Got;

    Slot.u.AsULONG = 0;
    Slot.u.bits.DeviceNumber = DevFn >> 3;
    Slot.u.bits.FunctionNumber = DevFn & 7;
    *Val = 0;
    Got = HalGetBusDataByOffset(PCIConfiguration, Bus, Slot.u.AsULONG, Val, Off, Len);
    return Got == Len ? 0 : -1;
}

int LX_ABI nt_pci_cfg_write(unsigned int Bus, unsigned int DevFn, unsigned int Off, unsigned int Len, unsigned int Val)
{
    PCI_SLOT_NUMBER Slot;
    ULONG Got;

    Slot.u.AsULONG = 0;
    Slot.u.bits.DeviceNumber = DevFn >> 3;
    Slot.u.bits.FunctionNumber = DevFn & 7;
    Got = HalSetBusDataByOffset(PCIConfiguration, Bus, Slot.u.AsULONG, &Val, Off, Len);
    return Got == Len ? 0 : -1;
}


/* ---- interrupts (legacy line through the HAL) and firmware files ----------------------------------- */

typedef struct _LKPI_IRQ
{
    PKINTERRUPT Obj;
    int Irq;
    void *Dev;
    int (LX_ABI *Handler)(int, void *);
    int (LX_ABI *Thread)(int, void *);
    KDPC Dpc;
    BOOLEAN ThreadPending;
} LKPI_IRQ;

static VOID NTAPI LkpiIrqDpc(PKDPC Dpc, PVOID Ctx, PVOID A1, PVOID A2)
{
    LKPI_IRQ *Q = Ctx;
    BOOLEAN Pending = Q->ThreadPending;

    Q->ThreadPending = FALSE;
    if (Pending && Q->Thread) Q->Thread(Q->Irq, Q->Dev);
}

static BOOLEAN NTAPI LkpiIsr(PKINTERRUPT Interrupt, PVOID Ctx)
{
    LKPI_IRQ *Q = Ctx;
    int R = Q->Handler(Q->Irq, Q->Dev);       /* IRQ_NONE 0, IRQ_HANDLED 1, IRQ_WAKE_THREAD 2 */

    if (R == 2 && Q->Thread)
    {
        Q->ThreadPending = TRUE;
        KeInsertQueueDpc(&Q->Dpc, NULL, NULL);
    }
    return R != 0;
}

void * LX_ABI nt_irq_connect(int Irq, int (LX_ABI *Handler)(int, void *), int (LX_ABI *Thread)(int, void *), void *Dev, int Shared)
{
    LKPI_IRQ *Q = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Q), LKPI_TAG);
    ULONG Vector;
    KIRQL Irql;
    KAFFINITY Affinity;
    NTSTATUS Status;

    if (!Q) return NULL;
    RtlZeroMemory(Q, sizeof(*Q));
    Q->Irq = Irq; Q->Dev = Dev; Q->Handler = Handler; Q->Thread = Thread;
    KeInitializeDpc(&Q->Dpc, LkpiIrqDpc, Q);
    Vector = HalGetInterruptVector(PCIBus, 0, (ULONG)Irq, (ULONG)Irq, &Irql, &Affinity);
    if (!Vector)
    {
        DbgPrint("LKPI: no interrupt vector for line %d\n", Irq);
        ExFreePoolWithTag(Q, LKPI_TAG);
        return NULL;
    }
    Status = IoConnectInterrupt(&Q->Obj, LkpiIsr, Q, NULL, Vector, Irql, Irql, LevelSensitive, Shared ? TRUE : FALSE, Affinity, FALSE);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("LKPI: IoConnectInterrupt failed %08lx\n", Status);
        ExFreePoolWithTag(Q, LKPI_TAG);
        return NULL;
    }
    DbgPrint("LKPI: interrupt line %d connected (vector %lu, irql %u)\n", Irq, Vector, (unsigned)Irql);
    return Q;
}

void LX_ABI nt_irq_disconnect(void *Handle)
{
    LKPI_IRQ *Q = Handle;
    if (!Q) return;
    IoDisconnectInterrupt(Q->Obj);
    KeRemoveQueueDpc(&Q->Dpc);
    ExFreePoolWithTag(Q, LKPI_TAG);
}

/* Reads \SystemRoot\system32\firmware\<name> (name uses '/' separators) into an nt_alloc'd buffer. */
void * LX_ABI nt_read_firmware(const char *Name, lx_ulong *Size)
{
    WCHAR Path[260];
    UNICODE_STRING Us;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Iosb;
    FILE_STANDARD_INFORMATION Info;
    HANDLE File;
    PVOID Buf = NULL;
    NTSTATUS Status;
    ULONG i, n;

    static const WCHAR Prefix[] = L"\\SystemRoot\\system32\\firmware\\";

    RtlCopyMemory(Path, Prefix, sizeof(Prefix));
    n = RTL_NUMBER_OF(Prefix) - 1;
    for (i = 0; Name[i] && n < RTL_NUMBER_OF(Path) - 1; i++) Path[n++] = Name[i] == '/' ? L'\\' : (WCHAR)Name[i];
    Path[n] = 0;
    RtlInitUnicodeString(&Us, Path);
    InitializeObjectAttributes(&Oa, &Us, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_READ | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(Status)) { DbgPrint("LKPI: firmware %s not found (%08lx)\n", Name, Status); return NULL; }
    Status = ZwQueryInformationFile(File, &Iosb, &Info, sizeof(Info), FileStandardInformation);
    if (NT_SUCCESS(Status) && Info.EndOfFile.QuadPart > 0 && Info.EndOfFile.QuadPart < 0x10000000)
    {
        Buf = nt_alloc((lx_ulong)Info.EndOfFile.QuadPart, 0);
        if (Buf && NT_SUCCESS(ZwReadFile(File, NULL, NULL, NULL, &Iosb, Buf, (ULONG)Info.EndOfFile.QuadPart, NULL, NULL)))
            *Size = (lx_ulong)Info.EndOfFile.QuadPart;
        else { nt_free(Buf); Buf = NULL; }
    }
    ZwClose(File);
    return Buf;
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
    { "nt_pci_cfg_read", (PVOID)nt_pci_cfg_read },
    { "nt_pci_cfg_write", (PVOID)nt_pci_cfg_write },
    { "nt_irq_connect", (PVOID)nt_irq_connect },
    { "nt_irq_disconnect", (PVOID)nt_irq_disconnect },
    { "nt_read_firmware", (PVOID)nt_read_firmware },
    { "nt_current_tid", (PVOID)nt_current_tid },
    { "nt_raise_dispatch", (PVOID)nt_raise_dispatch },
    { "nt_lower_irql", (PVOID)nt_lower_irql },
    { "nt_sleep_ms", (PVOID)nt_sleep_ms },
    { "nt_stall_us", (PVOID)nt_stall_us },
    { "nt_time_ms", (PVOID)nt_time_ms },
    { "nt_dbgprint", (PVOID)nt_dbgprint },
    { "nt_register_tick", (PVOID)nt_register_tick },
};

const ULONG LkpiExportCount = RTL_NUMBER_OF(LkpiExports);
