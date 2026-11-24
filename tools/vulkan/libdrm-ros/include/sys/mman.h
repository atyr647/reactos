/*
 * libdrm-ros: mmap()/munmap() for DRM buffers. mmap() on a DRM device fd maps the buffer behind a fake offset
 * (from DRM_IOCTL_MODE_MAP_DUMB, DRM_IOCTL_AMDGPU_GEM_MMAP, ...) through IOCTL_LKPI_DRM_MMAP; the mapping keeps the
 * device open until munmap(), as on Linux. Anything else (anonymous, private or MAP_FIXED mappings) fails with
 * EINVAL; MAP_ANONYMOUS is defined only so that code which never takes that path here compiles.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_ANON        MAP_ANONYMOUS
#define MAP_FAILED      ((void *)-1)

#ifdef __cplusplus
extern "C" {
#endif
void *mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset);
int munmap(void *addr, size_t length);
#ifdef __cplusplus
}
#endif
