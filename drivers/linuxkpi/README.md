# LinuxKPI for ReactOS (work in progress)

Goal: run Linux's own GPU drivers (first `bochs` in QEMU, then `amdgpu`) on ReactOS x64 by loading
**unmodified Linux kernel modules** and providing the Linux kernel API underneath them.

## Design
* Modules are Linux x86_64 `ET_REL` objects (`.ko`), LP64 and System V ABI. They keep that ABI.
* `elfldr.c` lays out the sections, resolves symbols (host exports + globals of already loaded modules),
  builds a GOT slot + PLT stub per symbol and applies the relocations.
* `shim.c` is the Linux API. Functions use `__attribute__((sysv_abi))` and 64-bit `long` types, so the
  module calls them directly (no thunks) and they can call NT services directly.

## Building modules for this loader
Linux builds modules with `-mcmodel=kernel`, which needs them in the top 2 GB of the address space.
NT's kernel addresses are not there, so modules must be position independent:

    make KCFLAGS="-mcmodel=small -fPIC" ...        # no -fno-plt (breaks some inline asm)

and the tree needs these config/source changes (all build-time, no driver code touched):
* `CONFIG_JUMP_LABEL=n` (static keys use `asm goto` with address immediates, rejected under -fPIC)
* `arch/x86/include/asm/cpufeature.h`: `#define static_cpu_has(bit) boot_cpu_has(bit)`
* `arch/x86/include/asm/page_64.h`: `clear_page()` -> plain memset (no `alternative_call`)
* build with `KBUILD_MODPOST_WARN=1` (no vmlinux to check symbols against)

Relocation types seen: PC32, PLT32, GOTPCREL(X), REX_GOTPCRELX, 64, 32 (all supported).

## Measured surface (Linux 6.12)
* amdgpu without the display core (`CONFIG_DRM_AMD_DC=n`) + drm + ttm + sched: 695 external symbols.
* drm + drm_kms_helper + ttm + drm_ttm_helper + drm_vram_helper + bochs: 392 external symbols; only 51 of
  those come from `lib/` (vsprintf, bitmap, rbtree, idr, xarray, sort, ...), the rest are scheduler, mm,
  vfs, driver core, dma-buf, pci. Those need real implementations on the NT side.

## Testing
`cmake -DLKPI_TEST_MODULE=/path/test.ko .`  puts the module on the CD as `lkpitest.ko`;
`sc start linuxkpi` loads it. Output goes to the kernel debug port (prefix `LKPI:` / `LINUX:`).
