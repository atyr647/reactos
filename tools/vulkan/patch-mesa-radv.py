#!/usr/bin/env python3
"""
RADV with its real amdgpu winsys on ReactOS (idempotent). usage: patch-mesa-radv.py <mesa-src-dir>

Upstream builds RADV on Windows with the null winsys only (no kernel driver to talk to). On ReactOS the Linux amdgpu
driver runs in the kernel (LinuxKPI) and libdrm works (tools/vulkan/libdrm-ros), so with -Dwindows-libdrm=true:
  - meson looks for libdrm/libdrm_amdgpu on Windows and builds the amdgpu winsys and ac_linux_drm.c;
  - the "no DRM on Windows" guards in RADV and ac become "no DRM on Windows unless HAVE_LIBDRM" (only those: the
    dmesg/umr debugging, RMV tracing and calibrated timestamps stay off on Windows);
  - DRM nodes are \\\\.\\LKPIDRM<n> devices, not files: their Linux device numbers come from libdrm-ros instead of
    stat().
"""
import os
import re
import sys

root = sys.argv[1]


def read(rel):
    return open(os.path.join(root, rel)).read()


def write(rel, s):
    open(os.path.join(root, rel), 'w').write(s)


def replace(rel, old, new, count=1):
    s = read(rel)
    if new in s and (old not in s or old in new):
        return      # done already (when new contains old, old is always there: then new decides)
    assert s.count(old) == count, '%s: %d x %r' % (rel, s.count(old), old[:80])
    write(rel, s.replace(old, new))


GUARDS = {
    '#ifdef _WIN32': '#if defined(_WIN32) && !defined(HAVE_LIBDRM)',
    '#ifndef _WIN32': '#if !defined(_WIN32) || defined(HAVE_LIBDRM)',
    '#if !defined(_WIN32)': '#if !defined(_WIN32) || defined(HAVE_LIBDRM)',
}


def flip_guard(rel, anchor, occurrence=1):
    """Rewrite the nearest _WIN32 guard line above the occurrence-th match of anchor."""
    s = read(rel)
    pos = -1
    for _ in range(occurrence):
        pos = s.find(anchor, pos + 1)
        assert pos >= 0, '%s: anchor %r' % (rel, anchor[:60])
    lines_before = s[:pos].split('\n')
    for i in range(len(lines_before) - 1, -1, -1):
        line = lines_before[i].strip()
        if 'HAVE_LIBDRM' in line and '_WIN32' in line:
            return  # already done
        if line in GUARDS:
            lines_before[i] = lines_before[i].replace(line, GUARDS[line])
            write(rel, '\n'.join(lines_before) + s[pos:])
            return
        assert not line.startswith('#if'), '%s: unexpected %r above %r' % (rel, line, anchor[:60])
    raise AssertionError('%s: no guard above %r' % (rel, anchor[:60]))


# ---- meson -------------------------------------------------------------------------------------------------------
replace('meson.options',
        "option(\n  'amd-use-llvm',",
        "option(\n  'windows-libdrm',\n  type : 'boolean',\n  value : false,\n"
        "  description : 'Windows: use libdrm (ReactOS LinuxKPI DRM nodes) for the amdgpu winsys'\n)\n\n"
        "option(\n  'amd-use-llvm',")
replace('meson.build',
        "  ['amdgpu', (with_amd_vk and not with_platform_windows) or with_gallium_radeonsi],",
        "  ['amdgpu', (with_amd_vk and (not with_platform_windows or get_option('windows-libdrm'))) or with_gallium_radeonsi],")
replace('meson.build',
        "with_gallium_drisw_kms = false\nif system_has_kms_drm\n",
        "with_gallium_drisw_kms = false\nif system_has_kms_drm or (with_platform_windows and get_option('windows-libdrm'))\n")
replace('src/amd/vulkan/meson.build',
        "if not with_platform_windows\n  libradv_files += files(\n    'winsys/amdgpu/radv_amdgpu_bo.c',",
        "if not with_platform_windows or dep_libdrm_amdgpu.found()\n  libradv_files += files(\n    'winsys/amdgpu/radv_amdgpu_bo.c',")
replace('src/amd/common/meson.build',
        "if not with_platform_windows\n  amd_common_files += files(\n    'ac_linux_drm.c',",
        "if not with_platform_windows or dep_libdrm_amdgpu.found()\n  amd_common_files += files(\n    'ac_linux_drm.c',")
# Linux display/dma-buf WSI glue: Windows presents through wsi_common_win32
replace('src/vulkan/wsi/meson.build',
        "if dep_libdrm.found()\n  files_vulkan_wsi += files('wsi_common_drm.c')",
        "if dep_libdrm.found() and not with_platform_windows\n  files_vulkan_wsi += files('wsi_common_drm.c')")

# the Linux presentation glue in wsi_common.c (dma-buf images, sync files, DRM display matching) goes with
# wsi_common_drm.c, which is not built on Windows
replace('src/vulkan/wsi/wsi_common.c', '#ifdef HAVE_LIBDRM\n', '#if defined(HAVE_LIBDRM) && !defined(_WIN32)\n', 7)
replace('src/vulkan/wsi/wsi_common.c', '#if HAVE_LIBDRM\n', '#if defined(HAVE_LIBDRM) && !defined(_WIN32)\n', 2)

# ---- sources: DRM on Windows when libdrm is there ----------------------------------------------------------------
replace('src/util/os_drm.h', '#ifdef _WIN32\n#error "Windows shouldn\'t include this."',
        '#if defined(_WIN32) && !defined(HAVE_LIBDRM)\n#error "Windows shouldn\'t include this."')

flip_guard('src/amd/common/ac_gpu_info.c', 'static int drmGetCap(int fd, uint64_t capability, uint64_t *value)')
flip_guard('src/amd/common/ac_surface.c', '#define AMDGPU_TILING_ARRAY_MODE_SHIFT')
flip_guard('src/amd/common/ac_drm_fourcc.h', 'typedef uint64_t __u64;')
replace('src/amd/common/ac_linux_drm.h', '#if !defined(_WIN32)\n', '#if !defined(_WIN32) || defined(HAVE_LIBDRM)\n')
replace('src/amd/common/ac_linux_drm.h', '#ifdef _WIN32\n#define __U_STUB__',
        '#if defined(_WIN32) && !defined(HAVE_LIBDRM)\n#define __U_STUB__')
replace('src/amd/common/ac_linux_drm.h', '#ifdef _WIN32\n   void *abo;',
        '#if defined(_WIN32) && !defined(HAVE_LIBDRM)\n   void *abo;')

flip_guard('src/amd/vulkan/radv_device.c', '#include "winsys/amdgpu/radv_amdgpu_winsys_public.h"')
flip_guard('src/amd/vulkan/radv_device.c', '   return VK_ERROR_INCOMPATIBLE_DRIVER;\n#else\n   const struct radv_physical_device *pdev')
flip_guard('src/amd/vulkan/radv_physical_device.h', '#include <xf86drm.h>')
flip_guard('src/amd/vulkan/radv_physical_device.h', '   int available_nodes;')
P = 'src/amd/vulkan/radv_physical_device.c'
flip_guard(P, 'typedef void *drmDevicePtr;')
flip_guard(P, '      .EXT_physical_device_drm = true,')
flip_guard(P, '      /* VK_EXT_pci_bus_info */\n      .pciDomain')
flip_guard(P, '   if (pdev->available_nodes & (1 << DRM_NODE_PRIMARY)) {\n      p->drmHasPrimary = true;')
flip_guard(P, '   assert(drm_device == NULL);\n   return VK_ERROR_INCOMPATIBLE_DRIVER;')
flip_guard(P, '   bool supported_device = false;')
flip_guard(P, '   if (pdev->drm_device)\n      ac_drm_device_deinitialize(pdev->drm_device);')
flip_guard(P, 'struct radeon_winsys_heap_info {')

# DRM nodes are not files on ReactOS: no stat(), libdrm-ros knows their device numbers
replace(P, '#include "git_sha1.h"\n',
        '#include "git_sha1.h"\n\n'
        '#if defined(_WIN32) && defined(HAVE_LIBDRM)\n'
        '#include <xf86drm_ros.h>\n'
        '/* ReactOS: \\\\.\\LKPIDRM<n> nodes are devices, not files; libdrm-ros gives their Linux device numbers */\n'
        '#define radv_stat_node(path, st) drmRosGetNodeDevid((path), &(st)->st_rdev)\n'
        '#else\n'
        '#define radv_stat_node(path, st) stat((path), (st))\n'
        '#endif\n')
replace(P, '       stat(drm_device->nodes[DRM_NODE_PRIMARY], &primary_stat) != 0) {',
        '       radv_stat_node(drm_device->nodes[DRM_NODE_PRIMARY], &primary_stat) != 0) {')
replace(P, '       stat(drm_device->nodes[DRM_NODE_RENDER], &render_stat) != 0) {',
        '       radv_stat_node(drm_device->nodes[DRM_NODE_RENDER], &render_stat) != 0) {')
# addrlib uses _tzcnt_u32 whenever _M_X64 is defined; MSVC allows it anywhere, mingw-w64 GCC (which also defines
# _M_X64) needs -mbmi for it. GCC has __builtin_ctz on the next branch but one.
replace('src/amd/addrlib/src/core/addrcommon.h',
        '#if ((defined(_WIN64) && defined(_M_X64)) || (defined(_WIN32) && defined(_M_IX64))) && !defined(_M_ARM64EC)\n    out = ::_tzcnt_u32(mask);\n#elif (defined(_WIN32) || defined(_WIN64))\n    ::_BitScanForward(&out, mask);',
        '#if defined(_MSC_VER) && ((defined(_WIN64) && defined(_M_X64)) || (defined(_WIN32) && defined(_M_IX64))) && !defined(_M_ARM64EC)\n    out = ::_tzcnt_u32(mask);\n#elif defined(_MSC_VER) && (defined(_WIN32) || defined(_WIN64))\n    ::_BitScanForward(&out, mask);')
# VK_EXT_map_memory_placed maps buffers over address ranges the application reserved (MAP_FIXED); NT cannot put a
# device mapping over an existing reservation, so libdrm-ros refuses MAP_FIXED: do not offer the extension
replace(P, '      .EXT_map_memory_placed = true,', '      .EXT_map_memory_placed = !DETECT_OS_WINDOWS,')
replace(P, '      .memoryMapPlaced = true,', '      .memoryMapPlaced = !DETECT_OS_WINDOWS,')
replace(P, '      .memoryUnmapReserve = true,', '      .memoryUnmapReserve = !DETECT_OS_WINDOWS,')
print('patch-mesa-radv: done')
