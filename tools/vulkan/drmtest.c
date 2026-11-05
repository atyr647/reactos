/* Talks to the Linux DRM driver through \\.\LKPIDRM0 (LinuxKPI host): version, caps, mode resources, connectors. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include "../drivers/linuxkpi/lkpi_drm.h"

static void L(const char *fmt, ...) { char b[400]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b - 2, fmt, a); va_end(a); strcat(b, "\n"); OutputDebugStringA(b); }

static HANDLE dev;
static long drm_ioctl(unsigned long cmd, void *arg, unsigned size)
{
    unsigned char buf[sizeof(LKPI_DRM_IOCTL) + 512];
    LKPI_DRM_IOCTL *io = (LKPI_DRM_IOCTL *)buf;
    DWORD ret = 0;

    io->Cmd = cmd; io->Size = size; io->Result = 0;
    memcpy(io->Data, arg, size);
    if (!DeviceIoControl(dev, IOCTL_LKPI_DRM, io, 24 + size, io, 24 + size, &ret, NULL)) { L("DRMTEST: DeviceIoControl failed %lu", GetLastError()); return -1000; }
    memcpy(arg, io->Data, size);
    return io->Result;
}

struct drm_version { int major, minor, patch; int pad; uint64_t name_len; uint64_t name; uint64_t date_len; uint64_t date; uint64_t desc_len; uint64_t desc; };
struct drm_get_cap { uint64_t capability, value; };
struct drm_card_res { uint64_t fb_ids, crtc_ids, conn_ids, enc_ids; uint32_t n_fbs, n_crtcs, n_conns, n_encs; uint32_t min_w, max_w, min_h, max_h; };
struct drm_get_conn { uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr; uint32_t count_modes, count_props, count_encoders, encoder_id, connector_id, connector_type, connector_type_id, connection, mm_width, mm_height, subpixel, pad; };
struct drm_mode_modeinfo { uint32_t clock; uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew, vdisplay, vsync_start, vsync_end, vtotal, vscan; uint32_t vrefresh, flags, type; char name[32]; };

int main(void)
{
    char name[64] = { 0 }, desc[128] = { 0 };
    struct drm_version v = { 0 };
    struct drm_get_cap cap = { 1, 0 };
    struct drm_card_res res = { 0 };
    uint32_t crtcs[8] = { 0 }, conns[8] = { 0 }, encs[8] = { 0 }, fbs[8] = { 0 };
    long r;
    unsigned i;

    L("DRMTEST: start");
    dev = CreateFileA("\\\\.\\LKPIDRM0", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    L("DRMTEST: open LKPIDRM0 -> %p (err %lu)", dev, GetLastError());
    if (dev == INVALID_HANDLE_VALUE) return 1;

    v.name_len = sizeof name; v.name = (uint64_t)(uintptr_t)name; v.desc_len = sizeof desc; v.desc = (uint64_t)(uintptr_t)desc;
    r = drm_ioctl(0xC0406400, &v, sizeof v);
    L("DRMTEST: DRM_IOCTL_VERSION -> %ld: %d.%d.%d name '%s' desc '%s'", r, v.major, v.minor, v.patch, name, desc);

    r = drm_ioctl(0xC010640C, &cap, sizeof cap);
    L("DRMTEST: GET_CAP(DUMB_BUFFER) -> %ld value %llu", r, (unsigned long long)cap.value);

    res.crtc_ids = (uint64_t)(uintptr_t)crtcs; res.conn_ids = (uint64_t)(uintptr_t)conns; res.enc_ids = (uint64_t)(uintptr_t)encs; res.fb_ids = (uint64_t)(uintptr_t)fbs;
    r = drm_ioctl(0xC04064A0, &res, sizeof res);           /* first call: counts only */
    L("DRMTEST: MODE_GETRESOURCES -> %ld crtcs %u connectors %u encoders %u fbs %u size %u..%ux%u..%u", r, res.n_crtcs, res.n_conns, res.n_encs, res.n_fbs, res.min_w, res.max_w, res.min_h, res.max_h);
    if (res.n_crtcs > 8) res.n_crtcs = 8;
    if (res.n_conns > 8) res.n_conns = 8;
    r = drm_ioctl(0xC04064A0, &res, sizeof res);           /* second call: ids */
    for (i = 0; i < res.n_conns; i++)
    {
        struct drm_get_conn c = { 0 };
        struct drm_mode_modeinfo modes[8];
        c.connector_id = conns[i];
        c.count_modes = 0;
        r = drm_ioctl(0xC05064A7, &c, sizeof c);          /* GETCONNECTOR: counts */
        L("DRMTEST: connector %u -> %ld: modes %u type %u connection %u %ux%u mm", conns[i], r, c.count_modes, c.connector_type, c.connection, c.mm_width, c.mm_height);
        if (c.count_modes && c.count_modes <= 8)
        {
            c.modes_ptr = (uint64_t)(uintptr_t)modes;
            r = drm_ioctl(0xC05064A7, &c, sizeof c);
            L("DRMTEST: first mode %s %ux%u @%u", modes[0].name, modes[0].hdisplay, modes[0].vdisplay, modes[0].vrefresh);
        }
    }
    {
        struct { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; } cd = { 600, 800, 32, 0, 0, 0, 0 };
        struct { uint32_t handle, pad; uint64_t offset; } md = { 0 };
        r = drm_ioctl(0xC02064B2, &cd, sizeof cd);
        L("DRMTEST: CREATE_DUMB 800x600x32 -> %ld handle %u pitch %u size %llu", r, cd.handle, cd.pitch, (unsigned long long)cd.size);
        md.handle = cd.handle;
        r = drm_ioctl(0xC01064B3, &md, sizeof md);
        L("DRMTEST: MAP_DUMB -> %ld offset %llx", r, (unsigned long long)md.offset);
    }
    L("DRMTEST: DONE");
    CloseHandle(dev);
    return 0;
}
