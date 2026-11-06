# Which Linux drivers can we adopt through LinuxKPI? (measured gap survey)

Method: build each Linux 6.12 driver as an unmodified module (`-fPIC -mcmodel=small`), take its undefined
symbols, remove what the module set defines itself and what `kpi.ko`+`kpi2.ko` already provide.
Script: `kpi/survey.py`; full missing lists: `kpi/survey_missing.txt`.

| Driver set | Needs (externals) | Still missing | What the missing part is | Needed to be useful on ReactOS |
|---|---|---|---|---|
| xHCI (`xhci-hcd`, `xhci-pci`) | 166 | 59 | `usb_hcd_*`/usbcore + DMA pools | usbcore (+ HID/usbhid + input) built as modules, then a bridge feeding ReactOS's kbd/mouse/mass-storage stacks |
| NVMe (`nvme-core`, `nvme`) | 242 | 109 | ~55 blk-mq/gendisk symbols + PCI/DMA/irq | minimal blk-mq shim, bridge to a storport-style miniport so ReactOS sees a disk |
| r8169 (Realtek GbE) | 163 | 104 | ~73 netdev/NAPI/skb/phylib | minimal netdev/skb/phylib shim, bridge to an NDIS miniport |
| igc (Intel I225/I226) | 197 | 109 | same as r8169 plus ptp/devlink | same net shim, second consumer |
| amdgpu | see README | 25 (after DRM) | | DRM node bridge exists; no mmap/radv yet |

## Takeaways

* All four are in reach: the largest gap (≈110 symbols) is smaller than what the DRM/TTM chain needed.
* The gap is not the hard part. Each driver needs a **subsystem shim + a bridge into the matching ReactOS
  stack** (block->storport, netdev->NDIS, usb->usbport/HID). The GPU path already proved the pattern.
* Net shim is shared by r8169 and igc (and later e1000e/ath/mt7921 Wi-Fi), so it is the best-value network item.
* Priority for a machine that must at least boot and take input: xHCI+HID (keyboard/mouse after UEFI exit),
  then NVMe (install target), then NIC.

## Full USB input stack (usbcore + usb-common + hid + usbhid + hid-generic + xhci-hcd + xhci-pci)

488 externals, **158 missing** (`kpi/survey_missing.txt` has xhci alone; full list from the same script with all
seven objects). They fall into a few shim groups, none of them large:

* driver core: `bus_register`, `driver_register`, `device_attach`, `bus_for_each_dev/drv`, `device_create/destroy`,
  `device_link_add`, platform_device_* (the USB bus needs real match/probe, our `device_add` is a stub today)
* input: `input_allocate_device/register_device/event/set_abs_params` (-> feed ReactOS kbd/mouse class drivers)
* char dev / sysfs / kernfs / pm_runtime / acpi / pm_qos: stubs
* library: `__kfifo_*`, `gen_pool_*`, `dma_pool_*`, notifier chains, `down_interruptible`, `utf16s_to_utf8s`, `add_timer`
