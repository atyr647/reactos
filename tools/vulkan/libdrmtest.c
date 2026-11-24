/*
 * libdrm on ReactOS (tools/vulkan/libdrm-ros): the standard libdrm API against a Linux DRM driver in the kernel.
 * Enumerates devices, reads KMS resources and modes, creates a dumb buffer, maps it with mmap(), draws, sets the mode
 * on it, and checks Linux semantics (the mapping outlives close(fd); dup/fcntl; errno on failure). Then hands a
 * non-AMD device to libdrm_amdgpu, which must refuse it cleanly.
 *   usage: libdrmtest [node]     node: the primary node to use, default the last one found (QEMU: the second display)
 * Build: x86_64-w64-mingw32-gcc -O1 -static -include ros_posix.h -I<prefix>/include/libdrm libdrmtest.c
 *        -L<prefix>/lib -ldrm_amdgpu -ldrm -lpthread -o libdrmtest.exe
 * Prints "LIBDRMTEST: ALL PASS" or "LIBDRMTEST: FAILED".
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <amdgpu.h>

static int failures;

static void L(const char *fmt, ...)
{
    char b[512];
    va_list a;

    va_start(a, fmt);
    vsnprintf(b, sizeof(b) - 2, fmt, a);
    va_end(a);
    strcat(b, "\n");
    OutputDebugStringA(b);
    fputs(b, stdout);
    fflush(stdout);
}

#define CHECK(cond, ...) do { if (cond) L("LIBDRMTEST: ok   " __VA_ARGS__); else { failures++; L("LIBDRMTEST: FAIL " __VA_ARGS__); } } while (0)

int main(int argc, char **argv)
{
    drmDevicePtr devs[8], dev = NULL, self = NULL;
    drmVersionPtr ver;
    drmModeResPtr res;
    drmModeConnectorPtr conn = NULL;
    struct drm_mode_create_dumb cd = { 0 };
    struct drm_mode_map_dumb md = { 0 };
    struct drm_mode_destroy_dumb dd = { 0 };
    amdgpu_device_handle adev;
    uint32_t major_v, minor_v, fb = 0, *px, syncobj, x, y, bad = 0, crtc = 0;
    uint64_t cap = 0;
    const char *node = argc > 1 ? argv[1] : NULL;
    int n, i, fd, fd2, r;

    /* devices */
    n = drmGetDevices2(0, devs, 8);
    CHECK(n > 0, "drmGetDevices2 -> %d device(s)", n);
    for (i = 0; i < n; i++)
    {
        L("LIBDRMTEST:   device %d: primary %s render %s bus %d %04x:%04x at %04x:%02x:%02x.%d", i,
          devs[i]->available_nodes & (1 << DRM_NODE_PRIMARY) ? devs[i]->nodes[DRM_NODE_PRIMARY] : "-",
          devs[i]->available_nodes & (1 << DRM_NODE_RENDER) ? devs[i]->nodes[DRM_NODE_RENDER] : "-",
          devs[i]->bustype,
          devs[i]->bustype == DRM_BUS_PCI ? devs[i]->deviceinfo.pci->vendor_id : 0,
          devs[i]->bustype == DRM_BUS_PCI ? devs[i]->deviceinfo.pci->device_id : 0,
          devs[i]->bustype == DRM_BUS_PCI ? devs[i]->businfo.pci->domain : 0,
          devs[i]->bustype == DRM_BUS_PCI ? devs[i]->businfo.pci->bus : 0,
          devs[i]->bustype == DRM_BUS_PCI ? devs[i]->businfo.pci->dev : 0,
          devs[i]->bustype == DRM_BUS_PCI ? devs[i]->businfo.pci->func : 0);
        if ((devs[i]->available_nodes & (1 << DRM_NODE_PRIMARY)) &&
            (!node || !_stricmp(node, devs[i]->nodes[DRM_NODE_PRIMARY])))
            dev = devs[i];
    }
    if (!dev)
    {
        L("LIBDRMTEST: FAILED (no primary node to test)");
        return 1;
    }

    fd = open(dev->nodes[DRM_NODE_PRIMARY], O_RDWR | O_BINARY);
    CHECK(fd >= 0, "open(%s) with the C runtime -> fd %d", dev->nodes[DRM_NODE_PRIMARY], fd);
    if (fd < 0)
        goto done;
    CHECK(drmGetNodeTypeFromFd(fd) == DRM_NODE_PRIMARY, "drmGetNodeTypeFromFd -> primary");
    CHECK(drmGetDevice2(fd, 0, &self) == 0 && drmDevicesEqual(self, dev), "drmGetDevice2(fd) is the same device");
    ver = drmGetVersion(fd);
    CHECK(ver && ver->name_len > 0, "drmGetVersion -> %s %d.%d.%d '%s'", ver ? ver->name : "?",
          ver ? ver->version_major : 0, ver ? ver->version_minor : 0, ver ? ver->version_patchlevel : 0,
          ver ? ver->desc : "");
    drmFreeVersion(ver);
    CHECK(drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) == 0 && cap == 1, "drmGetCap(DUMB_BUFFER) -> %llu",
          (unsigned long long)cap);

    /* dup/fcntl: the same DRM file */
    fd2 = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    ver = fd2 >= 0 ? drmGetVersion(fd2) : NULL;
    CHECK(ver != NULL, "fcntl(F_DUPFD_CLOEXEC) gives a working fd %d", fd2);
    drmFreeVersion(ver);
    if (fd2 >= 0)
        close(fd2);

    /* KMS: pointer arrays filled by the kernel (64-bit pointers in the ioctl structures) */
    res = drmModeGetResources(fd);
    CHECK(res && res->count_crtcs > 0 && res->count_connectors > 0, "drmModeGetResources -> %d crtc(s), %d connector(s), %d encoder(s)",
          res ? res->count_crtcs : 0, res ? res->count_connectors : 0, res ? res->count_encoders : 0);
    if (!res)
        goto close;
    crtc = res->crtcs[0];
    for (i = 0; i < res->count_connectors && !conn; i++)
    {
        conn = drmModeGetConnector(fd, res->connectors[i]);
        if (conn && (conn->connection != DRM_MODE_CONNECTED || !conn->count_modes))
        {
            drmModeFreeConnector(conn);
            conn = NULL;
        }
    }
    CHECK(conn != NULL, "drmModeGetConnector -> connector %u, %d mode(s), first %s",
          conn ? conn->connector_id : 0, conn ? conn->count_modes : 0, conn ? conn->modes[0].name : "-");
    if (!conn)
        goto res;

    /* a dumb buffer, mapped with mmap() */
    cd.width = conn->modes[0].hdisplay;
    cd.height = conn->modes[0].vdisplay;
    cd.bpp = 32;
    CHECK(drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == 0, "CREATE_DUMB %ux%u -> handle %u pitch %u size %llu",
          cd.width, cd.height, cd.handle, cd.pitch, (unsigned long long)cd.size);
    CHECK(drmModeAddFB(fd, cd.width, cd.height, 24, 32, cd.pitch, cd.handle, &fb) == 0, "drmModeAddFB -> fb %u", fb);
    md.handle = cd.handle;
    CHECK(drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0, "MAP_DUMB -> offset 0x%llx", (unsigned long long)md.offset);
    px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    CHECK(px != MAP_FAILED, "mmap(fd, 0x%llx) -> %p", (unsigned long long)md.offset, px);
    if (px == MAP_FAILED)
        goto fb;
    for (y = 0; y < cd.height; y++)
        for (x = 0; x < cd.width; x++)
            px[y * (cd.pitch / 4) + x] = ((x / 40 + y / 40) & 1) ? 0x0000C0FF : 0x00FF8000;   /* blue/orange */
    for (y = 0; y < cd.height; y++)
        for (x = 0; x < cd.width; x++)
            bad += px[y * (cd.pitch / 4) + x] != (((x / 40 + y / 40) & 1) ? 0x0000C0FFu : 0x00FF8000u);
    CHECK(bad == 0, "drew and read back %ux%u, %u mismatches", cd.width, cd.height, bad);
    CHECK(drmModeSetCrtc(fd, crtc, fb, 0, 0, &conn->connector_id, 1, &conn->modes[0]) == 0, "drmModeSetCrtc crtc %u fb %u", crtc, fb);
    Sleep(1500);
    /* the buffer moved to VRAM for scanout: the same pointer still reaches it */
    for (bad = 0, y = 0; y < cd.height; y++)
        for (x = 0; x < cd.width; x++)
            bad += px[y * (cd.pitch / 4) + x] != (((x / 40 + y / 40) & 1) ? 0x0000C0FFu : 0x00FF8000u);
    CHECK(bad == 0, "after the mode set the mapping still reads back, %u mismatches", bad);

    /* errors come back as errno (syncobjs are not supported by this driver, or a bogus handle is refused) */
    r = drmSyncobjCreate(fd, 0, &syncobj);
    CHECK(r == -EOPNOTSUPP, "drmSyncobjCreate on a KMS-only driver -> %d (%s): Linux EOPNOTSUPP translated", r, strerror(-r));
    r = drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &(struct drm_gem_close){ .handle = 0x7fffffff });
    CHECK(r == -1 && errno == EINVAL, "GEM_CLOSE of a bogus handle -> -1, errno %d (%s)", errno, strerror(errno));

    /* amdgpu: a bochs node is not an AMD GPU; libdrm_amdgpu must say so, not crash */
    r = amdgpu_device_initialize(fd, &major_v, &minor_v, &adev);
    CHECK(r != 0, "amdgpu_device_initialize on %s -> %d (refused)", dev->nodes[DRM_NODE_PRIMARY], r);
    if (r == 0)
        amdgpu_device_deinitialize(adev);

    CHECK(munmap(px, cd.size) == 0, "munmap");
    CHECK(munmap(px, cd.size) == -1 && errno == EINVAL, "a second munmap is refused (errno %d)", errno);
    {
        MEMORY_BASIC_INFORMATION mbi;

        CHECK(VirtualQuery(px, &mbi, sizeof(mbi)) && mbi.State == MEM_FREE, "the range is free after munmap");
    }

    /* Linux semantics: a mapping keeps its DRM file open. A buffer on its own fd: map, close(fd), write, munmap
     * (the last reference: the driver then frees the buffer with the file) */
    {
        struct drm_mode_create_dumb c2 = { .width = 64, .height = 64, .bpp = 32 };
        struct drm_mode_map_dumb m2 = { 0 };
        int fd3 = open(dev->nodes[DRM_NODE_PRIMARY], O_RDWR | O_BINARY);
        uint32_t *p2 = MAP_FAILED;

        if (fd3 >= 0 && !drmIoctl(fd3, DRM_IOCTL_MODE_CREATE_DUMB, &c2))
        {
            m2.handle = c2.handle;
            if (!drmIoctl(fd3, DRM_IOCTL_MODE_MAP_DUMB, &m2))
                p2 = mmap(NULL, c2.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd3, m2.offset);
        }
        if (fd3 >= 0)
            close(fd3);
        if (p2 != MAP_FAILED)
        {
            p2[0] = 0x12345678;
            p2[c2.size / 4 - 1] = 0x9abcdef0;
        }
        CHECK(p2 != MAP_FAILED && p2[0] == 0x12345678 && p2[c2.size / 4 - 1] == 0x9abcdef0,
              "a mapping works after close(fd)");
        CHECK(p2 != MAP_FAILED && munmap(p2, c2.size) == 0, "munmap after close(fd)");
    }
fb:
    if (fd >= 0 && fb)
        drmModeRmFB(fd, fb);
    if (fd >= 0 && cd.handle)
    {
        dd.handle = cd.handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    }
    drmModeFreeConnector(conn);
res:
    drmModeFreeResources(res);
close:
    if (fd >= 0)
        close(fd);
done:
    drmFreeDevice(&self);
    drmFreeDevices(devs, n > 8 ? 8 : n);
    L("LIBDRMTEST: %s (%d failures)", failures ? "FAILED" : "ALL PASS", failures);
    return failures != 0;
}
