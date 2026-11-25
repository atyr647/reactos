/*
 * GPU buffer and fence sharing on ReactOS (libdrm-ros over the LinuxKPI host): the Linux fds a DRM driver hands out
 * (dma-bufs, sync files, syncobj fds) as C runtime fds, within a process and across processes. Uses vgem, Linux's
 * virtual GEM driver (a test driver: buffers in system memory, fences signalled from user mode), so it runs without a
 * GPU; amdgpu shares through the same code.
 *   - dma-buf: export (PRIME), mmap of the dma-buf fd, import on another DRM fd (same pages), self-import (same
 *     handle), import after the exporting handle is gone, dup(), errors for fds that are not dma-bufs
 *   - another process: the dma-buf handle is inherited; the child imports it, checks and rewrites the pixels
 *   - sync files: export from a dma-buf with an unsignalled vgem fence, poll() timeout, wake-up from another thread,
 *     merge, poll() of the dma-buf itself, import back into a dma-buf
 *   - syncobjs: sync file import/export, syncobj fd export/import on another DRM fd, wait
 *   - no leaks: 2000 export/close rounds leave the process handle count where it was
 *   usage: sharetest [node]      node: vgem's primary node (default: found by driver name)
 * vgem is a test module: build the tree with drivers/linuxkpi/kpi/linux-6.12.config (CONFIG_DRM_VGEM=m) and add
 * "drivers/gpu/drm/drm_shmem_helper.ko;drivers/gpu/drm/vgem/vgem.ko" to the end of LKPI_GPU_MODULES.
 * Build: x86_64-w64-mingw32-gcc -O1 -static -include ros_posix.h -I<prefix>/include/libdrm sharetest.c
 *        -L<prefix>/lib -ldrm -lpthread -o sharetest.exe       (build-radv-x64.sh does this)
 * Prints "SHARETEST: ALL PASS" or "SHARETEST: FAILED".
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
#include <poll.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drm_ros.h>
#include <libsync.h>

/* include/uapi/drm/vgem_drm.h */
struct drm_vgem_fence_attach { uint32_t handle, flags, out_fence, pad; };
struct drm_vgem_fence_signal { uint32_t fence, flags; };
#define VGEM_FENCE_WRITE 0x1
#define DRM_IOCTL_VGEM_FENCE_ATTACH DRM_IOWR(DRM_COMMAND_BASE + 0x1, struct drm_vgem_fence_attach)
#define DRM_IOCTL_VGEM_FENCE_SIGNAL DRM_IOW(DRM_COMMAND_BASE + 0x2, struct drm_vgem_fence_signal)

/* include/uapi/linux/dma-buf.h */
struct dma_buf_export_sync_file { uint32_t flags; int32_t fd; };
struct dma_buf_import_sync_file { uint32_t flags; int32_t fd; };
#define DMA_BUF_SYNC_READ  (1 << 0)
#define DMA_BUF_SYNC_WRITE (2 << 0)
#define DMA_BUF_SYNC_RW    (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR('b', 2, struct dma_buf_export_sync_file)
#define DMA_BUF_IOCTL_IMPORT_SYNC_FILE _IOW('b', 3, struct dma_buf_import_sync_file)

#define W 256
#define H 64
#define SIZE (W * H * 4)

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

#define CHECK(cond, ...) do { if (cond) L("SHARETEST: ok   " __VA_ARGS__); else { failures++; L("SHARETEST: FAIL " __VA_ARGS__); } } while (0)

static int find_vgem(char *node, size_t len)
{
    int minor, fd;

    for (minor = 0; minor < 64; minor++)
    {
        drmVersionPtr v;

        drmRosNodeName(minor, node, len);
        fd = open(node, O_RDWR | O_BINARY);
        if (fd < 0)
            continue;
        v = drmGetVersion(fd);
        close(fd);
        if (v && !strcmp(v->name, "vgem"))
        {
            drmFreeVersion(v);
            return 0;
        }
        drmFreeVersion(v);
    }
    return -1;
}

/* a W x H 32-bit dumb buffer; *map: its mapping through the DRM fd */
static int make_buffer(int fd, uint32_t *handle, uint32_t **map)
{
    struct drm_mode_create_dumb cd = { .width = W, .height = H, .bpp = 32 };
    struct drm_mode_map_dumb md = { 0 };
    void *p;

    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd))
        return -errno;
    md.handle = cd.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
        return -errno;
    p = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (p == MAP_FAILED)
        return -errno;
    *handle = cd.handle;
    *map = p;
    return 0;
}

static unsigned bad_pixels(const uint32_t *px, uint32_t seed)
{
    unsigned i, bad = 0;

    for (i = 0; i < W * H; i++)
        bad += px[i] != (i * 2654435761u ^ seed);
    return bad;
}

static void fill(uint32_t *px, uint32_t seed)
{
    unsigned i;

    for (i = 0; i < W * H; i++)
        px[i] = i * 2654435761u ^ seed;
}

/* the child process: argv: sharetest child <node> <dma-buf handle> */
static int child(const char *node, const char *handle_text)
{
    HANDLE h = (HANDLE)(uintptr_t)strtoull(handle_text, NULL, 0);
    int buf = drmRosFdFromHandle(h), fd = open(node, O_RDWR | O_BINARY), r;
    uint32_t handle = 0, *px;
    void *p;

    CHECK(buf >= 0, "child: fd %d for the inherited dma-buf handle %p", buf, h);
    CHECK(fd >= 0, "child: open %s", node);
    r = drmPrimeFDToHandle(fd, buf, &handle);
    CHECK(r == 0 && handle, "child: import -> GEM handle %u (%d)", handle, r);
    p = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, buf, 0);
    CHECK(p != MAP_FAILED, "child: mmap of the dma-buf");
    if (p == MAP_FAILED)
        return 1;
    px = p;
    CHECK(bad_pixels(px, 0x5EED0001) == 0, "child: the parent's pixels are there");
    fill(px, 0xC41D0002);
    munmap(p, SIZE);
    drmCloseBufferHandle(fd, handle);
    close(buf);
    close(fd);
    return failures ? 2 : 0;
}

struct signal_args {
    int fd;
    uint32_t fence;
    DWORD delay;
};

static DWORD WINAPI signal_later(void *arg)
{
    struct signal_args *a = arg;
    struct drm_vgem_fence_signal s = { .fence = a->fence };

    Sleep(a->delay);
    return drmIoctl(a->fd, DRM_IOCTL_VGEM_FENCE_SIGNAL, &s) ? errno : 0;
}

static int attach_fence(int fd, uint32_t handle, uint32_t *fence)
{
    struct drm_vgem_fence_attach fa = { .handle = handle, .flags = VGEM_FENCE_WRITE };

    if (drmIoctl(fd, DRM_IOCTL_VGEM_FENCE_ATTACH, &fa))
        return -errno;
    *fence = fa.out_fence;
    return 0;
}

static int signal_fence(int fd, uint32_t fence)
{
    struct drm_vgem_fence_signal s = { .fence = fence };

    return drmIoctl(fd, DRM_IOCTL_VGEM_FENCE_SIGNAL, &s) ? -errno : 0;
}

static int export_sync_file(int buf, uint32_t flags)
{
    struct dma_buf_export_sync_file e = { .flags = flags, .fd = -1 };

    return ioctl(buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &e) ? -errno : e.fd;
}

static short poll1(int fd, short events, int timeout, DWORD *ms)
{
    struct pollfd p = { .fd = fd, .events = events };
    DWORD t = GetTickCount();
    int r = poll(&p, 1, timeout);

    if (ms)
        *ms = GetTickCount() - t;
    return r < 0 ? -1 : p.revents;
}

static void test_dma_buf(const char *node, const char *self)
{
    uint32_t handle, handle2 = 0, handle3 = 0, *px, *px2;
    int fd = open(node, O_RDWR | O_BINARY), fd2, buf = -1, buf2 = -1, dupfd, nul, r;
    char cmd[MAX_PATH * 2];
    STARTUPINFOA si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD code = 99, before, after;
    void *p;
    int i;

    CHECK(fd >= 0, "open %s", node);
    r = make_buffer(fd, &handle, &px);
    CHECK(r == 0, "dumb buffer %ux%u, mapped (%d)", W, H, r);
    if (r)
        return;
    fill(px, 0x5EED0001);

    r = drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC | DRM_RDWR, &buf);
    CHECK(r == 0 && buf >= 0, "PRIME export -> dma-buf fd %d (%d)", buf, r);
    if (r)
        return;
    CHECK(_get_osfhandle(buf) != -1, "the dma-buf fd is a C runtime fd (handle %p)", (void *)_get_osfhandle(buf));

    /* the dma-buf maps to the same pages */
    p = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, buf, 0);
    CHECK(p != MAP_FAILED, "mmap of the dma-buf fd");
    if (p != MAP_FAILED)
    {
        CHECK(bad_pixels(p, 0x5EED0001) == 0, "the dma-buf mapping shows the buffer's pixels");
        ((uint32_t *)p)[7] = 0xABCD1234;
        CHECK(px[7] == 0xABCD1234, "a write through the dma-buf mapping is in the buffer");
        px[7] = 7 * 2654435761u ^ 0x5EED0001;
        munmap(p, SIZE);
    }

    /* the same buffer through another DRM fd: a new GEM handle, the same pages */
    fd2 = open(node, O_RDWR | O_BINARY);
    r = drmPrimeFDToHandle(fd2, buf, &handle2);
    CHECK(r == 0 && handle2, "import on a second DRM fd -> handle %u (%d)", handle2, r);
    r = drmPrimeFDToHandle(fd, buf, &handle3);
    CHECK(r == 0 && handle3 == handle, "import on the exporting fd gives the original handle (%u, %u)", handle3, handle);
    {
        struct drm_mode_map_dumb md = { .handle = handle2 };

        r = drmIoctl(fd2, DRM_IOCTL_MODE_MAP_DUMB, &md);
        px2 = r ? MAP_FAILED : mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, md.offset);
        CHECK(px2 != MAP_FAILED, "map the imported handle");
        if (px2 != MAP_FAILED)
        {
            CHECK(bad_pixels(px2, 0x5EED0001) == 0, "the importer sees the exporter's pixels");
            munmap(px2, SIZE);
        }
    }

    /* fds: dup shares the dma-buf; exporting again gives a dma-buf that imports to the same handle */
    dupfd = dup(buf);
    r = drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC, &buf2);
    CHECK(r == 0 && buf2 >= 0 && buf2 != buf, "export again -> fd %d", buf2);
    handle3 = 0;
    r = drmPrimeFDToHandle(fd2, dupfd, &handle3);
    CHECK(r == 0 && handle3 == handle2, "a dup() of the dma-buf fd imports to the same handle (%u)", handle3);
    close(dupfd);
    close(buf2);

    /* errors */
    nul = open("NUL", O_RDWR | O_BINARY);
    errno = 0;
    r = drmPrimeFDToHandle(fd2, nul, &handle3);
    CHECK(r == -EBADF, "import of an fd that is not a dma-buf -> %d (EBADF %d)", r, -EBADF);
    r = drmPrimeFDToHandle(fd2, fd, &handle3);
    CHECK(r == -EINVAL || r == -EBADF, "import of a DRM fd -> %d", r);
    r = drmPrimeFDToHandle(fd2, -1, &handle3);
    CHECK(r == -EBADF, "import of fd -1 -> %d", r);
    close(nul);

    /* another process: inherits the dma-buf handle, imports it, checks the pixels and writes new ones */
    SetHandleInformation((HANDLE)_get_osfhandle(buf), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    snprintf(cmd, sizeof(cmd), "\"%s\" child %s 0x%llx", self, node, (unsigned long long)_get_osfhandle(buf));
    if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 60000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    SetHandleInformation((HANDLE)_get_osfhandle(buf), HANDLE_FLAG_INHERIT, 0);
    CHECK(code == 0, "child process imported the dma-buf (exit code %lu)", code);
    CHECK(bad_pixels(px, 0xC41D0002) == 0, "the child's pixels are in the parent's buffer");

    /* the dma-buf keeps the buffer alive: drop every GEM handle and mapping, import again */
    munmap(px, SIZE);
    drmCloseBufferHandle(fd, handle);
    drmCloseBufferHandle(fd2, handle2);
    handle2 = 0;
    r = drmPrimeFDToHandle(fd2, buf, &handle2);
    p = MAP_FAILED;
    if (!r)
    {
        p = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, buf, 0);
        CHECK(p != MAP_FAILED && bad_pixels(p, 0xC41D0002) == 0, "only the dma-buf left: imports again, pixels intact");
        if (p != MAP_FAILED)
            munmap(p, SIZE);
    }
    else
        CHECK(0, "only the dma-buf left: import again (%d)", r);
    drmCloseBufferHandle(fd2, handle2);

    /* handles: many export/close rounds must not leak */
    r = make_buffer(fd, &handle, &px);
    GetProcessHandleCount(GetCurrentProcess(), &before);
    for (i = 0; i < 2000 && !r; i++)
    {
        int b;

        r = drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC, &b);
        if (!r)
        {
            r = drmPrimeFDToHandle(fd2, b, &handle2);
            close(b);
            if (!r)
                drmCloseBufferHandle(fd2, handle2);
        }
    }
    GetProcessHandleCount(GetCurrentProcess(), &after);
    CHECK(r == 0 && after == before, "2000 export/import/close rounds: handles %lu -> %lu (%d)", before, after, r);
    munmap(px, SIZE);
    drmCloseBufferHandle(fd, handle);

    close(buf);
    close(fd2);
    close(fd);
}

static void test_fences(const char *node)
{
    int fd = open(node, O_RDWR | O_BINARY), fd2 = open(node, O_RDWR | O_BINARY), buf = -1, sf, sf2, merged, r;
    uint32_t handle, *px, fence, fence2, so = 0, so2 = 0;
    struct signal_args sa;
    HANDLE t;
    DWORD ms, exit_code = 1;
    short ev;

    if (make_buffer(fd, &handle, &px) || drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC | DRM_RDWR, &buf))
    {
        CHECK(0, "buffer and dma-buf for the fence tests");
        return;
    }

    /* nothing pending: the sync file of an idle buffer is signalled */
    sf = export_sync_file(buf, DMA_BUF_SYNC_RW);
    CHECK(sf >= 0, "sync file of an idle dma-buf -> fd %d", sf);
    CHECK(poll1(sf, POLLIN, 0, NULL) == POLLIN, "it is signalled");
    close(sf);

    r = attach_fence(fd, handle, &fence);
    CHECK(r == 0, "vgem write fence attached (%d)", r);
    sf = export_sync_file(buf, DMA_BUF_SYNC_RW);
    CHECK(sf >= 0, "sync file for the pending write -> fd %d", sf);
    CHECK(poll1(sf, POLLIN, 0, NULL) == 0, "poll(0): not signalled");
    ev = poll1(sf, POLLIN, 150, &ms);
    CHECK(ev == 0 && ms >= 120 && ms < 2000, "poll(150 ms) times out (%d after %lu ms)", ev, ms);
    CHECK(sync_wait(sf, 0) < 0 && errno == ETIME, "sync_wait(0) -> ETIME");
    CHECK(poll1(buf, POLLIN, 0, NULL) == 0, "the dma-buf itself is busy for readers");

    /* syncobjs: the sync file goes into one, its fd to another DRM fd, a sync file back out */
    r = drmSyncobjCreate(fd, 0, &so);
    CHECK(r == 0, "syncobj created (%d)", r);
    r = drmSyncobjImportSyncFile(fd, so, sf);
    CHECK(r == 0, "sync file imported into the syncobj (%d)", r);
    r = drmSyncobjWait(fd, &so, 1, 0, 0, NULL);
    CHECK(r == -ETIME, "syncobj wait(0) -> %d (ETIME)", r);
    {
        int sofd = -1, back = -1;

        r = drmSyncobjHandleToFD(fd, so, &sofd);
        CHECK(r == 0 && sofd >= 0, "syncobj -> fd %d", sofd);
        r = drmSyncobjFDToHandle(fd2, sofd, &so2);
        CHECK(r == 0 && so2, "syncobj fd -> handle %u on the second DRM fd", so2);
        close(sofd);
        r = drmSyncobjExportSyncFile(fd2, so2, &back);
        CHECK(r == 0 && back >= 0 && poll1(back, POLLIN, 0, NULL) == 0, "its sync file is the pending fence");
        if (back >= 0)
            close(back);
    }

    /* another fence on the same buffer is refused while the first is pending; merge with a second buffer's */
    {
        uint32_t h2, *px2;
        int buf2 = -1;

        r = make_buffer(fd, &h2, &px2);
        if (!r)
            r = drmPrimeHandleToFD(fd, h2, DRM_CLOEXEC, &buf2);
        if (!r)
            r = attach_fence(fd, h2, &fence2);
        sf2 = r ? -1 : export_sync_file(buf2, DMA_BUF_SYNC_RW);
        merged = sf2 < 0 ? -1 : sync_merge("sharetest", sf, sf2);
        CHECK(merged >= 0, "two sync files merged -> fd %d", merged);
        r = signal_fence(fd, fence2);
        CHECK(r == 0, "second fence signalled");
        CHECK(poll1(sf2, POLLIN, 0, NULL) == POLLIN, "its sync file is signalled");
        CHECK(poll1(merged, POLLIN, 0, NULL) == 0, "the merge still waits for the first");
        close(sf2);
        close(buf2);
        munmap(px2, SIZE);
        drmCloseBufferHandle(fd, h2);
    }

    /* the first fence is signalled by another thread while poll() waits */
    sa.fd = fd;
    sa.fence = fence;
    sa.delay = 300;
    t = CreateThread(NULL, 0, signal_later, &sa, 0, NULL);
    ev = poll1(sf, POLLIN, 10000, &ms);
    CHECK(ev == POLLIN && ms >= 200 && ms < 5000, "poll() wakes when the fence is signalled (%d after %lu ms)", ev, ms);
    WaitForSingleObject(t, INFINITE);
    GetExitCodeThread(t, &exit_code);
    CloseHandle(t);
    CHECK(exit_code == 0, "signalled from the other thread (%lu)", exit_code);
    CHECK(poll1(merged, POLLIN, 1000, NULL) == POLLIN, "the merged sync file is signalled");
    CHECK(sync_wait(sf, 0) == 0, "sync_wait(0) -> 0");
    CHECK(poll1(buf, POLLIN | POLLOUT, 0, NULL) == (POLLIN | POLLOUT), "the dma-buf is idle");
    r = drmSyncobjWait(fd2, &so2, 1, 0, 0, NULL);
    CHECK(r == 0, "the imported syncobj is signalled (%d)", r);

    /* a sync file into a dma-buf: the buffer is busy until it signals */
    r = attach_fence(fd, handle, &fence);
    sf2 = r ? -1 : export_sync_file(buf, DMA_BUF_SYNC_WRITE);
    if (sf2 >= 0)
    {
        uint32_t h3, *px3;
        int buf3 = -1;
        struct dma_buf_import_sync_file im = { .flags = DMA_BUF_SYNC_WRITE };

        r = make_buffer(fd, &h3, &px3);
        if (!r)
            r = drmPrimeHandleToFD(fd, h3, DRM_CLOEXEC | DRM_RDWR, &buf3);
        im.fd = sf2;
        r = r ? r : ioctl(buf3, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &im) ? -errno : 0;
        CHECK(r == 0 && im.fd == sf2, "sync file imported into another dma-buf (%d)", r);
        CHECK(poll1(buf3, POLLIN, 0, NULL) == 0, "that dma-buf is now busy");
        signal_fence(fd, fence);
        CHECK(poll1(buf3, POLLIN, 1000, NULL) == POLLIN, "and idle once the fence signals");
        close(buf3);
        munmap(px3, SIZE);
        drmCloseBufferHandle(fd, h3);
        close(sf2);
    }
    else
        CHECK(0, "second sync file (%d)", sf2);

    CHECK(poll1(-1, POLLIN, 0, NULL) == 0, "poll() ignores fd -1");
    {
        int nul = open("NUL", O_RDWR | O_BINARY);

        CHECK(poll1(nul, POLLIN, 0, NULL) == POLLNVAL, "poll() of an fd that is not a Linux file -> POLLNVAL");
        close(nul);
    }

    drmSyncobjDestroy(fd, so);
    drmSyncobjDestroy(fd2, so2);
    close(merged);
    close(sf);
    close(buf);
    munmap(px, SIZE);
    drmCloseBufferHandle(fd, handle);
    close(fd2);
    close(fd);
}

int main(int argc, char **argv)
{
    char node[64], self[MAX_PATH];

    if (argc == 4 && !strcmp(argv[1], "child"))
        return child(argv[2], argv[3]);
    if (argc > 1)
        snprintf(node, sizeof(node), "%s", argv[1]);
    else if (find_vgem(node, sizeof(node)))
    {
        L("SHARETEST: FAILED (no vgem device; load vgem.ko)");
        return 1;
    }
    L("SHARETEST: vgem at %s", node);
    GetModuleFileNameA(NULL, self, sizeof(self));
    test_dma_buf(node, self);
    test_fences(node);
    L(failures ? "SHARETEST: FAILED (%d)" : "SHARETEST: ALL PASS", failures);
    return failures ? 1 : 0;
}
