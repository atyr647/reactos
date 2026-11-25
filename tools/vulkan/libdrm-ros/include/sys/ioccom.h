/*
 * libdrm-ros: ioctl numbers for the DRM uapi headers on ReactOS/Windows.
 *
 * Off Linux, libdrm's and Mesa's drm.h take their "BSD" branch and build ioctl numbers with <sys/ioccom.h>. The
 * numbers go to the Linux DRM driver unchanged (LinuxKPI host, \\.\LKPIDRM<n>), which decodes them the Linux way, so
 * this header produces Linux encodings under the BSD names: direction in bits 30-31 (1 = write/in, 2 = read/out),
 * size in bits 16-29, type in bits 8-15, number in bits 0-7.
 */
#pragma once

#define _IOC_NRBITS     8
#define _IOC_TYPEBITS   8
#define _IOC_SIZEBITS   14
#define _IOC_NRSHIFT    0
#define _IOC_TYPESHIFT  (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT  (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT   (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE       0U
#define _IOC_WRITE      1U
#define _IOC_READ       2U

#define IOC_VOID        (_IOC_NONE << _IOC_DIRSHIFT)
#define IOC_IN          (_IOC_WRITE << _IOC_DIRSHIFT)
#define IOC_OUT         (_IOC_READ << _IOC_DIRSHIFT)
#define IOC_INOUT       (IOC_IN | IOC_OUT)

/* dir is one of IOC_VOID/IOC_IN/IOC_OUT/IOC_INOUT (already shifted), as drm.h passes it */
#define _IOC(dir, type, nr, size) \
    ((unsigned long)((dir) | ((unsigned long)(size) << _IOC_SIZESHIFT) | ((type) << _IOC_TYPESHIFT) | \
                     ((nr) << _IOC_NRSHIFT)))
#define _IO(type, nr)           _IOC(IOC_VOID, (type), (nr), 0)
#define _IOR(type, nr, t)       _IOC(IOC_OUT, (type), (nr), sizeof(t))
#define _IOW(type, nr, t)       _IOC(IOC_IN, (type), (nr), sizeof(t))
#define _IOWR(type, nr, t)      _IOC(IOC_INOUT, (type), (nr), sizeof(t))

#define _IOC_DIR(nr)            (((nr) >> _IOC_DIRSHIFT) & 3U)
#define _IOC_TYPE(nr)           (((nr) >> _IOC_TYPESHIFT) & 0xffU)
#define _IOC_NR(nr)             (((nr) >> _IOC_NRSHIFT) & 0xffU)
#define _IOC_SIZE(nr)           (((nr) >> _IOC_SIZESHIFT) & 0x3fffU)
