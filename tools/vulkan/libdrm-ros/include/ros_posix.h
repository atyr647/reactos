/*
 * libdrm-ros: the POSIX bits libdrm and Mesa's amdgpu code use that mingw-w64 lacks. Force-included when building
 * them (-include ros_posix.h). fds are C runtime fds (DRM nodes are character devices, so open() works on
 * \\.\LKPIDRM<n>).
 */
#pragma once
#ifndef __ASSEMBLER__     /* force-included into Mesa's .S files too */
#include <fcntl.h>
#include <stdio.h>
#include <io.h>
#include <sys/types.h>
#include <sys/sysmacros.h>

#ifndef _ROS_POSIX_IDS
#define _ROS_POSIX_IDS
typedef int uid_t;                  /* xf86drm.h's drmServerInfo (legacy, unused) */
typedef int gid_t;
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC       0           /* handles are not inherited unless asked for */
#endif
#ifndef F_DUPFD
#define F_DUPFD         0
#endif
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC 1030
#endif
#ifndef F_GETFD
#define F_GETFD         1
#define F_SETFD         2
#define FD_CLOEXEC      1
#endif
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE    30
#define _SC_PAGE_SIZE   _SC_PAGESIZE
#endif

#ifdef __cplusplus
extern "C" {
#endif
int fcntl(int fd, int cmd, ...);
ssize_t getline(char **lineptr, size_t *n, FILE *stream);
ssize_t readlink(const char *path, char *buf, size_t len);   /* no /proc here: always -1, ENOENT */
/* libdrm_amdgpu's GPU name table (AMDGPU_ASIC_ID_TABLE): amdgpu.ids next to the module libdrm is linked into */
const char *ros_amdgpu_ids_path(void);    /* F_DUPFD, F_DUPFD_CLOEXEC (dup), F_GETFD/F_SETFD (no-op) */
long sysconf(int name);             /* _SC_PAGESIZE only */
int getpagesize(void);
#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLER__ */
