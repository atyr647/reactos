/*
 * libdrm-ros: mmap()/munmap() of DRM buffers and the few other POSIX calls libdrm_amdgpu and Mesa's amdgpu code use
 * (include/ros_posix.h, include/sys/mman.h).
 *
 * mmap(fd, offset) asks the driver for the buffer behind the fake offset (IOCTL_LKPI_DRM_MMAP). The mapping holds a
 * duplicate of the device handle, so like on Linux it survives close(fd) and goes away with munmap() (or when the
 * process exits). The driver keeps the address valid when the buffer moves between VRAM and system memory.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ros_posix.h"
#include "sys/mman.h"
#include "poll.h"
#include "lkpi_drm.h"

typedef struct ros_map {
    struct ros_map *next;
    void *addr;
    size_t length;
    HANDLE dev;         /* duplicate of the fd's handle: keeps the DRM file open while mapped */
} ros_map;

static int drm_unmap(ros_map *m);

static ros_map *maps;
static SRWLOCK maps_lock = SRWLOCK_INIT;

static size_t page_size(void)
{
    static size_t size;

    if (!size)
    {
        SYSTEM_INFO si;

        GetSystemInfo(&si);
        size = si.dwPageSize;
    }
    return size;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd), dev;
    LKPI_DRM_MMAP req;
    ros_map *m;
    DWORD got = 0;

    (void)prot;         /* a DRM mapping is always read/write */
    if ((flags & (MAP_FIXED | MAP_ANONYMOUS)) || !(flags & MAP_SHARED) || !length || offset < 0 ||
        (offset & (page_size() - 1)))
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    (void)addr;         /* a hint only, as on Linux without MAP_FIXED */
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = EBADF;
        return MAP_FAILED;
    }
    m = calloc(1, sizeof(*m));
    if (!m)
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dev, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        free(m);
        errno = EBADF;
        return MAP_FAILED;
    }
    req.Offset = (unsigned long long)offset;
    req.Size = (length + page_size() - 1) & ~(unsigned long long)(page_size() - 1);
    req.Address = 0;
    if (!DeviceIoControl(dev, IOCTL_LKPI_DRM_MMAP, &req, sizeof(req), &req, sizeof(req), &got, NULL) || !req.Address)
    {
        DWORD err = GetLastError();

        CloseHandle(dev);
        free(m);
        errno = err == ERROR_ACCESS_DENIED ? EACCES : err == ERROR_INVALID_PARAMETER ? EINVAL :
                err == ERROR_NO_SYSTEM_RESOURCES || err == ERROR_NOT_ENOUGH_MEMORY ? ENOMEM : ENODEV;
        return MAP_FAILED;
    }
    m->addr = (void *)(uintptr_t)req.Address;
    m->length = (size_t)req.Size;
    m->dev = dev;
    AcquireSRWLockExclusive(&maps_lock);
    m->next = maps;
    maps = m;
    ReleaseSRWLockExclusive(&maps_lock);
    return m->addr;
}

/* Whole mappings only: addr must be what mmap() returned (length is not checked; the whole mapping goes). */
int munmap(void *addr, size_t length)
{
    ros_map **p, *m = NULL;
    BOOL ok;

    (void)length;
    AcquireSRWLockExclusive(&maps_lock);
    for (p = &maps; *p; p = &(*p)->next)
        if ((*p)->addr == addr)
        {
            m = *p;
            *p = m->next;
            break;
        }
    ReleaseSRWLockExclusive(&maps_lock);
    if (!m)
    {
        errno = EINVAL;
        return -1;
    }
    ok = drm_unmap(m);
    free(m);
    if (!ok)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* Remove a DRM mapping in the driver and drop its device handle. TRUE on success. */
static int drm_unmap(ros_map *m)
{
    LKPI_DRM_MMAP req;
    DWORD got = 0;
    BOOL ok;

    req.Offset = 0;
    req.Size = m->length;
    req.Address = (uintptr_t)m->addr;
    ok = DeviceIoControl(m->dev, IOCTL_LKPI_DRM_MUNMAP, &req, sizeof(req), NULL, 0, &got, NULL);
    CloseHandle(m->dev);        /* closing the last handle would also remove the mapping */
    return ok;
}

/* <poll.h>: no sync files exist here (include/poll.h) */
int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    (void)fds;
    (void)nfds;
    (void)timeout;
    errno = ENOSYS;
    return -1;
}

int fcntl(int fd, int cmd, ...)
{
    switch (cmd)
    {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        return _dup(fd);        /* a new fd on the same DRM file (same drm_file in the driver), as on Linux */
    case F_GETFD:
        return _get_osfhandle(fd) == -1 ? (errno = EBADF, -1) : FD_CLOEXEC;
    case F_SETFD:
        return _get_osfhandle(fd) == -1 ? (errno = EBADF, -1) : 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

long sysconf(int name)
{
    if (name == _SC_PAGESIZE)
        return (long)page_size();
    errno = EINVAL;
    return -1;
}

int getpagesize(void)
{
    return (int)page_size();
}

/* POSIX getline(): one line including its newline, *lineptr grown with realloc(); -1 at end of file */
ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
    size_t len = 0;
    int c;

    if (!lineptr || !n || !stream)
    {
        errno = EINVAL;
        return -1;
    }
    if (!*lineptr || !*n)
    {
        *n = 128;
        *lineptr = realloc(*lineptr, *n);
        if (!*lineptr)
        {
            errno = ENOMEM;
            return -1;
        }
    }
    while ((c = fgetc(stream)) != EOF)
    {
        if (len + 2 > *n)
        {
            char *p = realloc(*lineptr, *n * 2);

            if (!p)
            {
                errno = ENOMEM;
                return -1;
            }
            *lineptr = p;
            *n *= 2;
        }
        (*lineptr)[len++] = (char)c;
        if (c == '\n')
            break;
    }
    if (!len)
        return -1;
    (*lineptr)[len] = 0;
    return (ssize_t)len;
}

ssize_t readlink(const char *path, char *buf, size_t len)
{
    (void)path;
    (void)buf;
    (void)len;
    errno = ENOENT;
    return -1;
}

/* amdgpu.ids in the directory of the module this code is linked into (vulkan_radeon.dll, or the program) */
const char *ros_amdgpu_ids_path(void)
{
    static char path[MAX_PATH + 16];
    HMODULE mod = NULL;
    char *slash;

    if (!path[0])
    {
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)(void *)ros_amdgpu_ids_path, &mod) ||
            !GetModuleFileNameA(mod, path, MAX_PATH))
            path[0] = 0;
        slash = strrchr(path, '\\');
        strcpy(slash ? slash + 1 : path, "amdgpu.ids");
    }
    return path;
}
