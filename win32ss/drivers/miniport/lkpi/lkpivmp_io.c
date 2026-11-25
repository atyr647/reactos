/*
 * PROJECT:     ReactOS LinuxKPI display miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The two kernel calls the miniport needs: open the host's \Device\LkpiDisplay and ask it for the display.
 *              Separate from lkpivmp.c because <ntddk.h> and the video miniport headers cannot be mixed.
 */

#include <ntddk.h>
#include "../../../../drivers/linuxkpi/lkpi_drm.h"
#include "lkpivmp.h"

NTSTATUS
LkpiVmpOpenDisplay(PVOID *DisplayFile, PVOID *DisplayDevice)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\LkpiDisplay");
    NTSTATUS Status;

    Status = IoGetDeviceObjectPointer(&Name, FILE_READ_DATA, (PFILE_OBJECT *)DisplayFile, (PDEVICE_OBJECT *)DisplayDevice);
    if (!NT_SUCCESS(Status))
        DbgPrint("LkpiVmp: no %wZ (%08lx): Linux GPU drivers not loaded (boot option /LINUXKPI_GPU)\n", &Name, Status);
    return Status;
}

/* PASSIVE_LEVEL; the host may wait (up to 2 minutes) for the GPU modules */
NTSTATUS
LkpiVmpQueryDisplay(PVOID DisplayDevice, LKPI_DISPLAY_INFO *Info)
{
    IO_STATUS_BLOCK Iosb;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IOCTL_LKPI_DISPLAY_START, DisplayDevice, NULL, 0, Info, sizeof(*Info), TRUE,
                                        &Event, &Iosb);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = IoCallDriver(DisplayDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    return Status;
}
