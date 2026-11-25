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

/*
 * Which device a DRM handle is open on (for libdrm's device enumeration, which reads sysfs on Linux). Minors follow
 * Linux: primary nodes 0-63 (\\.\LKPIDRM<n>), render nodes 128-191; -1 = the device has no node of that kind.
 * Layout shared with kpi/kpi2_drm.c (struct kpi_drm_info): 64 bytes, no implicit padding.
 */
#define IOCTL_LKPI_DRM_INFO   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _LKPI_DRM_INFO
{
    int Minor;                  /* the node this handle is open on */
    int PrimaryMinor;
    int RenderMinor;
    int IsPci;                  /* the fields below are valid */
    unsigned short Domain, Vendor, Device, SubVendor, SubDevice, Reserved;
    unsigned char Bus, Dev, Func, Revision;
    char Driver[32];            /* Linux driver name, e.g. "amdgpu", "bochs-drm" */
} LKPI_DRM_INFO;

/*
 * The desktop on a Linux KMS driver: \Device\LkpiDisplay, kernel-mode callers only (IRP_MJ_INTERNAL_DEVICE_CONTROL),
 * used by the LinuxKPI video miniport (win32ss/drivers/miniport/lkpi). START waits for the GPU modules, sets the
 * preferred mode of the first card with a connected monitor on one XRGB8888 buffer and returns it. Layout shared with
 * kpi/kpi2_drm.c (struct kpi_display_info): 64 bytes.
 */
#define IOCTL_LKPI_DISPLAY_START CTL_CODE(FILE_DEVICE_VIDEO, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _LKPI_DISPLAY_INFO
{
    unsigned long long Address;     /* kernel (system space) address of the scanout buffer */
    unsigned int Width, Height;
    unsigned int Pitch;             /* bytes per line */
    unsigned int Bpp;               /* 32: XRGB8888 */
    unsigned int Refresh;           /* Hz */
    unsigned int Minor;             /* the DRM card */
    char Connector[32];             /* e.g. "DP-1" */
} LKPI_DISPLAY_INFO;

/*
 * poll() of a Linux file reached through a handle: a shared one (dma-buf, sync file, syncobj fd; their handles are
 * the fds Linux reports, on \\Device\\LkpiFile) or a DRM node. Events/Revents: Linux EPOLL* bits. TimeoutMs < 0:
 * wait without limit.
 */
#define IOCTL_LKPI_FILE_POLL  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _LKPI_FILE_POLL
{
    unsigned int Events;
    int TimeoutMs;
    unsigned int Revents;
    unsigned int Reserved;
} LKPI_FILE_POLL;
