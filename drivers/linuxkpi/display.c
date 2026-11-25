/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     \Device\LkpiDisplay: the desktop on a Linux KMS driver (amdgpu, bochs, ...). The LinuxKPI video
 *              miniport (win32ss/drivers/miniport/lkpi) asks for the display when win32k sets up the desktop; this
 *              waits for the GPU modules (they load late, from a worker thread) and starts the in-kernel DRM client
 *              in kpi/kpi2_drm.c, which sets the mode and returns the scanout buffer.
 */

#include "linuxkpi.h"
#include "lkpi_drm.h"

C_ASSERT(sizeof(LKPI_DISPLAY_INFO) == 64);  /* struct kpi_display_info in kpi/kpi2_drm.c */

typedef int (LX_ABI *PFN_DISPLAY_START)(LKPI_DISPLAY_INFO *Info);

static PDEVICE_OBJECT LkpiDisplayDevice;
static KEVENT LkpiGpuLoaded;
static BOOLEAN LkpiGpuEnabled;

/* DriverEntry: GpuEnabled = the GPU module group will be loaded (boot option /LINUXKPI_GPU) */
VOID
LkpiDisplayInit(PDRIVER_OBJECT DriverObject, BOOLEAN GpuEnabled)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\LkpiDisplay");
    NTSTATUS Status;

    KeInitializeEvent(&LkpiGpuLoaded, NotificationEvent, FALSE);
    LkpiGpuEnabled = GpuEnabled;
    Status = IoCreateDevice(DriverObject, 0, &Name, FILE_DEVICE_VIDEO, FILE_DEVICE_SECURE_OPEN, FALSE,
                            &LkpiDisplayDevice);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("LKPI: cannot create \\Device\\LkpiDisplay (%08lx)\n", Status);
        LkpiDisplayDevice = NULL;
        return;
    }
    LkpiDisplayDevice->Flags |= DO_BUFFERED_IO;
    LkpiDisplayDevice->Flags &= ~DO_DEVICE_INITIALIZING;
}

/* the GPU group thread is done (whether or not a module failed) */
VOID
LkpiDisplayGpuLoaded(VOID)
{
    KeSetEvent(&LkpiGpuLoaded, IO_NO_INCREMENT, FALSE);
}

BOOLEAN
LkpiDisplayOwns(PDEVICE_OBJECT Dev)
{
    return Dev && Dev == LkpiDisplayDevice;
}

static NTSTATUS
LkpiDisplayStart(LKPI_DISPLAY_INFO *Info)
{
    static PFN_DISPLAY_START Start;
    LARGE_INTEGER Timeout;
    int r;

    if (!LkpiGpuEnabled)
        return STATUS_DEVICE_DOES_NOT_EXIST;
    /* win32k sets up the desktop while the GPU modules may still be initialising (amdgpu: firmware, display core) */
    Timeout.QuadPart = -10LL * 1000 * 1000 * 120;
    if (KeWaitForSingleObject(&LkpiGpuLoaded, Executive, KernelMode, FALSE, &Timeout) != STATUS_SUCCESS)
    {
        DbgPrint("LKPI: display: the GPU modules did not finish loading in 120 s\n");
        return STATUS_IO_TIMEOUT;
    }
    if (!Start)
        Start = (PFN_DISPLAY_START)LkpiLookupModuleSymbol("kpi_display_start");
    if (!Start)
        return STATUS_DEVICE_DOES_NOT_EXIST;
    r = Start(Info);
    if (r)
    {
        DbgPrint("LKPI: display: no desktop on a Linux KMS driver (%d)\n", r);
        return r == -2 /* ENOENT: no monitor */ ? STATUS_NO_MEDIA_IN_DEVICE : STATUS_DEVICE_DOES_NOT_EXIST;
    }
    Info->Connector[sizeof(Info->Connector) - 1] = 0;
    DbgPrint("LKPI: display: desktop on card %u (%s), %ux%u@%u Hz, pitch %u, buffer at %p\n", Info->Minor,
             Info->Connector, Info->Width, Info->Height, Info->Refresh, Info->Pitch, (PVOID)(ULONG_PTR)Info->Address);
    return STATUS_SUCCESS;
}

NTSTATUS
LkpiDisplayDispatch(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG_PTR Info = 0;

    UNREFERENCED_PARAMETER(Dev);
    switch (Sp->MajorFunction)
    {
        case IRP_MJ_CREATE:
        case IRP_MJ_CLOSE:
        case IRP_MJ_CLEANUP:
            break;
        case IRP_MJ_INTERNAL_DEVICE_CONTROL:    /* kernel-mode callers only */
            if (Sp->Parameters.DeviceIoControl.IoControlCode != IOCTL_LKPI_DISPLAY_START)
                Status = STATUS_INVALID_DEVICE_REQUEST;
            else if (Sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(LKPI_DISPLAY_INFO))
                Status = STATUS_BUFFER_TOO_SMALL;
            else if (NT_SUCCESS(Status = LkpiDisplayStart(Irp->AssociatedIrp.SystemBuffer)))
                Info = sizeof(LKPI_DISPLAY_INFO);
            break;
        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}
