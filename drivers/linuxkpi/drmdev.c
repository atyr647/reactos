/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     \Device\LKPIDRMn: user-mode access to the DRM devices registered by Linux drivers.
 *              Create -> drm_open(), IOCTL_LKPI_DRM -> the driver's unlocked_ioctl, Close -> drm_release().
 */

#include "linuxkpi.h"
#include "lkpi_drm.h"
#include <ntstrsafe.h>

typedef void * (LX_ABI *PFN_OPEN)(int Minor);
typedef long (LX_ABI *PFN_IOCTL)(void *H, unsigned int Cmd, void *Arg);
typedef void (LX_ABI *PFN_CLOSE)(void *H);

static PFN_OPEN DrmOpen;
static PFN_IOCTL DrmIoctl;
static PFN_CLOSE DrmClose;

typedef struct _DRM_DEVEXT { int Minor; } DRM_DEVEXT, *PDRM_DEVEXT;

static BOOLEAN
DrmResolve(VOID)
{
    if (!DrmOpen) DrmOpen = (PFN_OPEN)LkpiLookupModuleSymbol("kpi_drm_open");
    if (!DrmIoctl) DrmIoctl = (PFN_IOCTL)LkpiLookupModuleSymbol("kpi_drm_ioctl");
    if (!DrmClose) DrmClose = (PFN_CLOSE)LkpiLookupModuleSymbol("kpi_drm_close");
    return DrmOpen && DrmIoctl && DrmClose;
}

static NTSTATUS NTAPI
DrmCreate(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    PDRM_DEVEXT Ext = Dev->DeviceExtension;
    NTSTATUS Status = STATUS_SUCCESS;
    PVOID H = NULL;

    if (!DrmResolve()) Status = STATUS_NO_SUCH_DEVICE;
    else
    {
        H = DrmOpen(Ext->Minor);
        if (!H) Status = STATUS_NO_SUCH_DEVICE;
    }
    Sp->FileObject->FsContext = H;
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS NTAPI
DrmClean(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);

    if (Sp->FileObject->FsContext && DrmClose)
    {
        DrmClose(Sp->FileObject->FsContext);
        Sp->FileObject->FsContext = NULL;
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
DrmControl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Info = 0;

    if (Sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_LKPI_DRM)
    {
        LKPI_DRM_IOCTL *Io = Irp->AssociatedIrp.SystemBuffer;
        ULONG InLen = Sp->Parameters.DeviceIoControl.InputBufferLength;
        ULONG OutLen = Sp->Parameters.DeviceIoControl.OutputBufferLength;
        ULONG Hdr = FIELD_OFFSET(LKPI_DRM_IOCTL, Data);

        if (!Io || InLen < Hdr || Io->Size > InLen - Hdr || OutLen < Hdr + Io->Size || !Sp->FileObject->FsContext)
            Status = STATUS_INVALID_PARAMETER;
        else
        {
            /* the DRM core copies arguments with memcpy (copy_from_user), so give it a 16-byte aligned kernel copy */
            ULONG Size = Io->Size;
            PUCHAR Tmp = ExAllocatePoolWithTag(NonPagedPool, Size + 16, LKPI_TAG);

            if (!Tmp) Status = STATUS_INSUFFICIENT_RESOURCES;
            else
            {
                PUCHAR Arg = (PUCHAR)ALIGN_UP_POINTER_BY(Tmp, 16);
                RtlCopyMemory(Arg, Io->Data, Size);
                Io->Result = (long)DrmIoctl(Sp->FileObject->FsContext, (unsigned int)Io->Cmd, Arg);
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

NTSTATUS
LkpiCreateDrmNodes(PDRIVER_OBJECT DriverObject)
{
    static const int Minors[] = { 0, 128 };
    ULONG i;

    DriverObject->MajorFunction[IRP_MJ_CREATE] = DrmCreate;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = DrmClean;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DrmClean;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DrmControl;

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
        Status = IoCreateDevice(DriverObject, sizeof(DRM_DEVEXT), &Name, FILE_DEVICE_UNKNOWN, 0, FALSE, &Dev);
        if (!NT_SUCCESS(Status)) { DbgPrint("LKPI: IoCreateDevice(%wZ) failed %08lx\n", &Name, Status); continue; }
        ((PDRM_DEVEXT)Dev->DeviceExtension)->Minor = Minors[i];
        Dev->Flags |= DO_BUFFERED_IO;
        Dev->Flags &= ~DO_DEVICE_INITIALIZING;
        IoCreateSymbolicLink(&Link, &Name);
        DbgPrint("LKPI: DRM node %wZ created\n", &Name);
    }
    return STATUS_SUCCESS;
}
