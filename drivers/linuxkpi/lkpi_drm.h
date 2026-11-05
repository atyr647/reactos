/* Shared between the LinuxKPI host driver and user mode: the DRM device node ioctl envelope. */
#pragma once
#ifndef CTL_CODE
#include <winioctl.h>
#endif

/* \\.\LKPIDRM0 = primary node (minor 0), \\.\LKPIDRM128 = render node (minor 128) */
#define IOCTL_LKPI_DRM CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Input and output buffer layout (same buffer): Cmd = the Linux ioctl number (DRM_IOCTL_*), Size = bytes of Data
 * (must equal _IOC_SIZE(Cmd)); on return Result is 0 or a negative Linux errno and Data holds the output. */
typedef struct _LKPI_DRM_IOCTL
{
    unsigned long Cmd;
    unsigned long Size;
    long Result;
    unsigned long Pad;
    unsigned char Data[8];
} LKPI_DRM_IOCTL;
