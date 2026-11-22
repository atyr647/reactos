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

/*
 * Map a DRM buffer into the calling process: Offset is the fake mmap offset a driver ioctl returned
 * (DRM_IOCTL_MODE_MAP_DUMB, DRM_IOCTL_AMDGPU_GEM_MMAP, ...), Size a multiple of 4096. On return Address is the user
 * address. The mapping lasts until IOCTL_LKPI_DRM_MUNMAP (Address), until the handle is closed or until the process
 * exits. The buffer may still move (VRAM <-> system memory): the mapping follows it and keeps its address. The
 * caching is the driver's choice (write-combined for VRAM and most scanout buffers).
 */
#define IOCTL_LKPI_DRM_MMAP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_LKPI_DRM_MUNMAP CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _LKPI_DRM_MMAP
{
    unsigned long long Offset;
    unsigned long long Size;
    unsigned long long Address;
} LKPI_DRM_MMAP;
