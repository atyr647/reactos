/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     \Device\LKPIDRMn: user-mode access to the DRM devices registered by Linux drivers.
 *              Create -> drm_open(), IOCTL_LKPI_DRM -> the driver's unlocked_ioctl, Close -> drm_release().
 */

#include "linuxkpi.h"
#include "lkpi_drm.h"
#include <ntstrsafe.h>
#include <pseh/pseh2.h>

typedef void * (LX_ABI *PFN_OPEN)(int Minor);
typedef long (LX_ABI *PFN_IOCTL)(void *H, unsigned int Cmd, void *Arg);
typedef void (LX_ABI *PFN_CLOSE)(void *H);
typedef void * (LX_ABI *PFN_MMAP)(void *File, unsigned long long Offset, unsigned long Size, ULONG64 *Pfn, unsigned int *Cache,
                                 void *Host);
typedef void (LX_ABI *PFN_MUNMAP)(void *Cookie);
typedef void (LX_ABI *PFN_REMAP)(void *Host, ULONG64 *Pfn, unsigned long Npages, unsigned int Cache);
typedef void (LX_ABI *PFN_SET_REMAP)(PFN_REMAP Fn);
typedef int (LX_ABI *PFN_INFO)(void *H, LKPI_DRM_INFO *Info);
/* kpi/kpi2_file.c, kpi/kpi2_drm.c: Linux files (struct file *) */
typedef void * (LX_ABI *PFN_HANDLE_FILE)(void *H);
typedef long (LX_ABI *PFN_FILE_IOCTL)(void *File, unsigned int Cmd, void *Arg);
typedef unsigned int (LX_ABI *PFN_FILE_POLL)(void *File, unsigned int Events, int TimeoutMs);
typedef void (LX_ABI *PFN_FILE_REF)(void *File);

C_ASSERT(sizeof(LKPI_DRM_INFO) == 64);      /* struct kpi_drm_info in kpi/kpi2_drm.c */

/* ReactOS kernel extension (ntoskrnl/mm/ARM3/mdlsup.c): repoint a user mapping made from an MDL at other frames */
NTKERNELAPI NTSTATUS NTAPI MmReplaceLockedPagesInUserSpace(PVOID BaseAddress, PMDL NewMdl, MEMORY_CACHING_TYPE CacheType);

static PFN_OPEN DrmOpen;
static PFN_IOCTL DrmIoctl;
static PFN_CLOSE DrmClose;
static PFN_MMAP DrmMmap;
static PFN_MUNMAP DrmMunmap;
static PFN_INFO DrmInfo;
static PFN_HANDLE_FILE DrmHandleFile;
static PFN_FILE_IOCTL FileIoctl;
static PFN_FILE_POLL FilePoll;
static PFN_FILE_REF FileGet, FilePut;

/* \Device\LkpiFile: Linux files (dma-bufs, sync files, syncobj fds) handed to user mode as handles */
static PDEVICE_OBJECT LkpiFileDevice;
#define LKPI_FILE_MINOR (-1)

typedef struct _DRM_DEVEXT { int Minor; } DRM_DEVEXT, *PDRM_DEVEXT;

static BOOLEAN DrmPfnIsRam(ULONG64 Pfn);
static void LX_ABI DrmRemap(void *Host, ULONG64 *Pfn, unsigned long Npages, unsigned int Cache);

#define LKPI_DRM_MAX_MAP (4ULL << 30)

/* A user mapping of a DRM buffer (IOCTL_LKPI_DRM_MMAP) */
typedef struct _DRM_MAP
{
    LIST_ENTRY Link;            /* DRM_FILE.Maps */
    LIST_ENTRY AllLink;         /* DrmAllMaps */
    PVOID Address;
    PMDL Mdl;
    PVOID Cookie;               /* kpi_drm_mmap() */
    PEPROCESS Process;          /* referenced */
    ULONG64 *Pfn;               /* the frames, kept current by Linux across buffer moves */
    ULONG64 Pages;
    KMUTEX Lock;                /* remap (buffer moved) vs unmap */
    BOOLEAN Dead;
} DRM_MAP, *PDRM_MAP;

/*
 * FileObject->FsContext of an open DRM node, or of a Linux file shared through a handle (\Device\LkpiFile: the
 * handle is the Linux fd). ioctl, mmap and poll go to File either way.
 */
typedef struct _DRM_FILE
{
    PVOID Handle;               /* kpi_drm_open(), DRM nodes only */
    PVOID File;                 /* the struct file; for LkpiFile set by nt_fd_install() */
    BOOLEAN OwnsFile;           /* this file object holds a reference to File (LkpiFile) */
    LIST_ENTRY Maps;
} DRM_FILE, *PDRM_FILE;

/* All mappings, for process exit; DrmMapLock guards both lists a mapping is on */
static LIST_ENTRY DrmAllMaps;
static FAST_MUTEX DrmMapLock;

static VOID
DrmUnlinkMap(PDRM_MAP M)
{
    RemoveEntryList(&M->Link);
    RemoveEntryList(&M->AllLink);
}

static BOOLEAN
DrmResolve(VOID)
{
    if (!DrmOpen) DrmOpen = (PFN_OPEN)LkpiLookupModuleSymbol("kpi_drm_open");
    if (!DrmIoctl) DrmIoctl = (PFN_IOCTL)LkpiLookupModuleSymbol("kpi_drm_ioctl");
    if (!DrmClose) DrmClose = (PFN_CLOSE)LkpiLookupModuleSymbol("kpi_drm_close");
    if (!DrmMmap) DrmMmap = (PFN_MMAP)LkpiLookupModuleSymbol("kpi_drm_mmap");
    if (!DrmMunmap) DrmMunmap = (PFN_MUNMAP)LkpiLookupModuleSymbol("kpi_drm_munmap");
    if (!DrmInfo) DrmInfo = (PFN_INFO)LkpiLookupModuleSymbol("kpi_drm_info");
    if (!DrmHandleFile) DrmHandleFile = (PFN_HANDLE_FILE)LkpiLookupModuleSymbol("kpi_drm_handle_file");
    if (!FileIoctl) FileIoctl = (PFN_FILE_IOCTL)LkpiLookupModuleSymbol("kpi_file_ioctl");
    if (!FilePoll) FilePoll = (PFN_FILE_POLL)LkpiLookupModuleSymbol("kpi_file_poll");
    if (!FileGet) FileGet = (PFN_FILE_REF)LkpiLookupModuleSymbol("kpi_file_get");
    if (!FilePut) FilePut = (PFN_FILE_REF)LkpiLookupModuleSymbol("kpi_file_put");
    {
        static BOOLEAN RemapSet;
        PFN_SET_REMAP SetRemap = (PFN_SET_REMAP)LkpiLookupModuleSymbol("kpi_drm_set_remap");

        if (!RemapSet && SetRemap)
        {
            SetRemap(DrmRemap);
            RemapSet = TRUE;
        }
    }
    /* the bridge (kpi2.ko) is always there; the DRM core itself (drm.ko) only when a DRM driver ships. Without it
     * drm_open() would be an unresolved-import stub, so there is no device to offer. */
    return DrmOpen && DrmIoctl && DrmClose && LkpiLookupModuleSymbol("drm_open");
}

NTSTATUS NTAPI
DrmCreate(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    PDRM_DEVEXT Ext = Dev->DeviceExtension;
    NTSTATUS Status = STATUS_SUCCESS;
    PDRM_FILE F = NULL;

    if (!DrmResolve()) Status = STATUS_NO_SUCH_DEVICE;
    else if (!(F = ExAllocatePoolZero(NonPagedPool, sizeof(*F), LKPI_TAG))) Status = STATUS_INSUFFICIENT_RESOURCES;
    else if (Ext->Minor == LKPI_FILE_MINOR)
    {
        InitializeListHead(&F->Maps);      /* an empty fd slot: nt_fd_install() fills it */
    }
    else
    {
        InitializeListHead(&F->Maps);
        F->Handle = DrmOpen(Ext->Minor);
        if (F->Handle && DrmHandleFile)
            F->File = DrmHandleFile(F->Handle);
        if (!F->Handle)
        {
            ExFreePoolWithTag(F, LKPI_TAG);
            F = NULL;
            Status = STATUS_NO_SUCH_DEVICE;
        }
    }
    Sp->FileObject->FsContext = F;
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/* Remove a user mapping: in its process (attaching if needed), then let the driver drop its references */
static VOID
DrmUnmap(PDRM_MAP M)
{
    KAPC_STATE Apc;
    BOOLEAN Attached = FALSE;

    /* no more remaps (one in progress finishes first: it holds the lock) */
    KeWaitForSingleObject(&M->Lock, Executive, KernelMode, FALSE, NULL);
    M->Dead = TRUE;
    KeReleaseMutex(&M->Lock, FALSE);
    if (PsGetCurrentProcess() != M->Process)
    {
        KeStackAttachProcess((PKPROCESS)M->Process, &Apc);
        Attached = TRUE;
    }
    MmUnmapLockedPages(M->Address, M->Mdl);
    if (Attached)
        KeUnstackDetachProcess(&Apc);
    M->Mdl->MdlFlags &= ~MDL_PAGES_LOCKED;     /* the pages were never probed and locked by us */
    IoFreeMdl(M->Mdl);
    DrmMunmap(M->Cookie);                       /* waits for a pending refresh, then drops pages and buffer */
    ObDereferenceObject(M->Process);
    ExFreePoolWithTag(M->Pfn, LKPI_TAG);
    ExFreePoolWithTag(M, LKPI_TAG);
}

/* An MDL describing exactly the given page frames (no virtual range behind it) */
static PMDL
DrmBuildMdl(ULONG64 *Pfn, ULONG64 Pages)
{
    PMDL Mdl = IoAllocateMdl((PVOID)(ULONG_PTR)PAGE_SIZE, (ULONG)(Pages << PAGE_SHIFT), FALSE, FALSE, NULL);
    PPFN_NUMBER MdlPfn;
    BOOLEAN Io = FALSE;
    ULONG64 i;

    if (!Mdl)
        return NULL;
    MdlPfn = MmGetMdlPfnArray(Mdl);
    for (i = 0; i < Pages; i++)
    {
        MdlPfn[i] = (PFN_NUMBER)Pfn[i];
        if (!Io && !DrmPfnIsRam(Pfn[i]))
            Io = TRUE;
    }
    Mdl->MdlFlags |= MDL_PAGES_LOCKED | (Io ? MDL_IO_SPACE : 0);
    return Mdl;
}

static MEMORY_CACHING_TYPE
DrmCacheType(unsigned int Cache)
{
    return Cache == 1 ? MmWriteCombined : Cache == 2 ? MmNonCached : MmCached;
}

/* Linux worker thread (PASSIVE_LEVEL): the buffer behind a mapping moved; repoint the user PTEs, same address */
static DECLSPEC_NOINLINE VOID
DrmRemapWorker(PVOID Host, ULONG64 *Pfn, ULONG Npages, ULONG Cache)
{
    PDRM_MAP M = Host;
    KAPC_STATE Apc;
    PMDL Mdl;
    NTSTATUS Status;

    KeWaitForSingleObject(&M->Lock, Executive, KernelMode, FALSE, NULL);
    if (M->Dead || Npages != M->Pages || !(Mdl = DrmBuildMdl(Pfn, Npages)))
    {
        KeReleaseMutex(&M->Lock, FALSE);
        return;
    }
    KeStackAttachProcess((PKPROCESS)M->Process, &Apc);
    Status = MmReplaceLockedPagesInUserSpace(M->Address, Mdl, DrmCacheType(Cache));
    KeUnstackDetachProcess(&Apc);
    if (NT_SUCCESS(Status))
    {
        PMDL Old = M->Mdl;

        M->Mdl = Mdl;
        Mdl = Old;
    }
    else
    {
        DbgPrint("LKPI: DRM mapping %p could not follow its buffer (%08lx)\n", M->Address, Status);
    }
    Mdl->MdlFlags &= ~MDL_PAGES_LOCKED;
    IoFreeMdl(Mdl);
    KeReleaseMutex(&M->Lock, FALSE);
}

/* sysv_abi entry point for Linux: a thin trampoline (GCC cannot always mix the ABIs in one large frame) */
static void LX_ABI
DrmRemap(void *Host, ULONG64 *Pfn, unsigned long Npages, unsigned int Cache)
{
    DrmRemapWorker(Host, Pfn, (ULONG)Npages, Cache);
}

/* Is the page frame ordinary RAM (else device memory: VRAM through a PCI BAR)? */
static BOOLEAN
DrmPfnIsRam(ULONG64 Pfn)
{
    static PPHYSICAL_MEMORY_RANGE Ranges;
    ULONG64 Pa = Pfn << PAGE_SHIFT;
    ULONG i;

    if (!Ranges)
        Ranges = MmGetPhysicalMemoryRanges();   /* kept: the RAM layout does not change */
    if (!Ranges)
        return FALSE;
    for (i = 0; Ranges[i].BaseAddress.QuadPart || Ranges[i].NumberOfBytes.QuadPart; i++)
        if (Pa >= (ULONG64)Ranges[i].BaseAddress.QuadPart &&
            Pa - Ranges[i].BaseAddress.QuadPart < (ULONG64)Ranges[i].NumberOfBytes.QuadPart)
            return TRUE;
    return FALSE;
}

static NTSTATUS
DrmMap(PDRM_FILE F, LKPI_DRM_MMAP *Req)
{
    ULONG64 Pages, *Pfn;
    unsigned int Cache = 0;
    PDRM_MAP M;
    PVOID Cookie, Va = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!DrmMmap || !DrmMunmap)
        return STATUS_NOT_SUPPORTED;
    if (!Req->Size || (Req->Size & (PAGE_SIZE - 1)) || (Req->Offset & (PAGE_SIZE - 1)) || Req->Size > LKPI_DRM_MAX_MAP)
        return STATUS_INVALID_PARAMETER;
    Pages = Req->Size >> PAGE_SHIFT;
    Pfn = ExAllocatePoolWithTag(NonPagedPool, Pages * sizeof(*Pfn), LKPI_TAG);
    M = ExAllocatePoolWithTag(NonPagedPool, sizeof(*M), LKPI_TAG);
    if (!Pfn || !M)
    {
        if (Pfn) ExFreePoolWithTag(Pfn, LKPI_TAG);
        if (M) ExFreePoolWithTag(M, LKPI_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(M, sizeof(*M));
    KeInitializeMutex(&M->Lock, 0);
    M->Pfn = Pfn;
    M->Pages = Pages;
    M->Process = PsGetCurrentProcess();
    /* hold the lock until the mapping exists: a move right after kpi_drm_mmap() must not remap a missing range */
    KeWaitForSingleObject(&M->Lock, Executive, KernelMode, FALSE, NULL);
    Cookie = DrmMmap(F->File, Req->Offset, (unsigned long)Req->Size, Pfn, &Cache, M);
    if ((ULONG_PTR)Cookie >= (ULONG_PTR)-4095)      /* ERR_PTR */
    {
        DbgPrint("LKPI: DRM mmap of offset %I64x (%I64u bytes) failed (errno %Id)\n", Req->Offset, Req->Size,
                 (LONG_PTR)Cookie);
        KeReleaseMutex(&M->Lock, FALSE);
        ExFreePoolWithTag(Pfn, LKPI_TAG);
        ExFreePoolWithTag(M, LKPI_TAG);
        return (LONG_PTR)Cookie == -13 ? STATUS_ACCESS_DENIED : STATUS_INVALID_PARAMETER;
    }
    M->Mdl = DrmBuildMdl(Pfn, Pages);
    if (!M->Mdl)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }
    _SEH2_TRY
    {
        Va = MmMapLockedPagesSpecifyCache(M->Mdl, UserMode, DrmCacheType(Cache), NULL, FALSE, NormalPagePriority);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Va = NULL;
    }
    _SEH2_END;
    if (!Va)
    {
        Status = STATUS_NO_MEMORY;
        M->Mdl->MdlFlags &= ~MDL_PAGES_LOCKED;
        IoFreeMdl(M->Mdl);
        goto Fail;
    }
    M->Address = Va;
    M->Cookie = Cookie;
    ObReferenceObject(M->Process);
    KeReleaseMutex(&M->Lock, FALSE);
    ExAcquireFastMutex(&DrmMapLock);
    InsertTailList(&F->Maps, &M->Link);
    InsertTailList(&DrmAllMaps, &M->AllLink);
    ExReleaseFastMutex(&DrmMapLock);
    Req->Address = (ULONG_PTR)Va;
    return STATUS_SUCCESS;
Fail:
    M->Dead = TRUE;
    KeReleaseMutex(&M->Lock, FALSE);
    DrmMunmap(Cookie);
    ExFreePoolWithTag(Pfn, LKPI_TAG);
    ExFreePoolWithTag(M, LKPI_TAG);
    return Status;
}

static NTSTATUS
DrmUnmapAddress(PDRM_FILE F, ULONG64 Address)
{
    PLIST_ENTRY E;
    PDRM_MAP Found = NULL;

    ExAcquireFastMutex(&DrmMapLock);
    for (E = F->Maps.Flink; E != &F->Maps; E = E->Flink)
    {
        PDRM_MAP M = CONTAINING_RECORD(E, DRM_MAP, Link);

        if ((ULONG_PTR)M->Address == Address && M->Process == PsGetCurrentProcess())
        {
            DrmUnlinkMap(M);
            Found = M;
            break;
        }
    }
    ExReleaseFastMutex(&DrmMapLock);
    if (!Found)
        return STATUS_INVALID_PARAMETER;
    DrmUnmap(Found);
    return STATUS_SUCCESS;
}

/* IRP_MJ_CLEANUP (last handle closed, in the process that closed it): drop the mappings and the DRM file.
 * IRP_MJ_CLOSE (last reference gone): free what is left. */
NTSTATUS NTAPI
DrmClean(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    PDRM_FILE F = Sp->FileObject->FsContext;

    if (F && Sp->MajorFunction == IRP_MJ_CLEANUP)
    {
        for (;;)
        {
            PDRM_MAP M = NULL;

            ExAcquireFastMutex(&DrmMapLock);
            if (!IsListEmpty(&F->Maps))
            {
                M = CONTAINING_RECORD(F->Maps.Flink, DRM_MAP, Link);
                DrmUnlinkMap(M);
            }
            ExReleaseFastMutex(&DrmMapLock);
            if (!M)
                break;
            DrmUnmap(M);
        }
        if (F->Handle && DrmClose)
            DrmClose(F->Handle);
        F->Handle = NULL;
        if (!F->OwnsFile)
            F->File = NULL;             /* a DRM node's file went with its handle */
    }
    else if (F && Sp->MajorFunction == IRP_MJ_CLOSE)
    {
        /* the last reference to a shared Linux file: Linux releases it (a dma-buf, a sync file, ...) once no fd,
         * mapping or importer holds it any more */
        if (F->OwnsFile && F->File && FilePut)
            FilePut(F->File);
        Sp->FileObject->FsContext = NULL;
        ExFreePoolWithTag(F, LKPI_TAG);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/*
 * poll() of one Linux file. Waits in slices so that a thread being terminated is not held in the kernel (Linux poll
 * is interrupted by signals the same way). TimeoutMs < 0: no limit.
 */
static unsigned int
DrmPoll(PVOID File, unsigned int Events, int TimeoutMs)
{
    ULONGLONG Start = KeQueryInterruptTime();
    unsigned int Revents;

    for (;;)
    {
        int Slice = TimeoutMs < 0 ? 100 : TimeoutMs;
        LONGLONG Left;

        if (Slice > 100)
            Slice = 100;
        Revents = FilePoll(File, Events, Slice);
        if (Revents || TimeoutMs == 0 || PsIsThreadTerminating(PsGetCurrentThread()))
            return Revents;
        if (TimeoutMs > 0)
        {
            Left = TimeoutMs - (LONGLONG)((KeQueryInterruptTime() - Start) / 10000);
            if (Left <= 0)
                return 0;
            TimeoutMs = (int)Left;
        }
    }
}

/*
 * Linux file descriptors (kpi/kpi2_file.c): a reserved fd is a handle, in the calling process, to an empty
 * \Device\LkpiFile file object; installing a file puts it there. The handle value is the fd number Linux reports
 * back (fds are small non-negative ints; NT handle values are multiples of 4 well below 2^31).
 */
int LX_ABI
nt_fd_reserve(void)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\LkpiFile");
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Iosb;
    HANDLE H;

    if (!LkpiFileDevice || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return -24;     /* EMFILE */
    /* not OBJ_KERNEL_HANDLE: the handle belongs to the process that made the request */
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&H, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &Oa, &Iosb, NULL, 0,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0)))
        return -24;
    if ((ULONG_PTR)H > 0x7FFFFFFF)
    {
        ZwClose(H);
        return -24;
    }
    return (int)(ULONG_PTR)H;
}

void LX_ABI
nt_fd_unreserve(int Fd)
{
    if (Fd > 0)
        ZwClose((HANDLE)(ULONG_PTR)Fd);
}

/* the DRM_FILE of an LkpiFile handle of the current process (referenced: ObDereferenceObject(*Fo)), or NULL */
static PDRM_FILE
LkpiFileFromFd(int Fd, KPROCESSOR_MODE Mode, PFILE_OBJECT *Fo)
{
    if (Fd <= 0 || !LkpiFileDevice)
        return NULL;
    if (!NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)Fd, 0, *IoFileObjectType, Mode, (PVOID *)Fo, NULL)))
        return NULL;
    if ((*Fo)->DeviceObject != LkpiFileDevice || !(*Fo)->FsContext)
    {
        ObDereferenceObject(*Fo);
        return NULL;
    }
    return (*Fo)->FsContext;
}

/* 0, or -EBADF if Fd is not an empty reserved slot (the caller then drops its reference) */
int LX_ABI
nt_fd_install(int Fd, void *File)
{
    PFILE_OBJECT Fo;
    PDRM_FILE F = LkpiFileFromFd(Fd, KernelMode, &Fo);
    int r = -9;

    if (!F)
        return r;
    if (!InterlockedCompareExchangePointer(&F->File, File, NULL))
    {
        F->OwnsFile = TRUE;     /* the reference fd_install() hands over */
        r = 0;
    }
    ObDereferenceObject(Fo);
    return r;
}

/* fget(): the file behind a handle the caller passed (user-mode handles only), with a reference; or NULL */
void * LX_ABI
nt_fd_get(int Fd)
{
    PFILE_OBJECT Fo;
    PDRM_FILE F = LkpiFileFromFd(Fd, UserMode, &Fo);
    PVOID File = NULL;

    if (!F)
        return NULL;
    if (F->OwnsFile && F->File && FileGet)
    {
        File = F->File;
        FileGet(File);
    }
    ObDereferenceObject(Fo);
    return File;
}

NTSTATUS NTAPI
DrmControl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Info = 0;
    PDRM_FILE F = Sp->FileObject->FsContext;
    ULONG Code = Sp->Parameters.DeviceIoControl.IoControlCode;

    if (Code == IOCTL_LKPI_DRM_INFO && (!F || !F->Handle))
        Status = STATUS_INVALID_HANDLE;
    else if ((Code == IOCTL_LKPI_DRM_MMAP || Code == IOCTL_LKPI_DRM_MUNMAP || Code == IOCTL_LKPI_FILE_POLL) &&
             (!F || !F->File))
        Status = STATUS_INVALID_HANDLE;
    else if (Code == IOCTL_LKPI_FILE_POLL)
    {
        LKPI_FILE_POLL *Req = Irp->AssociatedIrp.SystemBuffer;

        if (Sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(*Req) ||
            Sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(*Req))
            Status = STATUS_BUFFER_TOO_SMALL;
        else if (!FilePoll)
            Status = STATUS_NOT_SUPPORTED;
        else
        {
            Req->Revents = DrmPoll(F->File, Req->Events, Req->TimeoutMs);
            Status = STATUS_SUCCESS;
            Info = sizeof(*Req);
        }
    }
    else if (Code == IOCTL_LKPI_DRM_INFO)
    {
        LKPI_DRM_INFO *Req = Irp->AssociatedIrp.SystemBuffer;

        if (Sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(*Req))
            Status = STATUS_BUFFER_TOO_SMALL;
        else if (!DrmInfo)
            Status = STATUS_NOT_SUPPORTED;
        else if (DrmInfo(F->Handle, Req) != 0)
            Status = STATUS_NO_SUCH_DEVICE;
        else
        {
            Req->Driver[sizeof(Req->Driver) - 1] = 0;
            Status = STATUS_SUCCESS;
            Info = sizeof(*Req);
        }
    }
    else if (Code == IOCTL_LKPI_DRM_MMAP)
    {
        LKPI_DRM_MMAP *Req = Irp->AssociatedIrp.SystemBuffer;

        if (Sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(*Req) ||
            Sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(*Req))
            Status = STATUS_BUFFER_TOO_SMALL;
        else if (NT_SUCCESS(Status = DrmMap(F, Req)))
            Info = sizeof(*Req);
    }
    else if (Code == IOCTL_LKPI_DRM_MUNMAP)
    {
        LKPI_DRM_MMAP *Req = Irp->AssociatedIrp.SystemBuffer;

        Status = Sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(*Req) ? STATUS_BUFFER_TOO_SMALL :
                 DrmUnmapAddress(F, Req->Address);
    }
    else if (Code == IOCTL_LKPI_DRM)
    {
        LKPI_DRM_IOCTL *Io = Irp->AssociatedIrp.SystemBuffer;
        ULONG InLen = Sp->Parameters.DeviceIoControl.InputBufferLength;
        ULONG OutLen = Sp->Parameters.DeviceIoControl.OutputBufferLength;
        ULONG Hdr = FIELD_OFFSET(LKPI_DRM_IOCTL, Data);

        /* the Linux ioctl reads and writes exactly _IOC_SIZE(Cmd) bytes of its argument */
        if (!Io || InLen < Hdr || Io->Size > InLen - Hdr || OutLen < Hdr + Io->Size || !F ||
            (!F->Handle && !(F->File && FileIoctl)) || Io->Size != ((Io->Cmd >> 16) & 0x3FFF))
            Status = STATUS_INVALID_PARAMETER;
        else
        {
            /* the DRM core copies arguments with memcpy (copy_from_user), so give it a 16-byte aligned kernel copy */
            ULONG Size = Io->Size;
            PUCHAR Tmp = ExAllocatePoolWithTag(NonPagedPool, Size + 16, LKPI_TAG);

            if (!Tmp) Status = STATUS_INSUFFICIENT_RESOURCES;
            else if (!LkpiUserArgEnter(ALIGN_UP_POINTER_BY(Tmp, 16), Size))
            {
                ExFreePoolWithTag(Tmp, LKPI_TAG);
                Status = STATUS_DEVICE_BUSY;
            }
            else
            {
                /* nested pointers inside the argument stay user addresses of this process (checked by nt_copy_user) */
                PUCHAR Arg = (PUCHAR)ALIGN_UP_POINTER_BY(Tmp, 16);
                RtlCopyMemory(Arg, Io->Data, Size);
                Io->Result = F->Handle ? (long)DrmIoctl(F->Handle, (unsigned int)Io->Cmd, Arg) :
                                         (long)FileIoctl(F->File, (unsigned int)Io->Cmd, Arg);
                LkpiUserArgLeave();
                RtlCopyMemory(Io->Data, Arg, Size);
                ExFreePoolWithTag(Tmp, LKPI_TAG);
                Info = Hdr + Size;
                Status = STATUS_SUCCESS;
            }
        }
    }
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/*
 * A process that exits with mappings left: the handle may live on in another process (duplicated, inherited), so
 * IRP_MJ_CLEANUP need not come before the address space goes, and Mm does not tear down device mappings itself.
 * Process notify routines run in the exiting process's last thread, before its handles and address space go.
 */
static VOID NTAPI
DrmProcessNotify(HANDLE ParentId, HANDLE ProcessId, BOOLEAN Create)
{
    for (;;)
    {
        PDRM_MAP M = NULL;
        PLIST_ENTRY E;

        if (Create)
            return;
        ExAcquireFastMutex(&DrmMapLock);
        for (E = DrmAllMaps.Flink; E != &DrmAllMaps; E = E->Flink)
            if (PsGetProcessId(CONTAINING_RECORD(E, DRM_MAP, AllLink)->Process) == ProcessId)
            {
                M = CONTAINING_RECORD(E, DRM_MAP, AllLink);
                DrmUnlinkMap(M);
                break;
            }
        ExReleaseFastMutex(&DrmMapLock);
        if (!M)
            return;
        DbgPrint("LKPI: process %p exits with a DRM mapping at %p: removed\n", ProcessId, M->Address);
        DrmUnmap(M);
    }
}

NTSTATUS
LkpiCreateDrmNodes(PDRIVER_OBJECT DriverObject)
{
    static const int Minors[] = { 0, 1, 2, 3, 128, 129, 130, 131 };    /* up to four cards (opens of absent ones fail) */
    static BOOLEAN Created, MapsReady;
    ULONG i;

    if (Created)
        return STATUS_SUCCESS;

    if (!DrmResolve())
    {
        DbgPrint("LKPI: no Linux DRM core loaded, no DRM nodes\n");
        return STATUS_SUCCESS;
    }
    if (!LkpiFileDevice)
    {
        UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\LkpiFile");
        PDEVICE_OBJECT Dev;

        /* character device as the DRM nodes, so that the C runtime takes its handles as fds */
        if (NT_SUCCESS(IoCreateDevice(DriverObject, sizeof(DRM_DEVEXT), &Name, FILE_DEVICE_SCREEN, 0, FALSE, &Dev)))
        {
            ((PDRM_DEVEXT)Dev->DeviceExtension)->Minor = LKPI_FILE_MINOR;
            Dev->Flags |= DO_BUFFERED_IO;
            Dev->Flags &= ~DO_DEVICE_INITIALIZING;
            LkpiFileDevice = Dev;
        }
        else
        {
            DbgPrint("LKPI: cannot create \\Device\\LkpiFile: no fd sharing\n");
        }
    }
    if (!MapsReady)
    {
        InitializeListHead(&DrmAllMaps);
        ExInitializeFastMutex(&DrmMapLock);
        if (!NT_SUCCESS(PsSetCreateProcessNotifyRoutine(DrmProcessNotify, FALSE)))
            DbgPrint("LKPI: no process notify routine: a process must unmap DRM buffers before it exits\n");
        MapsReady = TRUE;
    }

    for (i = 0; i < RTL_NUMBER_OF(Minors); i++)
    {
        WCHAR NameBuf[48], LinkBuf[48];
        UNICODE_STRING Name, Link;
        PDEVICE_OBJECT Dev;
        NTSTATUS Status;

        RtlStringCbPrintfW(NameBuf, sizeof(NameBuf), L"\\Device\\LKPIDRM%d", Minors[i]);
        RtlStringCbPrintfW(LinkBuf, sizeof(LinkBuf), L"\\DosDevices\\LKPIDRM%d", Minors[i]);
        RtlInitUnicodeString(&Name, NameBuf);
        RtlInitUnicodeString(&Link, LinkBuf);
        /* a character device, as on Linux: GetFileType() says FILE_TYPE_CHAR for FILE_DEVICE_SCREEN, so the C runtime's
         * open()/_open_osfhandle() accept the node (they refuse FILE_TYPE_UNKNOWN) and libdrm/Mesa use plain fds */
        Status = IoCreateDevice(DriverObject, sizeof(DRM_DEVEXT), &Name, FILE_DEVICE_SCREEN, 0, FALSE, &Dev);
        if (!NT_SUCCESS(Status)) { DbgPrint("LKPI: IoCreateDevice(%wZ) failed %08lx\n", &Name, Status); continue; }
        ((PDRM_DEVEXT)Dev->DeviceExtension)->Minor = Minors[i];
        Dev->Flags |= DO_BUFFERED_IO;
        Dev->Flags &= ~DO_DEVICE_INITIALIZING;
        IoCreateSymbolicLink(&Link, &Name);
        DbgPrint("LKPI: DRM node %wZ created\n", &Name);
        Created = TRUE;
    }
    return STATUS_SUCCESS;
}
