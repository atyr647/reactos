/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     Present Linux block devices (nvme0n1, ...) to ReactOS as SCSI disks.
 *
 * linuxkpi.sys acts as a tiny SCSI port driver: one FDO (a legacy-detected bus device) whose BusRelations
 * lists one PDO per Linux disk. disk.sys / partmgr / the filesystems attach on top of those PDOs exactly as they
 * do on scsiport, and talk to us through IRP_MJ_INTERNAL_DEVICE_CONTROL + SRB.
 *
 * Reads and writes are handed to a worker thread (the Linux submit path may sleep) which calls
 * kpi_blk_submit(); the Linux completion arrives in an interrupt, so it only queues a DPC which completes the IRP.
 */

#include "linuxkpi.h"
#include <ntddscsi.h>
#include <ntddstor.h>
#include <scsi.h>
#include <srb.h>
#include <ntstrsafe.h>

#define LKDISK_TAG 'ksDL'
#define LKDISK_MAX 8
#define LKDISK_PORT_NUMBER 10
static LONG TraceCount;
#define LKDISK_TRACE(...) do { if (InterlockedIncrement(&TraceCount) < 400) DbgPrint("LKDISK: " __VA_ARGS__); } while (0)

typedef struct _LKDISK_COMMON
{
    BOOLEAN IsFdo;
    PDEVICE_OBJECT Self;
} LKDISK_COMMON;

typedef struct _LKDISK_PDO LKDISK_PDO, *PLKDISK_PDO;

typedef struct _LKDISK_FDO
{
    LKDISK_COMMON Common;
    PDEVICE_OBJECT Lower;
    ULONG Count;
    PLKDISK_PDO Disks[LKDISK_MAX];
} LKDISK_FDO, *PLKDISK_FDO;

struct _LKDISK_PDO
{
    LKDISK_COMMON Common;
    PLKDISK_FDO Fdo;
    PVOID Gendisk;
    ULONG64 Sectors;
    ULONG Lbs;
    ULONG Index;
    ULONG MaxBytes;
    INQUIRYDATA Inquiry;
};

/* ---- the Linux side ----------------------------------------------------------------------------------- */
typedef int (LX_ABI *PFN_SUBMIT)(void *Disk, int Write, ULONG64 ByteOff, void *Buf, ULONG Len, void (LX_ABI *Done)(void *, int), void *Ctx);
typedef int (LX_ABI *PFN_FLUSH)(void *Disk, void (LX_ABI *Done)(void *, int), void *Ctx);
typedef ULONG64 (LX_ABI *PFN_CAP)(void *Disk);
typedef ULONG (LX_ABI *PFN_LBS)(void *Disk);
typedef void (LX_ABI *PFN_SETHOOK)(void (LX_ABI *Hook)(void *, int));

static PFN_SUBMIT BlkSubmit;
static PFN_FLUSH BlkFlush;
static PFN_CAP BlkCapacity;
static PFN_LBS BlkLbs;

static PDRIVER_OBJECT DiskDriver;
static PLKDISK_FDO Fdo;
static KEVENT WorkEvent;
static KSPIN_LOCK WorkLock;
static LIST_ENTRY IoQueue;                /* IRPs waiting for the submit thread (Tail.Overlay.ListEntry) */
static PVOID NewDisks[LKDISK_MAX];        /* gendisks reported by Linux, not yet turned into PDOs */
static ULONG NewDiskCount;
static BOOLEAN DiskStarted;
static volatile LONG DiskPending;       /* gendisks reported by Linux whose PDO is not created yet */

typedef struct _LKDISK_IO
{
    KDPC Dpc;
    PIRP Irp;
    PSCSI_REQUEST_BLOCK Srb;
    int Err;
} LKDISK_IO, *PLKDISK_IO;

/* The SRB's DataBuffer is a virtual address in the MDL's original address space; map the MDL to get a usable
 * system address (what scsiport does for miniports that ask for mapped buffers). */
static PVOID MappedBuffer(PIRP Irp, PSCSI_REQUEST_BLOCK Srb)
{
    PVOID Base;

    if (!Irp->MdlAddress || !Srb->DataTransferLength)
        return Srb->DataBuffer;
    Base = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
    if (!Base)
        return NULL;
    return (PUCHAR)Base + ((ULONG_PTR)Srb->DataBuffer - (ULONG_PTR)MmGetMdlVirtualAddress(Irp->MdlAddress));
}

/* ---- SRB helpers --------------------------------------------------------------------------------------- */
static ULONG BE32(const UCHAR *p) { return ((ULONG)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static ULONG64 BE64(const UCHAR *p) { return ((ULONG64)BE32(p) << 32) | BE32(p + 4); }
static USHORT BE16(const UCHAR *p) { return (USHORT)((p[0] << 8) | p[1]); }
static VOID PutBE32(UCHAR *p, ULONG v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static VOID PutBE64(UCHAR *p, ULONG64 v) { PutBE32(p, (ULONG)(v >> 32)); PutBE32(p + 4, (ULONG)v); }

static VOID SrbDone(PIRP Irp, PSCSI_REQUEST_BLOCK Srb, UCHAR SrbStatus, ULONG Transferred)
{
    Srb->SrbStatus = SrbStatus;
    Srb->ScsiStatus = (SrbStatus == SRB_STATUS_SUCCESS) ? SCSISTAT_GOOD : SCSISTAT_CHECK_CONDITION;
    if (SrbStatus == SRB_STATUS_SUCCESS || SrbStatus == SRB_STATUS_DATA_OVERRUN)
        Srb->DataTransferLength = Transferred;
    Irp->IoStatus.Status = (SrbStatus == SRB_STATUS_SUCCESS) ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
    Irp->IoStatus.Information = Transferred;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static VOID NTAPI LkDiskIoDpc(PKDPC Dpc, PVOID Ctx, PVOID A1, PVOID A2)
{
    PLKDISK_IO Io = Ctx;
    PIRP Irp = Io->Irp;
    PSCSI_REQUEST_BLOCK Srb = Io->Srb;
    ULONG Len = Srb->DataTransferLength;
    int Err = Io->Err;

    ExFreePoolWithTag(Io, LKDISK_TAG);
    if (Err)
        DbgPrint("LKPI: disk I/O failed (errno %d)\n", Err);
    SrbDone(Irp, Srb, Err ? SRB_STATUS_ERROR : SRB_STATUS_SUCCESS, Err ? 0 : Len);
}

/* Linux completion context (an interrupt): only queue the DPC. */
static void LX_ABI LkDiskIoDone(void *Ctx, int Err)
{
    PLKDISK_IO Io = Ctx;

    Io->Err = Err;
    KeInsertQueueDpc(&Io->Dpc, NULL, NULL);
}

static VOID StartIo(PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    PLKDISK_PDO Pdo = Sp->DeviceObject->DeviceExtension;
    PSCSI_REQUEST_BLOCK Srb = Sp->Parameters.Scsi.Srb;
    PUCHAR Cdb = Srb->Cdb;
    ULONG64 Lba;
    ULONG Blocks, Bytes;
    BOOLEAN Write;
    PLKDISK_IO Io;
    PVOID Va;
    int R;

    switch (Cdb[0])
    {
        case SCSIOP_READ: case SCSIOP_WRITE:
            Write = Cdb[0] == SCSIOP_WRITE; Lba = BE32(Cdb + 2); Blocks = BE16(Cdb + 7); break;
        case SCSIOP_READ16: case SCSIOP_WRITE16:
            Write = Cdb[0] == SCSIOP_WRITE16; Lba = BE64(Cdb + 2); Blocks = BE32(Cdb + 10); break;
        case 0xA8: case 0xAA:   /* READ(12) / WRITE(12) */
            Write = Cdb[0] == 0xAA; Lba = BE32(Cdb + 2); Blocks = BE32(Cdb + 6); break;
        case SCSIOP_SYNCHRONIZE_CACHE: case 0x91:
        {
            Io = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Io), LKDISK_TAG);
            if (!Io) { SrbDone(Irp, Srb, SRB_STATUS_ERROR, 0); return; }
            KeInitializeDpc(&Io->Dpc, LkDiskIoDpc, Io);
            Io->Irp = Irp; Io->Srb = Srb; Io->Err = 0;
            Srb->DataTransferLength = 0;
            if (BlkFlush(Pdo->Gendisk, LkDiskIoDone, Io)) { ExFreePoolWithTag(Io, LKDISK_TAG); SrbDone(Irp, Srb, SRB_STATUS_ERROR, 0); }
            return;
        }
        default:
            SrbDone(Irp, Srb, SRB_STATUS_INVALID_REQUEST, 0);
            return;
    }

    Bytes = Blocks * Pdo->Lbs;
    if (Lba >= Pdo->Sectors || Blocks > Pdo->Sectors - Lba || Bytes > Srb->DataTransferLength || !Bytes)
    {
        SrbDone(Irp, Srb, SRB_STATUS_ERROR, 0);
        return;
    }
    Io = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Io), LKDISK_TAG);
    if (!Io) { SrbDone(Irp, Srb, SRB_STATUS_ERROR, 0); return; }
    KeInitializeDpc(&Io->Dpc, LkDiskIoDpc, Io);
    Io->Irp = Irp; Io->Srb = Srb; Io->Err = 0;
    Srb->DataTransferLength = Bytes;
    Va = MappedBuffer(Irp, Srb);
    if (!Va) { ExFreePoolWithTag(Io, LKDISK_TAG); SrbDone(Irp, Srb, SRB_STATUS_ERROR, 0); return; }
    R = BlkSubmit(Pdo->Gendisk, Write, Lba * Pdo->Lbs, Va, Bytes, LkDiskIoDone, Io);
    if (R)
    {
        ExFreePoolWithTag(Io, LKDISK_TAG);
        SrbDone(Irp, Srb, SRB_STATUS_ERROR, 0);
    }
}

/* ---- FDO / PDO creation (system thread, PASSIVE_LEVEL) --------------------------------------------------- */
static VOID FillInquiry(PLKDISK_PDO Pdo, PVOID Gendisk, ULONG Index)
{
    CHAR Prod[17];

    RtlZeroMemory(&Pdo->Inquiry, sizeof(Pdo->Inquiry));
    Pdo->Inquiry.DeviceType = DIRECT_ACCESS_DEVICE;
    Pdo->Inquiry.RemovableMedia = 0;
    Pdo->Inquiry.Versions = 5;
    Pdo->Inquiry.ResponseDataFormat = 2;
    Pdo->Inquiry.AdditionalLength = sizeof(INQUIRYDATA) - 5;
    Pdo->Inquiry.CommandQueue = 1;
    RtlCopyMemory(Pdo->Inquiry.VendorId, "Linux   ", 8);
    RtlStringCbPrintfA(Prod, sizeof(Prod), "NVMe disk %-6lu", Index);
    RtlCopyMemory(Pdo->Inquiry.ProductId, Prod, 16);
    RtlCopyMemory(Pdo->Inquiry.ProductRevisionLevel, "1.0 ", 4);
}

static PLKDISK_PDO CreateDiskPdo(PLKDISK_FDO F, PVOID Gendisk)
{
    PDEVICE_OBJECT Dev;
    PLKDISK_PDO Pdo;
    ULONG Index = F->Count;
    NTSTATUS Status;

    if (Index >= LKDISK_MAX) return NULL;
    Status = IoCreateDevice(DiskDriver, sizeof(LKDISK_PDO), NULL, FILE_DEVICE_DISK,
                            FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN, FALSE, &Dev);
    if (!NT_SUCCESS(Status)) return NULL;
    Pdo = Dev->DeviceExtension;
    RtlZeroMemory(Pdo, sizeof(*Pdo));
    Pdo->Common.IsFdo = FALSE;
    Pdo->Common.Self = Dev;
    Pdo->Fdo = F;
    Pdo->Gendisk = Gendisk;
    Pdo->Sectors = BlkCapacity(Gendisk);
    Pdo->Lbs = BlkLbs(Gendisk);
    if (Pdo->Lbs < 512) Pdo->Lbs = 512;
    Pdo->Sectors = Pdo->Sectors * 512 / Pdo->Lbs;       /* Linux capacity is in 512-byte units */
    Pdo->Index = Index;
    Pdo->MaxBytes = 128 * 1024;
    FillInquiry(Pdo, Gendisk, Index);
    Dev->Flags |= DO_DIRECT_IO;
    Dev->StackSize = F->Common.Self->StackSize + 1;
    Dev->Flags &= ~DO_DEVICE_INITIALIZING;
    F->Disks[F->Count++] = Pdo;
    return Pdo;
}

static NTSTATUS CreateFdo(VOID)
{
    PDEVICE_OBJECT FdoObj, LowerPdo = NULL;
    NTSTATUS Status;

    Status = IoCreateDevice(DiskDriver, sizeof(LKDISK_FDO), NULL, FILE_DEVICE_CONTROLLER, FILE_DEVICE_SECURE_OPEN, FALSE, &FdoObj);
    if (!NT_SUCCESS(Status)) return Status;
    Fdo = FdoObj->DeviceExtension;
    RtlZeroMemory(Fdo, sizeof(*Fdo));
    Fdo->Common.IsFdo = TRUE;
    Fdo->Common.Self = FdoObj;
    Status = IoReportDetectedDevice(DiskDriver, Internal, 0, 0, NULL, NULL, TRUE, &LowerPdo);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("LKPI: IoReportDetectedDevice failed %08lx\n", Status);
        IoDeleteDevice(FdoObj);
        Fdo = NULL;
        return Status;
    }
    Fdo->Lower = IoAttachDeviceToDeviceStack(FdoObj, LowerPdo);
    FdoObj->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static VOID NTAPI DiskThread(PVOID Ctx)
{
    for (;;)
    {
        KIRQL Old;
        PVOID Disk = NULL;
        PIRP Irp = NULL;

        KeAcquireSpinLock(&WorkLock, &Old);
        if (NewDiskCount)
        {
            Disk = NewDisks[0];
            RtlMoveMemory(&NewDisks[0], &NewDisks[1], (NewDiskCount - 1) * sizeof(PVOID));
            NewDiskCount--;
        }
        else if (!IsListEmpty(&IoQueue))
        {
            PLIST_ENTRY E = RemoveHeadList(&IoQueue);
            Irp = CONTAINING_RECORD(E, IRP, Tail.Overlay.ListEntry);
        }
        KeReleaseSpinLock(&WorkLock, Old);

        if (Disk)
        {
            if (!Fdo && !NT_SUCCESS(CreateFdo())) { InterlockedDecrement(&DiskPending); continue; }
            if (CreateDiskPdo(Fdo, Disk))
            {
                DbgPrint("LKPI: disk %lu registered with PnP\n", Fdo->Count - 1);
                IoInvalidateDeviceRelations(Fdo->Lower, BusRelations);
            }
            InterlockedDecrement(&DiskPending);
        }
        else if (Irp)
        {
            StartIo(Irp);
        }
        else
        {
            KeWaitForSingleObject(&WorkEvent, Executive, KernelMode, FALSE, NULL);
        }
    }
}

/* Called by the Linux block layer (device_add_disk) from an arbitrary Linux context. */
static void LX_ABI LkDiskHook(void *Gendisk, int Added)
{
    KIRQL Old;

    if (!Added || !DiskStarted) return;
    KeAcquireSpinLock(&WorkLock, &Old);
    if (NewDiskCount < LKDISK_MAX) { NewDisks[NewDiskCount++] = Gendisk; InterlockedIncrement(&DiskPending); }
    KeReleaseSpinLock(&WorkLock, Old);
    KeSetEvent(&WorkEvent, IO_NO_INCREMENT, FALSE);
}

VOID LkpiDiskInit(PDRIVER_OBJECT DriverObject)
{
    HANDLE Th;

    DiskDriver = DriverObject;
    KeInitializeSpinLock(&WorkLock);
    KeInitializeEvent(&WorkEvent, SynchronizationEvent, FALSE);
    InitializeListHead(&IoQueue);
    if (NT_SUCCESS(PsCreateSystemThread(&Th, THREAD_ALL_ACCESS, NULL, NULL, NULL, DiskThread, NULL)))
    {
        ZwClose(Th);
        DiskStarted = TRUE;
    }
}

/* DriverEntry calls this after the modules are initialised: when started as a boot driver the PnP manager enumerates
 * right after, and the disks must be on the bus by then. */
VOID LkpiDiskWaitIdle(VOID)
{
    LARGE_INTEGER Delay;
    ULONG i;

    Delay.QuadPart = -10 * 1000 * 50;       /* 50 ms */
    for (i = 0; i < 200 && DiskPending; i++)
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
}

/* Called by DriverEntry after each module init: once the block layer module is there, install the hook. */
VOID LkpiDiskTryHook(VOID)
{
    PFN_SETHOOK SetHook;

    if (BlkSubmit || !DiskStarted) return;
    SetHook = (PFN_SETHOOK)LkpiLookupModuleSymbol("kpi_blk_set_hook");
    BlkSubmit = (PFN_SUBMIT)LkpiLookupModuleSymbol("kpi_blk_submit");
    BlkFlush = (PFN_FLUSH)LkpiLookupModuleSymbol("kpi_blk_flush");
    BlkCapacity = (PFN_CAP)LkpiLookupModuleSymbol("kpi_blk_capacity");
    BlkLbs = (PFN_LBS)LkpiLookupModuleSymbol("kpi_blk_lbs");
    if (!SetHook || !BlkSubmit || !BlkFlush || !BlkCapacity || !BlkLbs) { BlkSubmit = NULL; return; }
    SetHook(LkDiskHook);
}

/* ---- PnP ----------------------------------------------------------------------------------------------- */
static NTSTATUS FdoPnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    PLKDISK_FDO F = Dev->DeviceExtension;

    LKDISK_TRACE("FDO pnp minor %u\n", Sp->MinorFunction);
    switch (Sp->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (Sp->Parameters.QueryDeviceRelations.Type == BusRelations)
            {
                ULONG i, N = F->Count;
                PDEVICE_RELATIONS R = ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS) + sizeof(PDEVICE_OBJECT) * (N ? N : 1), LKDISK_TAG);

                if (!R)
                {
                    Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                R->Count = N;
                for (i = 0; i < N; i++)
                {
                    R->Objects[i] = F->Disks[i]->Common.Self;
                    ObReferenceObject(R->Objects[i]);
                }
                Irp->IoStatus.Information = (ULONG_PTR)R;
                Irp->IoStatus.Status = STATUS_SUCCESS;
            }
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(F->Lower, Irp);
        default:
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(F->Lower, Irp);
    }
}

static PWSTR MultiSz(const CHAR *const *Strings, ULONG Count)
{
    ULONG i, Total = 1;
    PWSTR Buf, P;

    for (i = 0; i < Count; i++) Total += (ULONG)strlen(Strings[i]) + 1;
    Buf = ExAllocatePoolWithTag(PagedPool, Total * sizeof(WCHAR), LKDISK_TAG);
    if (!Buf) return NULL;
    for (P = Buf, i = 0; i < Count; i++)
    {
        const CHAR *S = Strings[i];
        while (*S) *P++ = (WCHAR)(UCHAR)*S++;
        *P++ = 0;
    }
    *P = 0;
    return Buf;
}

static PWSTR SingleSz(const CHAR *S)
{
    PWSTR Buf = ExAllocatePoolWithTag(PagedPool, (strlen(S) + 1) * sizeof(WCHAR), LKDISK_TAG), P = Buf;

    if (!Buf) return NULL;
    while (*S) *P++ = (WCHAR)(UCHAR)*S++;
    *P = 0;
    return Buf;
}

static NTSTATUS PdoPnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    PLKDISK_PDO Pdo = Dev->DeviceExtension;
    NTSTATUS Status = Irp->IoStatus.Status;
    CHAR Prod[17], Id1[80], Id2[80], Id3[80];

    LKDISK_TRACE("PDO pnp minor %u (idtype %u)\n", Sp->MinorFunction, Sp->MinorFunction == IRP_MN_QUERY_ID ? (ULONG)Sp->Parameters.QueryId.IdType : 0);
    RtlCopyMemory(Prod, Pdo->Inquiry.ProductId, 16);
    Prod[16] = 0;
    {   /* "NVMe disk 0     " -> "NVMe_disk_0" */
        int i, n = 16;
        while (n > 0 && Prod[n - 1] == ' ') n--;
        Prod[n] = 0;
        for (i = 0; i < n; i++) if (Prod[i] == ' ') Prod[i] = '_';
    }

    switch (Sp->MinorFunction)
    {
        case IRP_MN_START_DEVICE: case IRP_MN_REMOVE_DEVICE: case IRP_MN_QUERY_CAPABILITIES:
        case IRP_MN_QUERY_REMOVE_DEVICE: case IRP_MN_QUERY_STOP_DEVICE: case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_STOP_DEVICE: case IRP_MN_CANCEL_REMOVE_DEVICE: case IRP_MN_CANCEL_STOP_DEVICE:
            Status = STATUS_SUCCESS;
            break;
        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (Sp->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation)
            {
                PDEVICE_RELATIONS R = ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS), LKDISK_TAG);
                if (!R) { Status = STATUS_INSUFFICIENT_RESOURCES; break; }
                R->Count = 1;
                R->Objects[0] = Dev;
                ObReferenceObject(Dev);
                Irp->IoStatus.Information = (ULONG_PTR)R;
                Status = STATUS_SUCCESS;
            }
            break;
        case IRP_MN_QUERY_DEVICE_TEXT:
            if (Sp->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription)
            {
                CHAR T[64];
                PWSTR W;
                RtlStringCbPrintfA(T, sizeof(T), "Linux %s SCSI Disk Device", Prod);
                W = SingleSz(T);
                Irp->IoStatus.Information = (ULONG_PTR)W;
                Status = W ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
            }
            else if (Sp->Parameters.QueryDeviceText.DeviceTextType == DeviceTextLocationInformation)
            {
                CHAR T[64];
                PWSTR W;
                RtlStringCbPrintfA(T, sizeof(T), "Bus Number 0, Target ID %lu, LUN 0", Pdo->Index);
                W = SingleSz(T);
                Irp->IoStatus.Information = (ULONG_PTR)W;
                Status = W ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
            }
            break;
        case IRP_MN_QUERY_ID:
        {
            PWSTR W = NULL;
            RtlStringCbPrintfA(Id1, sizeof(Id1), "SCSI\\Disk&Ven_Linux&Prod_%s&Rev_1.0", Prod);
            switch (Sp->Parameters.QueryId.IdType)
            {
                case BusQueryDeviceID:
                    W = SingleSz(Id1);
                    break;
                case BusQueryInstanceID:
                {
                    CHAR T[16];
                    RtlStringCbPrintfA(T, sizeof(T), "%lx00", Pdo->Index);
                    W = SingleSz(T);
                    break;
                }
                case BusQueryHardwareIDs:
                {
                    const CHAR *L[4];
                    RtlStringCbPrintfA(Id2, sizeof(Id2), "SCSI\\DiskLinux___%s", Prod);
                    RtlStringCbPrintfA(Id3, sizeof(Id3), "SCSI\\DiskLinux___");
                    L[0] = Id2; L[1] = Id3; L[2] = "SCSI\\Disk"; L[3] = "GenDisk";
                    W = MultiSz(L, 4);
                    break;
                }
                case BusQueryCompatibleIDs:
                {
                    const CHAR *L[2] = { "SCSI\\Disk", "SCSI\\RAW" };
                    W = MultiSz(L, 2);
                    break;
                }
                default:
                    break;
            }
            if (W) { Irp->IoStatus.Information = (ULONG_PTR)W; Status = STATUS_SUCCESS; }
            break;
        }
        default:
            break;
    }
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS NTAPI LkDiskPnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    return ((LKDISK_COMMON *)Dev->DeviceExtension)->IsFdo ? FdoPnp(Dev, Irp) : PdoPnp(Dev, Irp);
}

NTSTATUS NTAPI LkDiskPower(PDEVICE_OBJECT Dev, PIRP Irp)
{
    LKDISK_COMMON *C = Dev->DeviceExtension;

    PoStartNextPowerIrp(Irp);
    if (C->IsFdo)
    {
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(((PLKDISK_FDO)C)->Lower, Irp);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* ---- SCSI requests -------------------------------------------------------------------------------------- */
static VOID SrbCopyOut(PIRP Irp, PSCSI_REQUEST_BLOCK Srb, const VOID *Data, ULONG Len, ULONG Alloc)
{
    ULONG N = min(Len, Srb->DataTransferLength);
    PVOID Va = MappedBuffer(Irp, Srb);

    if (!Va) { Srb->DataTransferLength = 0; return; }
    RtlZeroMemory(Va, Srb->DataTransferLength);
    RtlCopyMemory(Va, Data, min(N, Alloc));
    Srb->DataTransferLength = N;
}

NTSTATUS NTAPI LkDiskScsi(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    LKDISK_COMMON *C = Dev->DeviceExtension;
    PLKDISK_PDO Pdo = Dev->DeviceExtension;
    PSCSI_REQUEST_BLOCK Srb = Sp->Parameters.Scsi.Srb;
    PUCHAR Cdb;

    LKDISK_TRACE("SCSI srb func %02x cdb %02x len %lu\n", Srb->Function, Srb->Cdb[0], Srb->DataTransferLength);
    if (C->IsFdo)
    {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    switch (Srb->Function)
    {
        case SRB_FUNCTION_CLAIM_DEVICE:
            Srb->DataBuffer = Dev;
            SrbDone(Irp, Srb, SRB_STATUS_SUCCESS, 0);
            return STATUS_SUCCESS;
        case SRB_FUNCTION_RELEASE_DEVICE: case SRB_FUNCTION_LOCK_QUEUE: case SRB_FUNCTION_UNLOCK_QUEUE:
        case SRB_FUNCTION_RELEASE_QUEUE: case SRB_FUNCTION_FLUSH_QUEUE: case SRB_FUNCTION_SHUTDOWN: case SRB_FUNCTION_FLUSH:
            SrbDone(Irp, Srb, SRB_STATUS_SUCCESS, 0);
            return STATUS_SUCCESS;
        case SRB_FUNCTION_EXECUTE_SCSI:
            break;
        default:
            SrbDone(Irp, Srb, SRB_STATUS_INVALID_REQUEST, 0);
            return STATUS_SUCCESS;
    }

    Cdb = Srb->Cdb;
    switch (Cdb[0])
    {
        case SCSIOP_TEST_UNIT_READY: case SCSIOP_START_STOP_UNIT: case SCSIOP_MEDIUM_REMOVAL: case SCSIOP_VERIFY:
            SrbDone(Irp, Srb, SRB_STATUS_SUCCESS, 0);
            return STATUS_SUCCESS;
        case SCSIOP_INQUIRY:
            if (Cdb[1] & 1)     /* EVPD: only the supported-pages page */
            {
                UCHAR Page[6] = { 0, Cdb[2], 0, 2, 0x00, 0x80 };
                if (Cdb[2] != 0) { SrbDone(Irp, Srb, SRB_STATUS_INVALID_REQUEST, 0); return STATUS_SUCCESS; }
                SrbCopyOut(Irp, Srb, Page, sizeof(Page), sizeof(Page));
            }
            else
            {
                SrbCopyOut(Irp, Srb, &Pdo->Inquiry, sizeof(Pdo->Inquiry), sizeof(Pdo->Inquiry));
            }
            SrbDone(Irp, Srb, SRB_STATUS_SUCCESS, Srb->DataTransferLength);
            return STATUS_SUCCESS;
        case SCSIOP_READ_CAPACITY:
        {
            UCHAR D[8];
            ULONG64 Last = Pdo->Sectors - 1;
            PutBE32(D, Last > 0xFFFFFFFFULL ? 0xFFFFFFFF : (ULONG)Last);
            PutBE32(D + 4, Pdo->Lbs);
            SrbCopyOut(Irp, Srb, D, sizeof(D), sizeof(D));
            SrbDone(Irp, Srb, SRB_STATUS_SUCCESS, Srb->DataTransferLength);
            return STATUS_SUCCESS;
        }
        case SCSIOP_SERVICE_ACTION_IN16:
            if ((Cdb[1] & 0x1F) == SERVICE_ACTION_READ_CAPACITY16)
            {
                UCHAR D[32] = { 0 };
                PutBE64(D, Pdo->Sectors - 1);
                PutBE32(D + 8, Pdo->Lbs);
                SrbCopyOut(Irp, Srb, D, sizeof(D), sizeof(D));
                SrbDone(Irp, Srb, SRB_STATUS_SUCCESS, Srb->DataTransferLength);
                return STATUS_SUCCESS;
            }
            SrbDone(Irp, Srb, SRB_STATUS_INVALID_REQUEST, 0);
            return STATUS_SUCCESS;
        case SCSIOP_MODE_SENSE:
        {
            UCHAR D[4] = { 3, 0, 0, 0 };
            SrbCopyOut(Irp, Srb, D, sizeof(D), sizeof(D));
            SrbDone(Irp, Srb, SRB_STATUS_SUCCESS, Srb->DataTransferLength);
            return STATUS_SUCCESS;
        }
        case SCSIOP_MODE_SENSE10:
        {
            UCHAR D[8] = { 0, 6, 0, 0, 0, 0, 0, 0 };
            SrbCopyOut(Irp, Srb, D, sizeof(D), sizeof(D));
            SrbDone(Irp, Srb, SRB_STATUS_SUCCESS, Srb->DataTransferLength);
            return STATUS_SUCCESS;
        }
        case SCSIOP_READ: case SCSIOP_WRITE: case SCSIOP_READ16: case SCSIOP_WRITE16: case 0xA8: case 0xAA:
        case SCSIOP_SYNCHRONIZE_CACHE: case 0x91:
        {
            KIRQL Old;
            IoMarkIrpPending(Irp);
            KeAcquireSpinLock(&WorkLock, &Old);
            InsertTailList(&IoQueue, &Irp->Tail.Overlay.ListEntry);
            KeReleaseSpinLock(&WorkLock, Old);
            KeSetEvent(&WorkEvent, IO_NO_INCREMENT, FALSE);
            return STATUS_PENDING;
        }
        default:
            DbgPrint("LKPI: unsupported SCSI opcode %02x\n", Cdb[0]);
            SrbDone(Irp, Srb, SRB_STATUS_INVALID_REQUEST, 0);
            return STATUS_SUCCESS;
    }
}

/* ---- IOCTLs the class driver sends to the port ---------------------------------------------------------- */
static NTSTATUS Complete(PIRP Irp, NTSTATUS Status, ULONG_PTR Info)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS LkDiskControl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    LKDISK_COMMON *C = Dev->DeviceExtension;
    ULONG Out = Sp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID Buf = Irp->AssociatedIrp.SystemBuffer;

    LKDISK_TRACE("IOCTL %08lx fdo=%u\n", Sp->Parameters.DeviceIoControl.IoControlCode, C->IsFdo);
    switch (Sp->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_STORAGE_QUERY_PROPERTY:
        {
            PSTORAGE_PROPERTY_QUERY Q = Buf;
            if (Sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(*Q)) return Complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
            if (Q->PropertyId == StorageAdapterProperty && Q->QueryType == PropertyStandardQuery)
            {
                PSTORAGE_ADAPTER_DESCRIPTOR A = Buf;
                PLKDISK_PDO Pdo = C->IsFdo ? NULL : (PLKDISK_PDO)C;
                if (Out < sizeof(STORAGE_DESCRIPTOR_HEADER)) return Complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
                if (Out < sizeof(*A))
                {
                    ((PSTORAGE_DESCRIPTOR_HEADER)Buf)->Version = sizeof(*A);
                    ((PSTORAGE_DESCRIPTOR_HEADER)Buf)->Size = sizeof(*A);
                    return Complete(Irp, STATUS_SUCCESS, sizeof(STORAGE_DESCRIPTOR_HEADER));
                }
                RtlZeroMemory(A, sizeof(*A));
                A->Version = sizeof(*A);
                A->Size = sizeof(*A);
                A->MaximumTransferLength = Pdo ? Pdo->MaxBytes : 128 * 1024;
                A->MaximumPhysicalPages = A->MaximumTransferLength / PAGE_SIZE + 1;
                A->AlignmentMask = 3;
                A->CommandQueueing = TRUE;
                A->BusType = BusTypeScsi;
                return Complete(Irp, STATUS_SUCCESS, sizeof(*A));
            }
            if (Q->PropertyId == StorageDeviceProperty && !C->IsFdo)
            {
                PLKDISK_PDO Pdo = (PLKDISK_PDO)C;
                ULONG Total = sizeof(STORAGE_DEVICE_DESCRIPTOR) + 8 + 16 + 4 + 3;
                if (Q->QueryType == PropertyExistsQuery) return Complete(Irp, STATUS_SUCCESS, 0);
                if (Out < sizeof(STORAGE_DESCRIPTOR_HEADER)) return Complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
                if (Out < Total)
                {
                    ((PSTORAGE_DESCRIPTOR_HEADER)Buf)->Version = Total;
                    ((PSTORAGE_DESCRIPTOR_HEADER)Buf)->Size = Total;
                    return Complete(Irp, STATUS_SUCCESS, sizeof(STORAGE_DESCRIPTOR_HEADER));
                }
                {
                    PSTORAGE_DEVICE_DESCRIPTOR D = Buf;
                    PUCHAR P;
                    RtlZeroMemory(D, Total);
                    D->Version = sizeof(*D);
                    D->Size = Total;
                    D->DeviceType = DIRECT_ACCESS_DEVICE;
                    D->CommandQueueing = TRUE;
                    D->BusType = BusTypeScsi;
                    D->VendorIdOffset = FIELD_OFFSET(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties);
                    D->ProductIdOffset = D->VendorIdOffset + 8 + 1;
                    D->ProductRevisionOffset = D->ProductIdOffset + 16 + 1;
                    D->RawPropertiesLength = 8 + 16 + 4 + 3;
                    P = D->RawDeviceProperties;
                    RtlCopyMemory(P, Pdo->Inquiry.VendorId, 8); P += 9;
                    RtlCopyMemory(P, Pdo->Inquiry.ProductId, 16); P += 17;
                    RtlCopyMemory(P, Pdo->Inquiry.ProductRevisionLevel, 4);
                }
                return Complete(Irp, STATUS_SUCCESS, Total);
            }
            return Complete(Irp, STATUS_INVALID_PARAMETER_1, 0);
        }
        case IOCTL_SCSI_GET_ADDRESS:
        {
            PSCSI_ADDRESS A = Buf;
            if (C->IsFdo) return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
            if (Out < sizeof(*A)) return Complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
            A->Length = sizeof(*A);
            A->PortNumber = LKDISK_PORT_NUMBER;
            A->PathId = 0;
            A->TargetId = (UCHAR)((PLKDISK_PDO)C)->Index;
            A->Lun = 0;
            return Complete(Irp, STATUS_SUCCESS, sizeof(*A));
        }
        case IOCTL_SCSI_GET_CAPABILITIES:
        {
            PIO_SCSI_CAPABILITIES Cap = Buf;
            if (Out < sizeof(*Cap)) return Complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
            RtlZeroMemory(Cap, sizeof(*Cap));
            Cap->Length = sizeof(*Cap);
            Cap->MaximumTransferLength = 128 * 1024;
            Cap->MaximumPhysicalPages = 128 * 1024 / PAGE_SIZE + 1;
            Cap->AlignmentMask = 3;
            Cap->TaggedQueuing = TRUE;
            return Complete(Irp, STATUS_SUCCESS, sizeof(*Cap));
        }
        default:
            return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

BOOLEAN LkDiskOwns(PDEVICE_OBJECT Dev)
{
    return Dev->DeviceType == FILE_DEVICE_DISK || Dev->DeviceType == FILE_DEVICE_CONTROLLER;
}
