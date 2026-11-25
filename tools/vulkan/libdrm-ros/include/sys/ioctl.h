/*
 * libdrm-ros: ioctl() for DRM fds only (Mesa's util/os_drm.h calls it directly); the encoding macros come from
 * <sys/ioccom.h>. Same as drmIoctl() without the EINTR/EAGAIN retry: -1 and errno on failure.
 */
#pragma once
#include <sys/ioccom.h>

#ifdef __cplusplus
extern "C" {
#endif
int ioctl(int fd, unsigned long request, ...);
#ifdef __cplusplus
}
#endif
