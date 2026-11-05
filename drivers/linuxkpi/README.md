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

    make KCFLAGS="-mcmodel=small -fPIC -mpreferred-stack-boundary=4" ...   # no -fno-plt (breaks some inline asm)

and the tree needs these config/source changes (all build-time, no driver code touched):
* `CONFIG_JUMP_LABEL=n` (static keys use `asm goto` with address immediates, rejected under -fPIC)
* `arch/x86/include/asm/cpufeature.h`: `#define static_cpu_has(bit) boot_cpu_has(bit)`
* `arch/x86/include/asm/page_64.h`: `clear_page()` -> plain memset (no `alternative_call`)
* build with `KBUILD_MODPOST_WARN=1` (no vmlinux to check symbols against)

Linux keeps the stack 8-byte aligned (`-mpreferred-stack-boundary=3`); the host shim is compiled for 16-byte alignment
(it calls NT functions that use `movaps`), so modules are built with 16-byte alignment too.

Relocation types seen: PC32, PLT32, GOTPCREL(X), REX_GOTPCRELX, 64, 32 (all supported).

## Measured surface (Linux 6.12)
* amdgpu without the display core (`CONFIG_DRM_AMD_DC=n`) + drm + ttm + sched: 695 external symbols.
* drm + drm_kms_helper + ttm + drm_ttm_helper + drm_vram_helper + bochs: 392 external symbols; only 51 of
  those come from `lib/` (vsprintf, bitmap, rbtree, idr, xarray, sort, ...), the rest are scheduler, mm,
  vfs, driver core, dma-buf, pci. Those need real implementations on the NT side.

## Testing
`cmake -DLKPI_TEST_MODULE=/path/test.ko .`  puts the module on the CD as `lkpitest.ko`;
`sc start linuxkpi` loads it. Output goes to the kernel debug port (prefix `LKPI:` / `LINUX:`).

## Status
* `kpi/` builds `kpi.ko` against the Linux headers (`kpi/build.sh <linux tree>`): real Linux vsprintf/bitmap/rbtree/
  idr/xarray/sort/... from `lib/` plus `kpi_core.c` (spinlocks, rwlocks, mutex, completions, wait queues, kmalloc/
  slab, jiffies/delays, printk). It talks to NT only through the `nt_*` bridge in `shim.c`.
* The kernel config for this must be `CONFIG_SMP=n CONFIG_PREEMPT_NONE=y CONFIG_DEBUG_SPINLOCK=y CONFIG_TINY_RCU=y
  CONFIG_JUMP_LABEL=n` (UP per-cpu data, out-of-line spinlocks) in addition to the source tweaks above.
* Unresolved symbols no longer stop a module from loading: they are bound to a trap that logs
  `call to unimplemented Linux function '<name>'`, which gives the work list while running.
* Verified in QEMU (x64 UEFI): kpi.ko loads and initializes, a second module loads against it.
* DRM core + TTM + bochs still need 269 more functions (see `drm_need.txt` workflow in the notes): memory
  (`alloc_pages`, struct page / vmemmap), device model (`devres`, class, kobject), vfs/seq_file/debugfs,
  workqueues, kthreads, timers, PCI, RCU, dma-buf, fences.

## kpi2.ko (second core module, `kpi/build2.sh`)
Built the same way as kpi.ko (`kpi/build2.sh <linux tree> <outdir>`), loaded second (`LKPI_MODULES="kpi.ko;kpi2.ko;..."`).
It contains real Linux sources (`drivers/dma-buf/dma-fence*.c`, `dma-resv.c`) plus `kpi2_core.c` (workqueues, timers,
RCU/irq_work stand-ins, tracepoint stubs, allocator helpers) and `kpi2_dev.c` (devres, struct device lifetime, class,
debugfs/sysfs/seq_file stand-ins, ww_mutex, rwsem, SRCU, kthread workers, aperture/i2c/acpi stubs).
The host driver gained `nt_alloc_size` (allocations now carry a size header), `nt_thread_spawn`, `nt_ktime_ns`.

Measured for DRM + TTM + drm_kms_helper + vram_helper + bochs: 392 external symbols -> kpi 108 -> 284 -> kpi2 **69 left**:

* memory: `__alloc_pages_noprof`, `__free_pages`, `copy_page`, `rep_stos_alternative`, `vmap/vunmap`, `vmalloc_to_page`,
  `ioremap*`/`iounmap`/`memremap`, `set_pages_*_wc/uc/wb`, `pgprot_writecombine`, `vm_get_page_prot`, `vmf_insert_pfn_prot`,
  `boot_cpu_data`, folio helpers, shrinkers
* DMA API: `dma_alloc_attrs/free_attrs`, `dma_map_page_attrs`, `dma_map_sgtable`, `dma_unmap_*`, `dma_max_mapping_size`
* PCI: `__pci_register_driver`, `pci_unregister_driver`, `pcim_enable_device`, `pci_request_region`, `pci_release_regions`
* dma-buf (`dma_buf_*`), `sync_file_*`, `eventfd_*`, `hdmi_*` infoframes (real `drivers/video/hdmi.c` can be compiled in), agp stubs

Design for the memory layer (not written yet): pages come from 2 MiB physically contiguous chunks obtained from NT;
`struct page` array lives in a reserved virtual `vmemmap_base` region (committed per chunk); the tree's `__va()` is
redirected to a chunk-table lookup (phys >> 21 -> kernel VA) instead of `page_offset_base + phys`, and `__pa()` to
`MmGetPhysicalAddress`.

## Milestone: a real Linux DRM driver runs on ReactOS x64 (QEMU)
Unmodified Linux 6.12 `drm.ko`, `drm_kms_helper.ko`, `ttm.ko`, `drm_ttm_helper.ko`, `drm_vram_helper.ko` and `bochs.ko`
load in the order above (`LKPI_MODULES="kpi.ko;kpi2.ko;drm.ko;drm_kms_helper.ko;ttm/ttm.ko;drm_ttm_helper.ko;
drm_vram_helper.ko;tiny/bochs.ko"`, QEMU with `-vga std -device bochs-display`). The host's PCI layer finds both
`1234:1111` devices and the driver reports:

    [drm] Found bochs VGA, ID 0xb0c5.
    [drm] Framebuffer size 16384 kB @ 0x80000000, mmio @ 0x820c7000.
    [drm] Found EDID data blob.
    [drm] Initialized bochs-drm 1.0.0 for 0000:00:01.0 on minor 0

Things that had to be right (each was a crash on the way): 16-byte stack alignment for module code, lazy re-binding of
symbols across modules, a DISPATCH_LEVEL-safe allocator (NonPagedPool cannot grow there), `radix_tree_init()` for the
xarray node cache, an in-memory "pseudo filesystem"/anon inode for DRM, Linux's own `printk`.

### Still to do toward amdgpu
* interrupts (`request_irq` -> `IoConnectInterrupt`), MSI/MSI-X, PCI resource claiming, runtime PM
* firmware loading (`request_firmware` from the CD/disk), ACPI/BIOS access, `/dev/dri` equivalent so a user-mode
  stack can talk to the driver (a `\Device\Drm0` with the DRM ioctls, then a libdrm shim for Mesa/radv)
* real shmem/dma-buf, WC/UC cache attributes for system pages, larger/lazy system-memory region
* amdgpu itself: ~700 symbols (measured earlier), `drm_sched`, `drm_exec`, `drm_buddy`, display core or none

### amdgpu measurement (Linux 6.12, `CONFIG_DRM_AMDGPU=m`, DC/KFD/CIK/userptr off)
`amdgpu.ko` (8 MB) + gpu-sched + drm_exec + drm_buddy + drm_suballoc_helper link against our modules except for
**225 symbols** (list in `amdgpu_need.txt`; the 19 `nt_*` entries of the full list are host bridge functions):
PCI (~37: MSI/MSI-X vectors, save/restore state, ROM, ReBAR, PCIe capabilities, power states), sysfs/kobject (~25),
`drm_dp_*` helpers (build `drm_display_helper.ko`), IRQ (`request_threaded_irq`, enable/disable, irq_domain),
pm_runtime (10), ACPI (10), debugfs helpers, hrtimers, firmware (`request_firmware`), iommu, dma-buf dynamic attach,
perf pmu, kthreads, hwmon, backlight, i2c. Each is either a stub or a small host bridge; interrupts and firmware are
the two that need real NT integration.

## Status: the full amdgpu module chain loads (QEMU)
Boot log with `LKPI_MODULES = kpi.ko; kpi2.ko; drm.ko; drm_kms_helper.ko; ttm.ko; drm_ttm_helper.ko; drm_vram_helper.ko;
drm_display_helper.ko; gpu-sched.ko; drm_exec.ko; drm_buddy.ko; drm_suballoc_helper.ko; amdxcp.ko; amdgpu.ko`:

    [drm] amdgpu kernel modesetting enabled.
    kpi2_pci: amdgpu bound 0 device(s)         (QEMU has no AMD GPU)

Every external symbol of these 14 modules is provided (measured with `nm`; see `amdgpu_need.txt` for the history).
What is real: memory/page layer, PCI config + BAR sizing + probe, interrupt connect (legacy line via the HAL), firmware
file loading (`\\SystemRoot\\system32\\firmware\\<name>`, name with `/` -> `\\`), workqueues, timers, hrtimers, kthreads.
What is a stub: sysfs/debugfs, runtime PM, ACPI, hwmon, backlight, i2c, perf, IOMMU, MSI/MSI-X (vector 0 = INTx only),
dma-buf import/export, shmem, cache attributes (WC/UC) for system pages.

### Trying it on real hardware (untested, expect failures)
1. Put the GPU firmware on the CD/disk: from linux-firmware copy `amdgpu/*.bin` for your chip (Navi 31 / 7900 XT:
   `amdgpu/gc_11_0_0_*.bin`, `psp_13_0_0_*.bin`, `smu_13_0_0.bin`, `sdma_6_0_0.bin`, `vcn_4_0_0.bin`, ...) into
   `reactos\\system32\\firmware\\amdgpu\\` (the files are not shipped here: they are AMD's binary blobs).
2. Build with the module list above, boot, `sc start linuxkpi`, and read the debug port. The first things that will
   fail: INTx not routed (needs MSI support in the HAL), firmware names, ACPI/BIOS (VBIOS fetch), `pci_resize_resource`.
3. Even if the driver initializes the card there is no user-mode path yet: the ioctl device and libdrm shim for Mesa
   radv (see WDDM-BRIDGE.md, "C. One kernel ABI, two backends").

## Milestone: user mode talks to a Linux DRM driver through `\\.\LKPIDRM0`
`drmdev.c` creates `\Device\LKPIDRM0` (primary node, minor 0) and `\Device\LKPIDRM128` (render node) with DOS links
`\\.\LKPIDRM0` / `\\.\LKPIDRM128`. `CreateFile` -> `drm_open()`, `DeviceIoControl(IOCTL_LKPI_DRM)` (envelope in
`lkpi_drm.h`: Linux ioctl number + argument bytes in, result + argument bytes out) -> the driver's `unlocked_ioctl`,
`CloseHandle` -> `drm_release()`. The Linux side (`kpi/kpi2_drm.c`) builds the `struct inode`/`struct file` that
`drm_open()` expects; arguments are kernel copies, nested user pointers (e.g. `drm_version.name`) are written directly
because the IRP runs in the caller's context (no fault recovery yet: a bad user pointer crashes the kernel).

`tools/vulkan/drmtest.c` (QEMU, bochs driver) gets:

    DRM_IOCTL_VERSION    -> 1.0.0 'bochs-drm' 'bochs dispi vga interface (qemu stdvga)'
    GET_CAP(DUMB_BUFFER) -> 1
    MODE_GETRESOURCES    -> 1 crtc, 1 connector, 1 encoder, 8192x8192
    GETCONNECTOR 32      -> 15 modes, connected
    CREATE_DUMB 800x600x32 -> handle 1, pitch 3200, size 1921024
    MAP_DUMB             -> offset 0x100000000

Still missing for user-mode graphics: `mmap` of GEM objects into the caller (map the BO's pages/VRAM with an MDL in
`IRP_MJ_DEVICE_CONTROL`, or a dedicated `IOCTL_LKPI_DRM_MMAP`), fault recovery for user pointers (`__put_user`/
`copy_to_user` exception fixups), a `libdrm` shim (`drmOpen`, `drmIoctl`, `drmMap`, ...) and a Mesa winsys for it.
