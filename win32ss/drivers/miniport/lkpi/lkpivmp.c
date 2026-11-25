/*
 * PROJECT:     ReactOS LinuxKPI display miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The desktop on a Linux KMS driver (amdgpu, bochs, ... run by the LinuxKPI host, drivers/linuxkpi):
 *              the Linux driver sets the monitor's preferred mode on one linear XRGB8888 scanout buffer, and the
 *              generic framebuf display driver draws into it.
 *
 * A virtual adapter: the GPU's PCI device belongs to the Linux driver, so this miniport drives a root-enumerated device
 * (Root\LKPI_DISPLAY, lkpivmp_reg.inf; legacy detection needs firmware bus descriptions that UEFI systems lack).
 * HwFindAdapter succeeds only when the host offers \Device\LkpiDisplay (boot option /LINUXKPI_GPU). The display itself is started at the first mode query,
 * when win32k sets up the desktop: the host waits there for the GPU modules, which load late. If no Linux KMS driver
 * has a monitor, the mode queries fail and win32k uses another display device.
 */

#include <ntdef.h>
#include <dderror.h>
#include <devioctl.h>
#include <miniport.h>
#include <ntddvdeo.h>
#include <video.h>

#include "../../../../drivers/linuxkpi/lkpi_drm.h"
#include "lkpivmp.h"

typedef struct _LKPI_VMP_EXTENSION
{
    PVOID DisplayFile;                  /* \Device\LkpiDisplay: FILE_OBJECT, DEVICE_OBJECT */
    PVOID DisplayDevice;
    BOOLEAN Started;                    /* Info is valid */
    LKPI_DISPLAY_INFO Info;
} LKPI_VMP_EXTENSION, *PLKPI_VMP_EXTENSION;

/* Ask the host for the display (once). PASSIVE_LEVEL; may wait for the GPU modules. */
static BOOLEAN
LkpiVmpStart(PLKPI_VMP_EXTENSION Ext)
{
    LONG Status;

    if (Ext->Started)
        return TRUE;
    Status = LkpiVmpQueryDisplay(Ext->DisplayDevice, &Ext->Info);
    if (Status < 0 || Ext->Info.Bpp != 32 || !Ext->Info.Width || !Ext->Info.Height || !Ext->Info.Address)
    {
        VideoDebugPrint((Error, "LkpiVmp: no Linux KMS display (%08lx)\n", Status));
        return FALSE;
    }
    VideoDebugPrint((Info, "LkpiVmp: %lux%lu, pitch %lu\n", Ext->Info.Width, Ext->Info.Height, Ext->Info.Pitch));
    Ext->Started = TRUE;
    return TRUE;
}

static VP_STATUS NTAPI
LkpiVmpFindAdapter(PVOID HwDeviceExtension, PVOID HwContext, PWSTR ArgumentString,
                   PVIDEO_PORT_CONFIG_INFO ConfigInfo, PUCHAR Again)
{
    PLKPI_VMP_EXTENSION Ext = HwDeviceExtension;

    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(ArgumentString);
    *Again = FALSE;
    if (ConfigInfo->Length < sizeof(VIDEO_PORT_CONFIG_INFO))
        return ERROR_INVALID_PARAMETER;
    /* the host creates it only when it loads GPU drivers */
    if (LkpiVmpOpenDisplay(&Ext->DisplayFile, &Ext->DisplayDevice) < 0)
        return ERROR_DEV_NOT_EXIST;
    /* no legacy VGA ranges, no BIOS emulation: nothing else to claim */
    ConfigInfo->VdmPhysicalVideoMemoryAddress.QuadPart = 0;
    ConfigInfo->VdmPhysicalVideoMemoryLength = 0;
    return NO_ERROR;
}

static BOOLEAN NTAPI
LkpiVmpInitialize(PVOID HwDeviceExtension)
{
    UNREFERENCED_PARAMETER(HwDeviceExtension);
    return TRUE;
}

static VOID
LkpiVmpFillMode(PLKPI_VMP_EXTENSION Ext, PVIDEO_MODE_INFORMATION Mode)
{
    VideoPortZeroMemory(Mode, sizeof(*Mode));
    Mode->Length = sizeof(*Mode);
    Mode->ModeIndex = 0;
    Mode->VisScreenWidth = Ext->Info.Width;
    Mode->VisScreenHeight = Ext->Info.Height;
    Mode->ScreenStride = Ext->Info.Pitch;
    Mode->NumberOfPlanes = 1;
    Mode->BitsPerPlane = 32;
    Mode->Frequency = Ext->Info.Refresh ? Ext->Info.Refresh : 60;
    Mode->XMillimeter = 320;
    Mode->YMillimeter = 240;
    Mode->NumberRedBits = 8;
    Mode->NumberGreenBits = 8;
    Mode->NumberBlueBits = 8;
    Mode->RedMask = 0x00FF0000;
    Mode->GreenMask = 0x0000FF00;
    Mode->BlueMask = 0x000000FF;
    Mode->AttributeFlags = VIDEO_MODE_COLOR | VIDEO_MODE_GRAPHICS | VIDEO_MODE_NO_OFF_SCREEN;
    Mode->VideoMemoryBitmapWidth = Ext->Info.Width;
    Mode->VideoMemoryBitmapHeight = Ext->Info.Height;
}

static BOOLEAN NTAPI
LkpiVmpStartIO(PVOID HwDeviceExtension, PVIDEO_REQUEST_PACKET Rp)
{
    PLKPI_VMP_EXTENSION Ext = HwDeviceExtension;
    VP_STATUS Status = NO_ERROR;
    ULONG_PTR Info = 0;

    switch (Rp->IoControlCode)
    {
        case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES:
        {
            PVIDEO_NUM_MODES Num = Rp->OutputBuffer;

            if (Rp->OutputBufferLength < sizeof(VIDEO_NUM_MODES))
                Status = ERROR_INSUFFICIENT_BUFFER;
            else if (!LkpiVmpStart(Ext))
                Status = ERROR_DEV_NOT_EXIST;
            else
            {
                Num->NumModes = 1;          /* the monitor's preferred mode, set by the Linux driver */
                Num->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
                Info = sizeof(VIDEO_NUM_MODES);
            }
            break;
        }
        case IOCTL_VIDEO_QUERY_AVAIL_MODES:
        case IOCTL_VIDEO_QUERY_CURRENT_MODE:
            if (Rp->OutputBufferLength < sizeof(VIDEO_MODE_INFORMATION))
                Status = ERROR_INSUFFICIENT_BUFFER;
            else if (!LkpiVmpStart(Ext))
                Status = ERROR_DEV_NOT_EXIST;
            else
            {
                LkpiVmpFillMode(Ext, Rp->OutputBuffer);
                Info = sizeof(VIDEO_MODE_INFORMATION);
            }
            break;
        case IOCTL_VIDEO_SET_CURRENT_MODE:
            if (Rp->InputBufferLength < sizeof(VIDEO_MODE))
                Status = ERROR_INSUFFICIENT_BUFFER;
            else if (!Ext->Started || (((PVIDEO_MODE)Rp->InputBuffer)->RequestedMode & ~VIDEO_MODE_NO_ZERO_MEMORY) != 0)
                Status = ERROR_INVALID_PARAMETER;
            break;                          /* the mode is set already */
        case IOCTL_VIDEO_RESET_DEVICE:
            break;                          /* the Linux driver keeps the display */
        case IOCTL_VIDEO_MAP_VIDEO_MEMORY:
        {
            PVIDEO_MEMORY_INFORMATION Mem = Rp->OutputBuffer;

            if (Rp->OutputBufferLength < sizeof(VIDEO_MEMORY_INFORMATION) || Rp->InputBufferLength < sizeof(VIDEO_MEMORY))
                Status = ERROR_INSUFFICIENT_BUFFER;
            else if (!Ext->Started)
                Status = ERROR_INVALID_PARAMETER;
            else
            {
                /* the buffer is mapped in system space by the Linux driver for as long as it is the desktop's: the
                 * display driver (kernel mode) can use that address in any process */
                Mem->VideoRamBase = Mem->FrameBufferBase = (PVOID)(ULONG_PTR)Ext->Info.Address;
                Mem->VideoRamLength = Mem->FrameBufferLength = Ext->Info.Pitch * Ext->Info.Height;
                Info = sizeof(VIDEO_MEMORY_INFORMATION);
            }
            break;
        }
        case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY:
            break;                          /* nothing was mapped for the caller */
        default:
            Status = ERROR_INVALID_FUNCTION;
            break;
    }
    Rp->StatusBlock->Status = Status;
    Rp->StatusBlock->Information = Info;
    return TRUE;
}

static BOOLEAN NTAPI
LkpiVmpResetHw(PVOID HwDeviceExtension, ULONG Columns, ULONG Rows)
{
    UNREFERENCED_PARAMETER(HwDeviceExtension);
    UNREFERENCED_PARAMETER(Columns);
    UNREFERENCED_PARAMETER(Rows);
    return FALSE;   /* no text mode to go back to: let the system use its own way (bootvid) */
}

/* PnP miniport callbacks: no power management of our own (the Linux driver owns the GPU), no child devices */
static VP_STATUS NTAPI
LkpiVmpGetPowerState(PVOID HwDeviceExtension, ULONG HwId, PVIDEO_POWER_MANAGEMENT VideoPowerControl)
{
    UNREFERENCED_PARAMETER(HwDeviceExtension);
    UNREFERENCED_PARAMETER(HwId);
    UNREFERENCED_PARAMETER(VideoPowerControl);
    return ERROR_DEVICE_REINITIALIZATION_NEEDED;
}

static VP_STATUS NTAPI
LkpiVmpSetPowerState(PVOID HwDeviceExtension, ULONG HwId, PVIDEO_POWER_MANAGEMENT VideoPowerControl)
{
    UNREFERENCED_PARAMETER(HwDeviceExtension);
    UNREFERENCED_PARAMETER(HwId);
    UNREFERENCED_PARAMETER(VideoPowerControl);
    return NO_ERROR;
}

static VP_STATUS NTAPI
LkpiVmpGetChildDescriptor(PVOID HwDeviceExtension, PVIDEO_CHILD_ENUM_INFO ChildEnumInfo,
                          PVIDEO_CHILD_TYPE VideoChildType, PUCHAR pChildDescriptor, PULONG UId, PULONG pUnused)
{
    UNREFERENCED_PARAMETER(HwDeviceExtension);
    UNREFERENCED_PARAMETER(ChildEnumInfo);
    UNREFERENCED_PARAMETER(VideoChildType);
    UNREFERENCED_PARAMETER(pChildDescriptor);
    UNREFERENCED_PARAMETER(UId);
    UNREFERENCED_PARAMETER(pUnused);
    return ERROR_NO_MORE_DEVICES;
}

ULONG NTAPI
DriverEntry(PVOID Context1, PVOID Context2)
{
    VIDEO_HW_INITIALIZATION_DATA InitData;

    VideoPortZeroMemory(&InitData, sizeof(InitData));
    InitData.HwInitDataSize = sizeof(InitData);
    InitData.AdapterInterfaceType = Internal;
    InitData.HwFindAdapter = LkpiVmpFindAdapter;
    InitData.HwInitialize = LkpiVmpInitialize;
    InitData.HwStartIO = LkpiVmpStartIO;
    InitData.HwResetHw = LkpiVmpResetHw;
    InitData.HwDeviceExtensionSize = sizeof(LKPI_VMP_EXTENSION);
    InitData.HwGetPowerState = LkpiVmpGetPowerState;
    InitData.HwSetPowerState = LkpiVmpSetPowerState;
    InitData.HwGetVideoChildDescriptor = LkpiVmpGetChildDescriptor;
    return VideoPortInitialize(Context1, Context2, &InitData, NULL);
}
