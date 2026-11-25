/* Linux KMS from a ReactOS user program: dumb buffer -> mmap (IOCTL_LKPI_DRM_MMAP) -> draw -> SETCRTC -> redraw after
 * the buffer moved -> munmap. Usage: drmkms [minor] [hold seconds] [noset | nounmap | dupexit]
 * Build: x86_64-w64-mingw32-gcc -O1 -o drmkms.exe drmkms.c */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include "../../drivers/linuxkpi/lkpi_drm.h"

static void L(const char *fmt, ...) { char b[400]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b - 2, fmt, a); va_end(a); strcat(b, "\n"); OutputDebugStringA(b); fputs(b, stdout); fflush(stdout); }
static HANDLE dev;
static long ioc(unsigned long cmd, void *arg)
{
    unsigned size = (cmd >> 16) & 0x3FFF;
    unsigned char buf[sizeof(LKPI_DRM_IOCTL) + 256];
    LKPI_DRM_IOCTL *io = (LKPI_DRM_IOCTL *)buf;
    unsigned hdr = (unsigned)FIELD_OFFSET(LKPI_DRM_IOCTL, Data);
    DWORD ret;
    io->Cmd = cmd; io->Size = size; io->Result = 0;
    memcpy(io->Data, arg, size);
    if (!DeviceIoControl(dev, IOCTL_LKPI_DRM, io, hdr + size, io, hdr + size, &ret, NULL)) return -1000 - (long)GetLastError();
    memcpy(arg, io->Data, size);
    return io->Result;
}
struct card_res { uint64_t fb, crtc, conn, enc; uint32_t n_fb, n_crtc, n_conn, n_enc, minw, maxw, minh, maxh; };
struct get_conn { uint64_t enc_ptr, modes_ptr, props_ptr, propv_ptr; uint32_t n_modes, n_props, n_enc, enc_id, conn_id, type, type_id, connection, mmw, mmh, subpixel, pad; };
struct modeinfo { uint32_t clock; uint16_t hd, hss, hse, ht, hskew, vd, vss, vse, vt, vscan; uint32_t vrefresh, flags, type; char name[32]; };
struct create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct fb_cmd { uint32_t fb_id, width, height, pitch, bpp, depth, handle; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };
struct mode_crtc { uint64_t set_conn_ptr; uint32_t count_conn, crtc_id, fb_id, x, y, gamma, mode_valid; struct modeinfo mode; };

static uint32_t color(int x, int y, int w, int h)
{
    static const uint32_t bars[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
    if (x < 4 || y < 4 || x >= w - 4 || y >= h - 4) return 0xFFFFFF;     /* border */
    if (y < h * 2 / 3) return bars[x * 8 / w];
    return (uint32_t)((x * 255 / w) << 16 | (y * 255 / h) << 8 | 0x80);   /* gradient */
}

int main(int argc, char **argv)
{
    char path[32]; uint32_t crtcs[8] = {0}, conns[8] = {0}; struct card_res res = {0}; struct modeinfo modes[32];
    struct get_conn c = {0}; struct create_dumb cd = {0}; struct fb_cmd fb = {0}; struct map_dumb md = {0};
    struct mode_crtc mc = {0}; LKPI_DRM_MMAP mm = {0}; DWORD n; uint32_t *px, conn = 0, i, bad = 0, x, y;
    int ok = 1, m = 0, hold = argc > 2 ? atoi(argv[2]) : 15; long r;
    MEMORY_BASIC_INFORMATION mbi;

    snprintf(path, sizeof path, "\\\\.\\LKPIDRM%s", argc > 1 ? argv[1] : "1");
    if (argc > 1 && !strcmp(argv[1], "child")) { L("DRMKMS: child holds the inherited handle"); Sleep(8000); L("DRMKMS: child exits"); return 0; }
    {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
        dev = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);
    }
    if (dev == INVALID_HANDLE_VALUE) { L("DRMKMS: open %s failed %lu", path, GetLastError()); return 1; }
    res.crtc = (uintptr_t)crtcs; res.conn = (uintptr_t)conns; res.n_crtc = 8; res.n_conn = 8;
    r = ioc(0xC04064A0, &res);
    L("DRMKMS: %s GETRESOURCES -> %ld: %u crtcs, %u connectors, max %ux%u", path, r, res.n_crtc, res.n_conn, res.maxw, res.maxh);
    for (i = 0; i < res.n_conn && i < 8 && !conn; i++) {
        memset(&c, 0, sizeof c); c.conn_id = conns[i];
        ioc(0xC05064A7, &c);                          /* probe: counts */
        if (c.connection != 1 || !c.n_modes) continue;
        if (c.n_modes > 32) c.n_modes = 32;
        c.modes_ptr = (uintptr_t)modes; c.n_props = 0; c.n_enc = 0;
        r = ioc(0xC05064A7, &c);
        if (r == 0) conn = conns[i];
    }
    if (!conn) { L("DRMKMS: no connected connector"); return 2; }
    for (i = 0; i < c.n_modes; i++) if (modes[i].type & 8) { m = i; break; }      /* preferred */
    L("DRMKMS: connector %u, %u modes, using %s %ux%u@%u", conn, c.n_modes, modes[m].name, modes[m].hd, modes[m].vd, modes[m].vrefresh);

    cd.width = modes[m].hd; cd.height = modes[m].vd; cd.bpp = 32;
    r = ioc(0xC02064B2, &cd);
    L("DRMKMS: CREATE_DUMB -> %ld: handle %u pitch %u size %llu", r, cd.handle, cd.pitch, (unsigned long long)cd.size);
    fb.width = cd.width; fb.height = cd.height; fb.pitch = cd.pitch; fb.bpp = 32; fb.depth = 24; fb.handle = cd.handle;
    r = ioc(0xC01C64AE, &fb);
    L("DRMKMS: ADDFB -> %ld: fb %u", r, fb.fb_id);
    md.handle = cd.handle;
    r = ioc(0xC01064B3, &md);
    L("DRMKMS: MAP_DUMB -> %ld: offset 0x%llx", r, (unsigned long long)md.offset);

    mm.Offset = 0x7FFF0000000ULL; mm.Size = 4096;     /* a bogus offset must be refused, not crash */
    L("DRMKMS: mmap of a bogus offset -> %s (err %lu)", DeviceIoControl(dev, IOCTL_LKPI_DRM_MMAP, &mm, sizeof mm, &mm, sizeof mm, &n, NULL) ? "MAPPED (bad)" : "refused", GetLastError());
    if (mm.Address) ok = 0;

    mm.Offset = md.offset; mm.Size = (cd.size + 4095) & ~4095ULL; mm.Address = 0;
    if (!DeviceIoControl(dev, IOCTL_LKPI_DRM_MMAP, &mm, sizeof mm, &mm, sizeof mm, &n, NULL)) { L("DRMKMS: mmap failed %lu", GetLastError()); return 3; }
    px = (uint32_t *)(uintptr_t)mm.Address;
    VirtualQuery(px, &mbi, sizeof mbi);
    L("DRMKMS: mapped %llu bytes at %p (state %lx, protect %lx)", (unsigned long long)mm.Size, px, mbi.State, mbi.Protect);
    for (y = 0; y < cd.height; y++) for (x = 0; x < cd.width; x++) px[y * (cd.pitch / 4) + x] = color(x, y, cd.width, cd.height);
    for (y = 0; y < cd.height; y += 7) for (x = 0; x < cd.width; x += 5) if (px[y * (cd.pitch / 4) + x] != color(x, y, cd.width, cd.height)) bad++;
    L("DRMKMS: drew %ux%u, read back %u mismatches -> %s", cd.width, cd.height, bad, bad ? "FAIL" : "PASS");
    ok &= !bad;

    if (argc > 3 && !strcmp(argv[3], "noset")) { hold = 0; goto unmap; }    /* map/unmap only: the buffer never moves */
    mc.set_conn_ptr = (uintptr_t)&conn; mc.count_conn = 1; mc.crtc_id = crtcs[0]; mc.fb_id = fb.fb_id; mc.mode_valid = 1; mc.mode = modes[m];
    r = ioc(0xC06864A2, &mc);
    L("DRMKMS: SETCRTC crtc %u fb %u -> %ld -> %s", mc.crtc_id, fb.fb_id, r, r ? "FAIL" : "PASS");
    ok &= r == 0;
    /* the scanout pin moved the buffer (system memory -> VRAM): the mapping must follow it. Draw again through the
       same pointer; the second display must now show the checkerboard. */
    Sleep(3000);
    bad = 0;
    for (y = 0; y < cd.height; y++) for (x = 0; x < cd.width; x++)
        px[y * (cd.pitch / 4) + x] = ((x / 64) ^ (y / 64)) & 1 ? 0x00C000 : 0xC000C0;
    for (y = 0; y < cd.height; y += 7) for (x = 0; x < cd.width; x += 5)
        if (px[y * (cd.pitch / 4) + x] != (((x / 64) ^ (y / 64)) & 1 ? 0x00C000 : 0xC000C0)) bad++;
    L("DRMKMS: redrew a checkerboard after the buffer moved, %u mismatches -> %s", bad, bad ? "FAIL" : "PASS");
    ok &= !bad;
    L("DRMKMS: holding the picture for %d s", hold);
    Sleep(hold * 1000);

unmap:
    if (argc > 3 && !strcmp(argv[3], "dupexit")) {
        STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi; char cmd[MAX_PATH + 16];
        GetModuleFileNameA(NULL, cmd, MAX_PATH); strcat(cmd, " child");
        if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) { L("DRMKMS: FAILED to start the child"); return 1; }
        L("DRMKMS: exiting with the buffer mapped, the child keeps the handle"); ExitProcess(0);
    }
    if (argc > 3 && !strcmp(argv[3], "nounmap")) { L("DRMKMS: exiting with the buffer mapped"); ExitProcess(0); }
    if (!DeviceIoControl(dev, IOCTL_LKPI_DRM_MUNMAP, &mm, sizeof mm, NULL, 0, &n, NULL)) { L("DRMKMS: munmap failed %lu", GetLastError()); ok = 0; }
    VirtualQuery(px, &mbi, sizeof mbi);
    L("DRMKMS: after munmap the range is %s -> %s", mbi.State == MEM_FREE ? "free" : "STILL MAPPED", mbi.State == MEM_FREE ? "PASS" : "FAIL");
    ok &= mbi.State == MEM_FREE;
    L("DRMKMS: %s", ok ? "ALL PASS" : "FAILED");
    CloseHandle(dev);
    return ok ? 0 : 4;
}
