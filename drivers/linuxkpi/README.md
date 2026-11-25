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
What is a stub: sysfs/debugfs, runtime PM, ACPI, hwmon, backlight, i2c-algo-bit, perf, IOMMU, MSI/MSI-X (vector 0 = INTx only),
dma-buf import/export, shmem, cache attributes (WC/UC) for system pages. (Later milestones replaced several of these:
MSI/MSI-X, dma-buf/sync files/shmem, i2c; see below.)

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

Still missing for user-mode graphics: a `libdrm` shim (`drmOpen`, `drmIoctl`, `drmMap`, ...) and a Mesa winsys for it.
(Bad user pointers in ioctl arguments are now caught: `nt_copy_user` returns `-EFAULT`.)

## amdgpu with the display core (DC)
`CONFIG_DRM_AMD_DC=y` (with `DRM_AMD_DC_FP`): amdgpu can now drive monitors itself (DCN 3.2 on the RX 7900 XT),
instead of leaving the screen on the firmware framebuffer. amdgpu.ko grows from 8 to 16 MB (790 more objects); all its
imports resolve against our modules after four additions (`debugfs_create_file_unsafe`, `dmi_first_match`,
`ktime_get_raw`, `system_highpri_wq`). In QEMU it loads and binds no device, as before; it has not run on an AMD GPU.

What DC needed from the host:
* **Floating point in kernel mode.** DC computes bandwidth and clocks in double precision (SSE, no x87: checked in
  the objects) between `kernel_fpu_begin()`/`kernel_fpu_end()`. These were empty; the host now raises to
  DISPATCH_LEVEL (no thread switch, as Linux disables preemption), loads the default MXCSR, counts nesting per CPU
  and restores everything at the end (`nt_fpu_begin/end` in `shim.c`). DC's own per-CPU nesting counter would be one
  counter for all CPUs in our uniprocessor build, so the tree patch makes `dc_fpu_begin()` defer to the host.
* **16-byte stack alignment for FP code.** The kernel builds FP files with `-mpreferred-stack-boundary=3`; called
  into NT code with an 8-aligned stack, any aligned SSE store there faults. The tree patch keeps 16 for FP code.
* **i2c transfers.** DC reads monitor EDIDs over i2c (HDMI DDC, and DisplayPort AUX through DRM's i2c adapter);
  `i2c_transfer()` returned -ENODEV. It now runs the adapter's `master_xfer` with bus locking and -EAGAIN retries
  (`kpi2_amd.c`; tests in `kpi/test/kpi2test.c`). Bit-banged adapters (i2c-algo-bit) are still not implemented.
* **Firmware**: `amdgpu/dcn_3_2_0_dmcub.bin` (display microcontroller) is fetched and checksummed with the rest.

Kernel bug found on the way (upstream ReactOS): **MXCSR was not kept per thread.** The context switch saved no
floating-point state for system threads, so a kernel thread could resume with another thread's rounding mode,
flush-to-zero or unmasked exceptions (the x64 ABI makes MXCSR's control bits non-volatile). `KiSwapContext` now saves
and restores it in the exception frame it already builds, and new threads start with the kernel default.

`kpi/test/fputest.c` checks all of this: three threads run 1200 FP sections of 2 ms (long enough for clock and
device interrupts to land inside), each holding patterns in all 16 XMM registers, computing a double-precision result
and setting an unusual MXCSR outside the section. QEMU, 4 vCPUs: 0 register, 0 result and 0 MXCSR mismatches, three
boots in a row. Before the context-switch fix the same test found 4 MXCSR mismatches.

## libdrm and RADV on top of the DRM nodes
`tools/vulkan/libdrm-ros` is libdrm for ReactOS, and `tools/vulkan/build-radv-x64.sh` builds Mesa's RADV with its real
amdgpu winsys on it (see `tools/vulkan/README.md`). What the host needed for that:
* DRM nodes are created as `FILE_DEVICE_SCREEN`, which `GetFileType()` reports as a character device: the C runtime's
  `open()` refuses `FILE_TYPE_UNKNOWN`, and Mesa opens render nodes with `open()`.
* `IOCTL_LKPI_DRM_INFO` (`lkpi_drm.h`): which device a handle is open on (its primary and render minors, the Linux
  driver's name, PCI location and ids). libdrm's device enumeration reads sysfs on Linux; here it opens each node and
  asks.
* PCI buses now have `sysdata` (segment 0): x86 `pci_domain_nr()` reads it and crashed on NULL.

## Milestone: DRM buffers mapped into user mode (`IOCTL_LKPI_DRM_MMAP`)
`DeviceIoControl(IOCTL_LKPI_DRM_MMAP, {Offset, Size})` maps the buffer behind a fake mmap offset (from
`DRM_IOCTL_MODE_MAP_DUMB`, `DRM_IOCTL_AMDGPU_GEM_MMAP`, ...) into the calling process; `IOCTL_LKPI_DRM_MUNMAP`
(Address), closing the handle or the process exiting removes it. There are no Linux page tables, so `kpi2_drm.c` runs
the driver's real `f_op->mmap` on a vm_area_struct that is never installed and calls its `vm_ops->fault` for every page,
recording the frames and caching (PAT bits) the driver passes to `vmf_insert_pfn_prot()`. The host (`drmdev.c`) builds
an MDL from those frames and maps it with `MmMapLockedPagesSpecifyCache(UserMode)`: placement, caching
(write-combined for VRAM) and access checks stay the driver's decisions.

Buffers move (a dumb buffer goes to VRAM when it is scanned out; amdgpu evicts to system memory). TTM announces a move
with `unmap_mapping_range()`; that queues a refresh which faults the pages again after the move and repoints the user
PTEs in place with a new kernel call, `MmReplaceLockedPagesInUserSpace()` (`ntoskrnl/mm/ARM3/mdlsup.c`: each PTE goes
from valid to valid in one 64-bit store, then a TLB flush), so the address a program holds stays valid. System-memory
pages a process still maps are not handed out again until the mapping lets go (`kpi_page_umap_get/put`), so a write
that races with a move cannot land in someone else's page. A process-exit notify routine removes mappings whose
process exits while another process still holds the handle.

`tools/vulkan/drmkms.c` (QEMU, bochs): create a 1280x800 dumb buffer, map it (4,096,000 bytes), draw and read back with 0 mismatches,
set the mode on it (the buffer moves to VRAM), draw a checkerboard through the same pointer and read it back with 0
mismatches; a QEMU screendump of the display shows the checkerboard. Unmap leaves the range `MEM_FREE`; exiting with
the buffer mapped, also while a child process holds the handle, leaves no trace and a second run passes.

Kernel bugs fixed on the way (upstream ReactOS): `MiIsPteOnPdeBoundary(PointerPte + 1)` expanded to
`(ULONG_PTR)PointerPte + 1` (one byte, not one PTE), so unmapping any user MDL mapping that owned a whole page table
asserted; `VirtualQuery` asserted on device-memory VADs (`VadDevicePhysicalMemory`).

## Milestone: the ReactOS desktop on a Linux KMS driver
With the GPU group loaded, the desktop is drawn by the Linux display driver instead of the firmware framebuffer: the
Linux driver sets the monitor's preferred mode on its own scanout buffer, and win32k's generic `framebuf.dll` draws
into that buffer. Pieces:
* `kpi/kpi2_drm.c`: an in-kernel DRM client, as Linux's fbdev emulation uses (`drm_client_init`,
  `drm_client_modeset_probe`, `drm_client_framebuffer_create`, XRGB8888). The mode is set before the buffer is mapped
  (`drm_client_buffer_vmap` pins it), so a buffer of a VRAM-helper or TTM driver is pinned in VRAM where it is scanned
  out, not in system memory. Its hotplug callback sets the mode again after a monitor change, its restore callback
  after a program that took the display over (`drmkms.exe`, a Vulkan program with direct display) closes its fd. The
  first minor with a connected connector wins.
* `display.c`: `\Device\LkpiDisplay`. Its one internal ioctl waits (up to 120 s) for the GPU group, which loads late,
  then starts the client and returns the buffer's address, size, pitch and the mode.
* `win32ss/drivers/miniport/lkpi` (`lkpivmp.sys`): the video miniport. The GPU's PCI device belongs to the Linux driver,
  so this is a root-enumerated PnP miniport (`Root\LKPI_DISPLAY`): legacy detection needs firmware bus descriptions that
  UEFI machines do not have. Without `/LINUXKPI_GPU` it declines (`LkpiVmp: no \Device\LkpiDisplay`), and the
  desktop uses the firmware framebuffer as before.
* win32k (`eng/device.c`, `mdevobj.c`): with several display devices and no multi-monitor support, win32k enabled all
  of them and fell back to the first; it now prefers the LinuxKPI display and enables only that one.
* bochs (QEMU only): the tree patch sets `VBE_DISPI_NOCLEARMEM`, because QEMU's VBE clears video memory from offset 0
  on every mode set and wiped the desktop when another program set a mode.

Verified in QEMU (bochs-drm): `desktop on minor 0 (Virtual-1): 1280x800@75, pitch 5120, device memory`, the desktop
draws normally, and after `drmkms.exe` takes card 0 over and exits, the desktop comes back intact (the buffer stays
pinned at VRAM page 0). Not verified with amdgpu.

## Milestone: sharing GPU buffers and fences (dma-buf, sync files, syncobjs)
Linux drivers share buffers and fences as fds: a PRIME export gives a dma-buf fd, a fence becomes a sync file fd, a
syncobj can be exported as an fd. Vulkan uses them for swapchains, external memory and semaphores, and between
processes (a compositor, a game launcher's overlay, DXVK/vkd3d's shared resources). They now work as on Linux:
* `kpi/kpi2_file.c`: the VFS pieces Linux's own `drivers/dma-buf/dma-buf.c` and `sync_file.c` need, which are now
  compiled into kpi2 unchanged: anon inodes and pseudo mounts, `struct file` with reference counts (the last `fput()`
  in atomic context is deferred to a worker, as Linux does), shmem files for GEM objects (zeroed pages in the mapping's
  xarray), and the fd calls. Plus `kpi_file_poll()`, which runs a file's `poll` with a real poll table.
* fds are NT handles in the calling process: `get_unused_fd_flags()` opens `\Device\LkpiFile` (not a kernel handle,
  so it belongs to the process that made the ioctl), `fd_install()` puts the file into that file object, `fget()`
  looks a handle up (user-mode access checks; anything not a LinuxKPI file is `EBADF`). Closing the last handle drops
  the file. So handle inheritance and `DuplicateHandle()` share a dma-buf between processes, with the same lifetime
  rules as Linux fds.
* `drmdev.c`: on such a handle, `IOCTL_LKPI_DRM` reaches the file's `unlocked_ioctl` (dma-buf and sync file ioctls),
  `IOCTL_LKPI_DRM_MMAP` maps a dma-buf (the same fault-recording path as DRM buffers), `IOCTL_LKPI_FILE_POLL` waits for
  it (in 100 ms slices, so a process being terminated is not held).
* libdrm-ros turns the handles into C runtime fds and back in every ioctl that carries one (PRIME, syncobj fds, sync
  file export/import on a dma-buf, `SYNC_IOC_MERGE`, amdgpu fence to sync file), and `poll()` is real.

`tools/vulkan/sharetest.c` checks it with vgem, Linux's virtual GEM driver (built as a test module only, with syncobjs
enabled in the tree patch; it is not part of the boot image: append
`drm_shmem_helper.ko;vgem/vgem.ko` to `LKPI_GPU_MODULES` to test): export, mmap of the dma-buf, import on another fd (same
pages), self-import, import after every GEM handle is gone, `dup()`, errors; a child process that inherits the handle,
imports it and rewrites the pixels; sync files (poll timeout, wake-up from another thread, merge, poll of the dma-buf
itself, import into another dma-buf); syncobjs (sync file in, fd to another DRM fd, sync file out, wait); 2000
export/import/close rounds without a handle leak. `SHARETEST: ALL PASS` in QEMU; counting live Linux files showed the
same number before and after two full runs, so dma-bufs, sync files and GEM objects are freed. platform devices now
get their Linux names (vgem's `drm_dev_init()` copies it and failed with ENOMEM without one).

## Milestone: Linux USB stack (xHCI + usbcore + HID) running

Unmodified Linux 6.12 `usb-common`, `usbcore`, `xhci-hcd`, `xhci-pci`, `hid`, `usbhid`, `hid-generic` load on
top of `kpi`+`kpi2` (which now contains `kpi3_drv.c`: a real driver core with bus/driver matching and
probing, notifier chains, kfifo/genalloc, dma pools, a minimal input core, many sysfs/PM/ACPI stubs).
Measured in QEMU q35 (`-device qemu-xhci`): controller probe, both root hubs (HS + SS) enumerate, a hot-plugged
`usb-kbd` is addressed, its descriptors read, `usbhid` binds and key presses arrive as `input_event()`s.

Bugs found on the way (each would also break real hardware):
* BH work (URB completions) shared the single `events` worker with `hub_init_func2` and deadlocked: BH queues now
  have their own workers.
* `spin_lock_irqsave` did not mask interrupts: the xHCI ISR spun forever on the CPU it interrupted. The `_irq`
  variants now `cli`/`sti`.
* `dma_map_*` of `kmalloc` memory used direct-map arithmetic, but the arena is outside the direct map; it now
  resolves the physical address through the memory manager (`kpi_page_phys`). Limitation: single-page buffers.

Debug aids added: `LkpiSymbolize()` (address -> `name+off` for loaded modules), `nt_log_addr`, workqueue
tracing (`kpi_wq_trace`), stuck-wait tracer in `wait_for_completion`/`mutex_lock`.

Input bridge (verified in QEMU, no PS/2 path): Linux `input_event()` -> `nt_input_event()` ring buffer ->
system thread (`input.c`) -> private IOCTLs `IOCTL_LKPI_KEYBOARD_INJECT` / `IOCTL_LKPI_MOUSE_INJECT`
(`sdk/include/reactos/drivers/lkpiinput.h`, handled in `kbdclass`/`mouclass`) -> win32k. Typing `echo usb works`
on a USB-only keyboard worked; the USB mouse moves the pointer. KEY_* codes are translated to PS/2 set-1 make
codes (E0 prefix for the extended block).

Not done yet: mass storage (usb-storage + a SCSI/disk bridge), hub hotplug
removal, MSI (legacy INTx only), USB suspend.

## Milestone: Linux NVMe driver reads a disk

Unmodified `nvme-core.ko` + `nvme.ko` (Linux 6.12, multipath/hwmon/auth/fabrics off) load on `kpi`+`kpi2`.
`kpi/kpi4_blk.c` is a minimal blk-mq: tag sets with preallocated requests, one hctx per hardware queue,
request alloc/execute/complete, kernel-buffer mapping into bios (one bvec per page), `__blk_rq_map_sg`, gendisk
bookkeeping, mempool, srcu/async/once/param stubs, and legacy-INTx `pci_alloc_irq_vectors` / `pci_request_irq`.
`kpi_blk_rw(disk, write, byte_off, buf, len)` is the synchronous entry point for the NT side.

Measured in QEMU (`-device nvme`): controller identify, I/O queue creation, namespace scan, disk
`nvme0n1` registered (131072 sectors, lbs 512) and a self-test read of sector 0 returned the expected
bytes through the full path (queue_rq -> PRP setup -> DMA -> completion IRQ -> blk_mq_end_request).

Limits: one I/O queue (INTx only, `nr_cpu_ids` small), no request timeouts/error recovery, no flush/discard
mapping tested, writes untested, no NT-side disk object yet (next: SCSI-miniport bridge so disk.sys/partmgr/FAT/NTFS see it).

## Milestone: Linux NVMe disk is a normal ReactOS disk

`lkpidisk.c` makes `linuxkpi.sys` a small SCSI port driver: a legacy-detected bus FDO whose BusRelations lists one
disk PDO per Linux gendisk (`SCSI\Disk&Ven_Linux&Prod_NVMe_disk_N`, hardware ID `GenDisk`). ReactOS' own `disk.sys`,
`partmgr` and `fastfat` attach and talk SRBs to it (INQUIRY, READ CAPACITY 10/16, READ/WRITE 10/12/16, SYNC CACHE,
MODE SENSE, claim/release; adapter/device properties, SCSI address, capabilities). Reads/writes go to a worker
thread -> `kpi_blk_submit()` (Linux, may sleep); the Linux completion arrives in the NVMe ISR and only queues a DPC
that completes the IRP. SRB buffers are mapped through the MDL (the DataBuffer VA is not a system address).

Verified in QEMU (`-device nvme`, MBR + FAT32 image): the volume appears as a drive letter, `type C:\HELLO.TXT` prints the
file, `echo > C:\OUT.TXT` / `copy` create files, and the data is present in the raw image on the host afterwards.

Bugs fixed on the way: MmGetPhysicalAddress on unmapped addresses bugchecks while dumping its own stack
(`nt_virt_to_phys` now validates first); `kpi_page_phys` reconstructs VAs below the direct map correctly.

Limits: not yet a boot/system disk (the driver is loaded by `sc start`, so Setup/boot cannot use it yet), no hot-remove,
no MSI-X, 128 KiB max transfer, a single LUN per disk, disk name always "NVMe disk N".

## Boot-time loading and reproducible module build
* `kpi/build-all.sh <workdir>` builds kpi.ko, kpi2.ko and the NVMe/USB/HID modules from a pristine linux-6.12 tarball
  (`linux-6.12-kpi.patch` + `linux-6.12.config`); output list for `-DLKPI_MODULES=...` in `out/modules.list`.
  Verified from scratch locally; the CI workflow `x64-live-iso.yml` runs it and caches the result.
* With modules on the CD the service is registered with Start=1 (`linuxkpi_reg_boot.inf`); boot option `/NOLINUXKPI`
  (menu entry "without Linux drivers") makes DriverEntry return immediately.

## Milestone: MSI / MSI-X
* HAL (`hal/halx86/apic/apic.c`): `HalGetInterruptVector()` for a synthetic bus level in [0xC0, 0xFE) allocates a free
  IDT vector without an I/O APIC pin; `HalEnableSystemInterrupt/HalDisableSystemInterrupt` do nothing for those.
* Host: `nt_msi_alloc()` returns the MSI message (0xFEE00000 | boot-CPU APIC id << 12, data = vector); `nt_irq_connect()`
  accepts irq numbers >= 0x1000 as MSI slots (edge, non-shared).
* `kpi/kpi5_msi.c`: real `pci_alloc_irq_vectors[_affinity]` (MSI-X table programmed through the BAR, or MSI capability),
  `pci_irq_vector`, `pci_request_irq`, `pci_free_irq[_vectors]`; INTx is disabled while MSI is on.
Verified in QEMU: NVMe (1 admin + 2 queue vectors) and xHCI use MSI-X; disk mount/write and USB keyboard still work.
Limits: all vectors target the boot CPU, 60 slots, no multi-message MSI, no per-vector masking from the driver.

## Milestone: USB mass storage (Linux usb-storage) as ReactOS disks
`kpi/kpi6_scsi.c` is a minimal SCSI mid-layer (hosts, LUN scan with INQUIRY, `scsi_cmnd`/scatterlist construction,
`scsi_eh_prep/restore_cmnd`, sense helpers, deferred `scsi_done`). There is no sd/sr: every direct-access `scsi_device`
is reported to `lkpidisk.c`, which exposes it as a disk PDO and **passes SRB CDBs straight through**
(`kpi_scsi_submit`), copying sense data back as autosense. Verified in QEMU (`-device usb-storage` on the xHCI): the
FAT32 stick appears as a drive letter and a file on it can be read.

Bugs found: `kpi.ko` and `kpi2.ko` each defined `page_offset_base`/`vmemmap_base` (only kpi2's copy was set, so
`sg_init_one()` in kpi.ko built garbage pages) -> one shared copy now; `usb-storage` calls `scsi_done()` before it clears its
in-flight command (completion is now deferred to a worker, and submit retries while the host is busy); a NULL
`system_freezable_wq`. `kpi/symres.py` maps a crash address to a Linux function (needs `-DLKPI_DEBUG_SECTIONS`).
Limits: direct-access devices only (no CD-ROM/optical, no UAS), 64 KiB max transfer, no hot-unplug, and USB enumeration is
asynchronous so a USB boot device may not be present by the time the boot partition is needed.

## Milestone: Linux Ethernet drivers under NDIS (e1000e, igb; r8169, igc built)
`kpi7_net.c` (in kpi2.ko) is a minimal network core: sk_buff (linear and page-fragment heads, frags, expand/pull/trim/
pad), `alloc_etherdev_mqs`/`register_netdev`, TX queues with BQL (`lib/dynamic_queue_limits.c`), unicast/multicast lists
(`net/core/dev_addr_lists.c`, unmodified), NAPI polls in host DPCs (`nt_dpc_alloc`/`nt_dpc_queue`, the NET_RX softirq's
counterpart), carrier changes reported with the ethtool link speed. Every `net_device` becomes a PDO on the LinuxKPI
bus (`LKPI\NET_<driver>`, `netbus.c`) offering `LKPI_NET_INTERFACE` (`lkpi_net.h`); `drivers/network/dd/lkpinet` is a
serialized NDIS 5 miniport for those PDOs, installed by `netlkpi.inf`. TX copies the frame into an skb; RX hands the
whole frame to `NdisMEthIndicateReceive`; a full TX ring returns `NDIS_STATUS_RESOURCES` until the queue wakes.

Verified in QEMU/OVMF (TCG): e1000e (82574) and igb (82576, 3 MSI-X vectors, page-fragment RX) get a DHCP lease, ping,
and move 64 MiB each way over TCP with no corrupted byte (`nettest`). Throughput under TCG: e1000e 10.8/14.9 Mbit/s
down/up, igb 12.0/17.9; ReactOS's own e1000 driver in the same VM: 13.6/30.9 (the extra per-frame copy shows on
upload). r8169 (with libphy, mdio_devres and the Realtek PHY driver) and igc load and register; no emulated hardware.

## The GPU group, firmware and the USB-stick log
* `-DLKPI_GPU_MODULES=` embeds a second module list that is loaded only with the boot option `/LINUXKPI_GPU`, from a
  worker thread, once `\SystemRoot\system32` is reachable (firmware lives there). Boot entry: "ReactOS Live
  (experimental AMD GPU driver, test only)". Verified with drm + bochs standing in for amdgpu.
* `-DLKPI_FIRMWARE_DIR=` copies a linux-firmware style tree to `reactos\system32\firmware` on the CD;
  `fetch-firmware.sh` fetches Navi 31 (amdgpu) and Realtek NIC firmware from a pinned linux-firmware tag and checks
  sha256 sums.
* The HAL now fills `HalGetCachedAcpiTable`; `acpi_get_table()` returns real tables (amdgpu reads the VBIOS from VFCT),
  `pci_map_rom()` maps the expansion ROM. Startup logs whether VFCT is present.
* `lkpilog.c`: all host and Linux output also goes to a 2 MiB RAM ring that is appended (write-through, flushed) to
  `X:\LKPILOG.TXT` on any volume where that empty file exists. Survives a hard power-off (checked with `fsck.fat`).

## Bugs found on the way (all pre-existing, all affect real hardware)
* **Atomics were not atomic.** The modules are built `CONFIG_SMP=n`, where x86 `LOCK_PREFIX` is empty, but run on all
  CPUs. The tree patch now keeps the `lock` prefix and a real `smp_mb()`.
* **kmem_cache constructors were ignored.** XArray/IDR/radix-tree nodes rely on `radix_tree_node_ctor()` zeroing them;
  recycled heap blocks gave nodes garbage slots and intermittent wild writes (found as a crash in DRM minor
  allocation). Constructors now run on every cache allocation.
* The heap now detects double frees and free-list corruption and names the Linux callers from the stack.
* PCI devices had no parent device (`pm_runtime_suspended(dev->parent)` in e1000e dereferenced NULL).
* **Only PCI buses 0-3 were searched for devices**, so on a real board most devices would never have been found (GPUs,
  NVMe and NICs often sit on buses 5-15 behind bridges). Driver binding also sized the BARs of every device in the
  system on each driver registration, briefly turning off decoding on devices that Windows drivers were using, and
  leaked a `pci_dev` per device each time. Now the whole topology is walked (all 256 buses, multifunction devices),
  only config-space ids are read until a driver matches, and a device gets one `pci_dev` for good. Verified in QEMU with
  devices behind PCIe root ports: NVMe on bus 3, xHCI on bus 4 and the e1000e on bus 5 all bind; nettest and winchk
  pass over that NIC.
