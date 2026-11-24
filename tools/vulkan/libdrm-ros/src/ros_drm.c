/*
 * libdrm-ros: the core libdrm API (xf86drm.h) for ReactOS, over the Linux DRM drivers that the LinuxKPI host runs in
 * the kernel. Replaces libdrm's xf86drm.c, which finds devices through Linux's /dev and sysfs; everything else of
 * libdrm (xf86drmMode.c, libdrm_amdgpu) is compiled unchanged on top of this file.
 *
 *  - Nodes: \\.\LKPIDRM<minor>; minors as on Linux (primary 0-63, render 128-191). They are character devices, so
 *    an fd is an ordinary C runtime fd: open(), close(), dup() work, and _get_osfhandle() gives the handle.
 *  - An ioctl is one DeviceIoControl(IOCTL_LKPI_DRM) carrying the Linux ioctl number and argument
 *    (drivers/linuxkpi/lkpi_drm.h); the result is 0 or -errno as on Linux.
 *  - Devices are found by opening each node and asking which device it belongs to (IOCTL_LKPI_DRM_INFO).
 *
 * Not available: anything that passes an fd between processes or into the kernel (dma-buf/prime fds, syncobj fds,
 * sync files): the kernel side has no fd table, those ioctls fail with an errno. DRM events (read() on the fd) are not
 * delivered yet.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>

#include "xf86drm.h"
#include "drm.h"
#include "drm_fourcc.h"
#include "lkpi_drm.h"
#include "xf86drm_ros.h"
#include "sys/ioctl.h"

#define DRM_MAJOR 226

/* ---- memory ------------------------------------------------------------------------------------------------- */

void *drmMalloc(int size)
{
    return calloc(1, size);
}

void drmFree(void *pt)
{
    free(pt);
}

/* ---- ioctls ------------------------------------------------------------------------------------------------- */

static HANDLE fd_handle(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);

    return h == INVALID_HANDLE_VALUE ? NULL : h;
}

static int win32_errno(DWORD err)
{
    switch (err)
    {
    case ERROR_INVALID_HANDLE: return EBADF;
    case ERROR_INVALID_PARAMETER: return EINVAL;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_NO_SYSTEM_RESOURCES: return ENOMEM;
    case ERROR_ACCESS_DENIED: return EACCES;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION: return ENOTTY;
    case ERROR_INSUFFICIENT_BUFFER: return EINVAL;
    case ERROR_BAD_COMMAND:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_FILE_NOT_FOUND: return ENODEV;
    default: return EIO;
    }
}

/*
 * The driver returns Linux errno values; the C runtime numbers everything above ERANGE (34) differently (Linux
 * EOPNOTSUPP is 95, here 130; ETIME 62 vs 137, ECANCELED 125 vs 105, ...). Mesa compares against these (a syncobj wait
 * that times out is ETIME, a lost GPU context ECANCELED), so translate by name. Unknown codes become EIO.
 */
static int linux_errno(int e)
{
    static const struct { int lx, local; } map[] = {
        { 35, EDEADLK }, { 36, ENAMETOOLONG }, { 37, ENOLCK }, { 38, ENOSYS }, { 39, ENOTEMPTY }, { 40, ELOOP },
        { 42, ENOMSG }, { 43, EIDRM }, { 61, ENODATA }, { 62, ETIME }, { 67, ENOLINK }, { 71, EPROTO },
        { 74, EBADMSG }, { 75, EOVERFLOW }, { 84, EILSEQ }, { 95, EOPNOTSUPP }, { 105, ENOBUFS }, { 110, ETIMEDOUT },
        { 125, ECANCELED }, { 130, EOWNERDEAD }, { 131, ENOTRECOVERABLE },
    };
    size_t i;

    if (e >= 1 && e <= 34 && e != 15 && e != 26)    /* EPERM..ERANGE: the same numbers (15, 26: none here) */
        return e;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (map[i].lx == e)
            return map[i].local;
    return EIO;
}

/* One Linux ioctl on a DRM node. Returns the driver's result (>= 0) or -errno (this C runtime's numbering). */
static int ros_ioctl(HANDLE h, unsigned long request, void *arg)
{
    unsigned long size = _IOC_SIZE(request);
    size_t hdr = offsetof(LKPI_DRM_IOCTL, Data), total = hdr + (size > 8 ? size : 8);
    unsigned char stackbuf[512];
    LKPI_DRM_IOCTL *io = total <= sizeof(stackbuf) ? (LKPI_DRM_IOCTL *)stackbuf : malloc(total);
    DWORD got = 0;
    int ret;

    if (!io)
        return -ENOMEM;
    if (size && !arg)
    {
        ret = -EFAULT;
        goto out;
    }
    io->Cmd = request;
    io->Size = size;
    io->Result = 0;
    io->Pad = 0;
    if (size)
        memcpy(io->Data, arg, size);
    if (!DeviceIoControl(h, IOCTL_LKPI_DRM, io, (DWORD)(hdr + size), io, (DWORD)(hdr + size), &got, NULL))
    {
        ret = -win32_errno(GetLastError());
        goto out;
    }
    /* the driver writes back only what it may (read direction); the rest is the caller's own input */
    if (size)
        memcpy(arg, io->Data, size);
    ret = io->Result < 0 ? -linux_errno((int)-io->Result) : (int)io->Result;
out:
    if ((unsigned char *)io != stackbuf)
        free(io);
    return ret;
}

int drmIoctl(int fd, unsigned long request, void *arg)
{
    HANDLE h = fd_handle(fd);
    int ret;

    if (!h)
    {
        errno = EBADF;
        return -1;
    }
    do
        ret = ros_ioctl(h, request, arg);
    while (ret == -EINTR || ret == -EAGAIN);
    if (ret < 0)
    {
        errno = -ret;
        return -1;
    }
    return ret;
}

/* POSIX ioctl() on a DRM fd (include/sys/ioctl.h): one argument, a pointer to the structure */
int ioctl(int fd, unsigned long request, ...)
{
    HANDLE h = fd_handle(fd);
    va_list ap;
    void *arg;
    int ret;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (!h)
    {
        errno = EBADF;
        return -1;
    }
    ret = ros_ioctl(h, request, arg);
    if (ret < 0)
    {
        errno = -ret;
        return -1;
    }
    return ret;
}

int drmCommandNone(int fd, unsigned long drmCommandIndex)
{
    return drmIoctl(fd, DRM_IO(DRM_COMMAND_BASE + drmCommandIndex), NULL) ? -errno : 0;
}

int drmCommandRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size)
{
    return drmIoctl(fd, DRM_IOC(DRM_IOC_READ, DRM_IOCTL_BASE, DRM_COMMAND_BASE + drmCommandIndex, size), data) ?
           -errno : 0;
}

int drmCommandWrite(int fd, unsigned long drmCommandIndex, void *data, unsigned long size)
{
    return drmIoctl(fd, DRM_IOC(DRM_IOC_WRITE, DRM_IOCTL_BASE, DRM_COMMAND_BASE + drmCommandIndex, size), data) ?
           -errno : 0;
}

int drmCommandWriteRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size)
{
    return drmIoctl(fd, DRM_IOC(DRM_IOC_READWRITE, DRM_IOCTL_BASE, DRM_COMMAND_BASE + drmCommandIndex, size), data) ?
           -errno : 0;
}

/* ---- nodes and devices -------------------------------------------------------------------------------------- */

static int node_type(int minor)
{
    return minor >= 128 ? DRM_NODE_RENDER : DRM_NODE_PRIMARY;
}

void drmRosNodeName(int minor, char *buf, size_t len)
{
    snprintf(buf, len, "\\\\.\\LKPIDRM%d", minor);
}

/* "\\.\LKPIDRM<n>" (case-insensitive) -> n, or -1 */
static int node_minor(const char *name)
{
    static const char prefix[] = "\\\\.\\LKPIDRM";
    char *end;
    long n;

    if (!name || _strnicmp(name, prefix, sizeof(prefix) - 1))
        return -1;
    n = strtol(name + sizeof(prefix) - 1, &end, 10);
    return (*end || end == name + sizeof(prefix) - 1 || n < 0 || n > 255) ? -1 : (int)n;
}

int drmRosGetNodeDevid(const char *node, dev_t *devid)
{
    int minor = node_minor(node);

    if (minor < 0)
        return -EINVAL;
    *devid = makedev(DRM_MAJOR, minor);
    return 0;
}

static HANDLE open_node(int minor)
{
    char name[32];

    drmRosNodeName(minor, name, sizeof(name));
    return CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
                       NULL);
}

static int node_info(HANDLE h, LKPI_DRM_INFO *info)
{
    DWORD got = 0;

    memset(info, 0, sizeof(*info));
    if (!DeviceIoControl(h, IOCTL_LKPI_DRM_INFO, NULL, 0, info, sizeof(*info), &got, NULL) || got < sizeof(*info))
        return -win32_errno(GetLastError());
    return 0;
}

static int fd_info(int fd, LKPI_DRM_INFO *info)
{
    HANDLE h = fd_handle(fd);

    return h ? node_info(h, info) : -EBADF;
}

/* A drmDevice in one allocation (drmFreeDevice() frees it with one free(), as upstream) */
typedef struct {
    drmDevice dev;
    char *nodes[DRM_NODE_MAX];
    char names[DRM_NODE_MAX][24];
    drmPciBusInfo bus;
    drmPciDeviceInfo pci;
} ros_device;

static drmDevicePtr make_device(const LKPI_DRM_INFO *info)
{
    ros_device *d = calloc(1, sizeof(*d));

    if (!d)
        return NULL;
    d->dev.nodes = d->nodes;
    if (info->PrimaryMinor >= 0)
    {
        drmRosNodeName(info->PrimaryMinor, d->names[DRM_NODE_PRIMARY], sizeof(d->names[0]));
        d->nodes[DRM_NODE_PRIMARY] = d->names[DRM_NODE_PRIMARY];
        d->dev.available_nodes |= 1 << DRM_NODE_PRIMARY;
    }
    if (info->RenderMinor >= 0)
    {
        drmRosNodeName(info->RenderMinor, d->names[DRM_NODE_RENDER], sizeof(d->names[0]));
        d->nodes[DRM_NODE_RENDER] = d->names[DRM_NODE_RENDER];
        d->dev.available_nodes |= 1 << DRM_NODE_RENDER;
    }
    if (info->IsPci)
    {
        d->dev.bustype = DRM_BUS_PCI;
        d->bus.domain = info->Domain;
        d->bus.bus = info->Bus;
        d->bus.dev = info->Dev;
        d->bus.func = info->Func;
        d->pci.vendor_id = info->Vendor;
        d->pci.device_id = info->Device;
        d->pci.subvendor_id = info->SubVendor;
        d->pci.subdevice_id = info->SubDevice;
        d->pci.revision_id = info->Revision;
        d->dev.businfo.pci = &d->bus;
        d->dev.deviceinfo.pci = &d->pci;
    }
    else
    {
        d->dev.bustype = -1;        /* not a bus libdrm knows: callers check for DRM_BUS_PCI */
    }
    return &d->dev;
}

void drmFreeDevice(drmDevicePtr *device)
{
    if (device && *device)
    {
        free(*device);
        *device = NULL;
    }
}

void drmFreeDevices(drmDevicePtr devices[], int count)
{
    int i;

    if (devices)
        for (i = 0; i < count; i++)
            drmFreeDevice(&devices[i]);
}

int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
    if (!a || !b || a->bustype != b->bustype)
        return 0;
    if (a->bustype == DRM_BUS_PCI)
        return !memcmp(a->businfo.pci, b->businfo.pci, sizeof(drmPciBusInfo));
    /* no bus: the same device has the same primary node */
    return (a->available_nodes & (1 << DRM_NODE_PRIMARY)) && (b->available_nodes & (1 << DRM_NODE_PRIMARY)) &&
           !strcmp(a->nodes[DRM_NODE_PRIMARY], b->nodes[DRM_NODE_PRIMARY]);
}

/*
 * Every DRM device: render nodes first (opening one has no side effects), then primary nodes of devices without a
 * render node (KMS-only drivers such as bochs). Fills devices[] up to max_devices; returns the number of devices
 * found (devices == NULL: just count), or -errno.
 */
int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
    LKPI_DRM_INFO seen[64];
    int nseen = 0, found = 0, pass, minor, i;

    (void)flags;        /* the PCI revision is always known */
    for (pass = 0; pass < 2; pass++)
    {
        for (minor = pass ? 0 : 128; minor < (pass ? 64 : 192) && nseen < 64; minor++)
        {
            HANDLE h = open_node(minor);
            LKPI_DRM_INFO info;
            int dup = 0;

            if (h == INVALID_HANDLE_VALUE)
                continue;
            if (node_info(h, &info))
            {
                CloseHandle(h);
                continue;
            }
            CloseHandle(h);
            for (i = 0; i < nseen; i++)
                if (seen[i].PrimaryMinor == info.PrimaryMinor && seen[i].RenderMinor == info.RenderMinor)
                    dup = 1;
            if (dup)
                continue;
            seen[nseen++] = info;
            drmMsg("libdrm-ros: device %d: driver %s, primary minor %d, render minor %d, %s %04x:%04x\n", found,
                   info.Driver, info.PrimaryMinor, info.RenderMinor, info.IsPci ? "PCI" : "no bus", info.Vendor,
                   info.Device);
            if (devices && found < max_devices)
            {
                devices[found] = make_device(&info);
                if (!devices[found])
                {
                    drmFreeDevices(devices, found);
                    return -ENOMEM;
                }
            }
            found++;
        }
    }
    return devices && found > max_devices ? max_devices : found;
}

int drmGetDevices(drmDevicePtr devices[], int max_devices)
{
    return drmGetDevices2(DRM_DEVICE_GET_PCI_REVISION, devices, max_devices);
}

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
    LKPI_DRM_INFO info;
    int ret;

    (void)flags;
    if (!device)
        return -EINVAL;
    ret = fd_info(fd, &info);
    if (ret)
        return ret;
    *device = make_device(&info);
    return *device ? 0 : -ENOMEM;
}

int drmGetDevice(int fd, drmDevicePtr *device)
{
    return drmGetDevice2(fd, DRM_DEVICE_GET_PCI_REVISION, device);
}

int drmGetDeviceFromDevId(dev_t devid, uint32_t flags, drmDevicePtr *device)
{
    LKPI_DRM_INFO info;
    HANDLE h;
    int ret;

    (void)flags;
    if (!device || major(devid) != DRM_MAJOR)
        return -EINVAL;
    h = open_node(minor(devid));
    if (h == INVALID_HANDLE_VALUE)
        return -ENODEV;
    ret = node_info(h, &info);
    CloseHandle(h);
    if (ret)
        return ret;
    *device = make_device(&info);
    return *device ? 0 : -ENOMEM;
}

int drmGetNodeTypeFromFd(int fd)
{
    LKPI_DRM_INFO info;
    int ret = fd_info(fd, &info);

    if (ret)
    {
        errno = -ret;
        return -1;
    }
    return node_type(info.Minor);
}

int drmGetNodeTypeFromDevId(dev_t devid)
{
    if (major(devid) != DRM_MAJOR)
        return -EINVAL;
    return node_type(minor(devid));
}

static char *node_name_of_fd(int fd, int type)
{
    LKPI_DRM_INFO info;
    char name[32];
    int minor;

    if (fd_info(fd, &info))
        return NULL;
    minor = type == DRM_NODE_PRIMARY ? info.PrimaryMinor : type == DRM_NODE_RENDER ? info.RenderMinor : info.Minor;
    if (minor < 0)
        return NULL;
    drmRosNodeName(minor, name, sizeof(name));
    return strdup(name);
}

char *drmGetDeviceNameFromFd2(int fd)
{
    return node_name_of_fd(fd, -1);
}

char *drmGetDeviceNameFromFd(int fd)
{
    return node_name_of_fd(fd, -1);
}

char *drmGetPrimaryDeviceNameFromFd(int fd)
{
    return node_name_of_fd(fd, DRM_NODE_PRIMARY);
}

char *drmGetRenderDeviceNameFromFd(int fd)
{
    return node_name_of_fd(fd, DRM_NODE_RENDER);
}

/* ---- opening ------------------------------------------------------------------------------------------------ */

static int open_minor(int minor)
{
    char name[32];

    drmRosNodeName(minor, name, sizeof(name));
    return open(name, O_RDWR | O_BINARY);
}

int drmOpenRender(int minor)
{
    return open_minor(minor);
}

int drmOpenControl(int minor)
{
    (void)minor;
    errno = ENODEV;
    return -1;      /* control nodes are gone from Linux */
}

/* Open a device by driver name (and/or "pci:dddd:bb:dd.f" bus id), node type DRM_NODE_PRIMARY or DRM_NODE_RENDER */
int drmOpenWithType(const char *name, const char *busid, int type)
{
    drmDevicePtr devs[16];
    int n = drmGetDevices2(0, devs, 16), i, fd = -1;

    for (i = 0; i < n && fd < 0; i++)
    {
        char id[32];
        int f;

        if (!(devs[i]->available_nodes & (1 << type)))
            continue;
        if (busid)
        {
            if (devs[i]->bustype != DRM_BUS_PCI)
                continue;
            snprintf(id, sizeof(id), "pci:%04x:%02x:%02x.%d", devs[i]->businfo.pci->domain, devs[i]->businfo.pci->bus,
                     devs[i]->businfo.pci->dev, devs[i]->businfo.pci->func);
            if (_stricmp(id, busid))
                continue;
        }
        f = open(devs[i]->nodes[type], O_RDWR | O_BINARY);
        if (f < 0)
            continue;
        if (name)
        {
            drmVersionPtr v = drmGetVersion(f);
            int match = v && !strcmp(v->name, name);

            drmFreeVersion(v);
            if (!match)
            {
                close(f);
                continue;
            }
        }
        fd = f;
    }
    if (n > 0)
        drmFreeDevices(devs, n < 16 ? n : 16);
    if (fd < 0)
        errno = ENODEV;
    return fd;
}

int drmOpen(const char *name, const char *busid)
{
    return drmOpenWithType(name, busid, DRM_NODE_PRIMARY);
}

int drmClose(int fd)
{
    return close(fd);
}

int drmAvailable(void)
{
    LKPI_DRM_INFO info;
    int minor;

    for (minor = 0; minor < 64; minor++)
    {
        HANDLE h = open_node(minor);

        if (h != INVALID_HANDLE_VALUE)
        {
            int ok = !node_info(h, &info);

            CloseHandle(h);
            if (ok)
                return 1;
        }
    }
    return 0;
}

/* ---- version and capabilities ------------------------------------------------------------------------------- */

drmVersionPtr drmGetVersion(int fd)
{
    struct drm_version v;
    drmVersionPtr r;

    memset(&v, 0, sizeof(v));
    if (drmIoctl(fd, DRM_IOCTL_VERSION, &v))
        return NULL;
    r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    /* second call with buffers; the kernel writes the strings into this process's memory */
    v.name = r->name = calloc(1, v.name_len + 1);
    v.date = r->date = calloc(1, v.date_len + 1);
    v.desc = r->desc = calloc(1, v.desc_len + 1);
    if (!r->name || !r->date || !r->desc || drmIoctl(fd, DRM_IOCTL_VERSION, &v))
    {
        drmFreeVersion(r);
        return NULL;
    }
    r->version_major = v.version_major;
    r->version_minor = v.version_minor;
    r->version_patchlevel = v.version_patchlevel;
    r->name_len = (int)v.name_len;
    r->date_len = (int)v.date_len;
    r->desc_len = (int)v.desc_len;
    return r;
}

void drmFreeVersion(drmVersionPtr v)
{
    if (!v)
        return;
    free(v->name);
    free(v->date);
    free(v->desc);
    free(v);
}

drmVersionPtr drmGetLibVersion(int fd)
{
    drmVersionPtr v = calloc(1, sizeof(*v));

    (void)fd;
    if (v)
    {
        v->version_major = 1;       /* as upstream: 1.3 has drmCommand* and the device API */
        v->version_minor = 3;
    }
    return v;
}

int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
    struct drm_get_cap cap = { .capability = capability };

    if (drmIoctl(fd, DRM_IOCTL_GET_CAP, &cap))
        return -errno;
    *value = cap.value;
    return 0;
}

int drmSetClientCap(int fd, uint64_t capability, uint64_t value)
{
    struct drm_set_client_cap cap = { .capability = capability, .value = value };

    return drmIoctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) ? -errno : 0;
}

/* ---- master and authentication ------------------------------------------------------------------------------ */

int drmSetMaster(int fd)
{
    return drmIoctl(fd, DRM_IOCTL_SET_MASTER, NULL);
}

int drmDropMaster(int fd)
{
    return drmIoctl(fd, DRM_IOCTL_DROP_MASTER, NULL);
}

int drmIsMaster(int fd)
{
    /* as upstream: authenticating magic 0 needs master; a master gets EINVAL (0 is invalid), others EACCES */
    return drmAuthMagic(fd, 0) != -EACCES;
}

int drmGetMagic(int fd, drm_magic_t *magic)
{
    struct drm_auth auth = { 0 };

    if (drmIoctl(fd, DRM_IOCTL_GET_MAGIC, &auth))
        return -errno;
    *magic = auth.magic;
    return 0;
}

int drmAuthMagic(int fd, drm_magic_t magic)
{
    struct drm_auth auth = { .magic = magic };

    return drmIoctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth) ? -errno : 0;
}

/* ---- buffers ------------------------------------------------------------------------------------------------ */

int drmCloseBufferHandle(int fd, uint32_t handle)
{
    struct drm_gem_close args = { .handle = handle };

    return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
    struct drm_prime_handle args = { .handle = handle, .flags = flags, .fd = -1 };

    if (drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args))
        return -errno;      /* no fds in the kernel: fails until dma-buf sharing is bridged */
    *prime_fd = args.fd;
    return 0;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
    struct drm_prime_handle args = { .fd = prime_fd };

    if (drmIoctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &args))
        return -errno;
    *handle = args.handle;
    return 0;
}

/* ---- vblank ------------------------------------------------------------------------------------------------- */

/* union drm_wait_vblank as the Linux x86-64 kernel lays it out: its signal and tval_* fields are long, 64 bits there
 * and 32 here (abicheck.sh lists every ioctl whose layout differs; this is the only one still in use) */
union ros_wait_vblank {
    struct { uint32_t type, sequence; uint64_t signal; } request;
    struct { uint32_t type, sequence; int64_t tval_sec, tval_usec; } reply;
};
#define ROS_IOCTL_WAIT_VBLANK DRM_IOWR(0x3a, union ros_wait_vblank)

/* as upstream: -1 with errno on failure; EINTR is retried by drmIoctl() */
int drmWaitVBlank(int fd, drmVBlankPtr vbl)
{
    union ros_wait_vblank v;
    int ret;

    memset(&v, 0, sizeof(v));
    v.request.type = vbl->request.type;
    v.request.sequence = vbl->request.sequence;
    v.request.signal = vbl->request.signal;
    ret = drmIoctl(fd, ROS_IOCTL_WAIT_VBLANK, &v);
    if (ret == 0)
    {
        vbl->reply.type = v.reply.type;
        vbl->reply.sequence = v.reply.sequence;
        vbl->reply.tval_sec = (long)v.reply.tval_sec;
        vbl->reply.tval_usec = (long)v.reply.tval_usec;
    }
    return ret;
}

int drmCrtcGetSequence(int fd, uint32_t crtcId, uint64_t *sequence, uint64_t *ns)
{
    struct drm_crtc_get_sequence args = { .crtc_id = crtcId };

    if (drmIoctl(fd, DRM_IOCTL_CRTC_GET_SEQUENCE, &args))
        return -errno;
    if (sequence)
        *sequence = args.sequence;
    if (ns)
        *ns = args.sequence_ns;
    return 0;
}

/* ---- syncobj ------------------------------------------------------------------------------------------------ */

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
    struct drm_syncobj_create args = { .flags = flags };

    if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &args))
        return -errno;
    *handle = args.handle;
    return 0;
}

int drmSyncobjDestroy(int fd, uint32_t handle)
{
    struct drm_syncobj_destroy args = { .handle = handle };

    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args) ? -errno : 0;
}

int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
    struct drm_syncobj_handle args = { .handle = handle, .fd = -1 };

    if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &args))
        return -errno;
    *obj_fd = args.fd;
    return 0;
}

int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
    struct drm_syncobj_handle args = { .fd = obj_fd };

    if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &args))
        return -errno;
    *handle = args.handle;
    return 0;
}

int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
    struct drm_syncobj_handle args = { .fd = sync_file_fd, .handle = handle,
                                       .flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE };

    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &args) ? -errno : 0;
}

int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
    struct drm_syncobj_handle args = { .fd = -1, .handle = handle,
                                       .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE };

    if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &args))
        return -errno;
    *sync_file_fd = args.fd;
    return 0;
}

int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                   uint32_t *first_signaled)
{
    struct drm_syncobj_wait args = { .handles = (uintptr_t)handles, .timeout_nsec = timeout_nsec,
                                     .count_handles = num_handles, .flags = flags };

    if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args))
        return -errno;
    if (first_signaled)
        *first_signaled = args.first_signaled;
    return 0;
}

int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
    struct drm_syncobj_array args = { .handles = (uintptr_t)handles, .count_handles = handle_count };

    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &args) ? -errno : 0;
}

int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
    struct drm_syncobj_array args = { .handles = (uintptr_t)handles, .count_handles = handle_count };

    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &args) ? -errno : 0;
}

int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
    struct drm_syncobj_timeline_array args = { .handles = (uintptr_t)handles, .points = (uintptr_t)points,
                                               .count_handles = handle_count };

    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &args) ? -errno : 0;
}

int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec,
                           unsigned flags, uint32_t *first_signaled)
{
    struct drm_syncobj_timeline_wait args = { .handles = (uintptr_t)handles, .points = (uintptr_t)points,
                                              .timeout_nsec = timeout_nsec, .count_handles = num_handles,
                                              .flags = flags };

    if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &args))
        return -errno;
    if (first_signaled)
        *first_signaled = args.first_signaled;
    return 0;
}

int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags)
{
    struct drm_syncobj_timeline_array args = { .handles = (uintptr_t)handles, .points = (uintptr_t)points,
                                               .count_handles = handle_count, .flags = flags };

    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &args) ? -errno : 0;
}

int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
    return drmSyncobjQuery2(fd, handles, points, handle_count, 0);
}

int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
                       uint32_t flags)
{
    struct drm_syncobj_transfer args = { .src_handle = src_handle, .dst_handle = dst_handle,
                                         .src_point = src_point, .dst_point = dst_point, .flags = flags };

    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &args) ? -errno : 0;
}

/* ---- format modifiers --------------------------------------------------------------------------------------- */

/* A malloc()ed name for debugging output (upstream decodes every vendor's tiling fields; here: vendor and value) */
char *drmGetFormatModifierName(uint64_t modifier)
{
    static const char *const vendors[] = { "NONE", "INTEL", "AMD", "NVIDIA", "SAMSUNG", "QCOM", "VIVANTE",
                                           "BROADCOM", "ARM", "ALLWINNER", "AMLOGIC" };
    unsigned vendor = (unsigned)(modifier >> 56);
    char buf[64];

    if (modifier == DRM_FORMAT_MOD_INVALID)
        return strdup("INVALID");
    if (modifier == DRM_FORMAT_MOD_LINEAR)
        return strdup("LINEAR");
    if (vendor < sizeof(vendors) / sizeof(vendors[0]))
        snprintf(buf, sizeof(buf), "%s_0x%014llx", vendors[vendor],
                 (unsigned long long)(modifier & 0x00ffffffffffffffULL));
    else
        snprintf(buf, sizeof(buf), "0x%016llx", (unsigned long long)modifier);
    return strdup(buf);
}

char *drmGetFormatModifierVendor(uint64_t modifier)
{
    char *name = drmGetFormatModifierName(modifier), *us = name ? strchr(name, '_') : NULL;

    if (us)
        *us = 0;
    return name;
}

/* ---- messages ----------------------------------------------------------------------------------------------- */

void drmMsg(const char *format, ...)
{
    const char *dbg = getenv("LIBGL_DEBUG");
    va_list ap;

    if (dbg && strstr(dbg, "verbose"))
    {
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
    }
}
