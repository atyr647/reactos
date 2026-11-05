// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI PCI layer. Devices are found by scanning configuration space through the HAL (no NT PnP
 * integration yet), BARs are sized by the usual write-all-ones probe, drivers are probed synchronously
 * from __pci_register_driver(). Interrupts, MSI, power management, resource claiming are stubs.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/ioport.h>

extern int nt_pci_cfg_read(unsigned int bus, unsigned int devfn, unsigned int off, unsigned int len, unsigned int *val);
extern int nt_pci_cfg_write(unsigned int bus, unsigned int devfn, unsigned int off, unsigned int len, unsigned int val);
extern void nt_dbgprint(const char *s);

#define KPI_MAX_BUS 4

static struct pci_bus *kpi_buses[KPI_MAX_BUS];

static struct pci_bus *kpi_bus(unsigned int nr)
{
	if (!kpi_buses[nr]) {
		kpi_buses[nr] = kzalloc(sizeof(struct pci_bus), GFP_KERNEL);
		if (kpi_buses[nr]) {
			kpi_buses[nr]->number = nr;
			INIT_LIST_HEAD(&kpi_buses[nr]->devices);
		}
	}
	return kpi_buses[nr];
}

static u32 cfg_read(struct pci_dev *dev, int off, int len)
{
	unsigned int v = ~0u;
	nt_pci_cfg_read(dev->bus->number, dev->devfn, off, len, &v);
	return v;
}
static void cfg_write(struct pci_dev *dev, int off, int len, u32 v) { nt_pci_cfg_write(dev->bus->number, dev->devfn, off, len, v); }

int pci_bus_read_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 *val)
{ unsigned int v = ~0u; int r = nt_pci_cfg_read(bus->number, devfn, where, 1, &v); *val = v; return r ? PCIBIOS_DEVICE_NOT_FOUND : 0; }
int pci_bus_read_config_word(struct pci_bus *bus, unsigned int devfn, int where, u16 *val)
{ unsigned int v = ~0u; int r = nt_pci_cfg_read(bus->number, devfn, where, 2, &v); *val = v; return r ? PCIBIOS_DEVICE_NOT_FOUND : 0; }
int pci_bus_read_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 *val)
{ unsigned int v = ~0u; int r = nt_pci_cfg_read(bus->number, devfn, where, 4, &v); *val = v; return r ? PCIBIOS_DEVICE_NOT_FOUND : 0; }
int pci_bus_write_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 val) { return nt_pci_cfg_write(bus->number, devfn, where, 1, val); }
int pci_bus_write_config_word(struct pci_bus *bus, unsigned int devfn, int where, u16 val) { return nt_pci_cfg_write(bus->number, devfn, where, 2, val); }
int pci_bus_write_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 val) { return nt_pci_cfg_write(bus->number, devfn, where, 4, val); }
int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val) { return pci_bus_read_config_byte(dev->bus, dev->devfn, where, val); }
int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val) { return pci_bus_read_config_word(dev->bus, dev->devfn, where, val); }
int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val) { return pci_bus_read_config_dword(dev->bus, dev->devfn, where, val); }
int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val) { return pci_bus_write_config_byte(dev->bus, dev->devfn, where, val); }
int pci_write_config_word(const struct pci_dev *dev, int where, u16 val) { return pci_bus_write_config_word(dev->bus, dev->devfn, where, val); }
int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val) { return pci_bus_write_config_dword(dev->bus, dev->devfn, where, val); }

/* Fill in vendor/device ids, class and BARs. */
static struct pci_dev *kpi_pci_make_dev(unsigned int bus, unsigned int devfn)
{
	struct pci_dev *dev;
	unsigned int id, v, cmd;
	int i;

	if (nt_pci_cfg_read(bus, devfn, 0, 4, &id) || (id & 0xffff) == 0xffff || id == 0)
		return NULL;
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return NULL;
	dev->bus = kpi_bus(bus);
	dev->devfn = devfn;
	dev->vendor = id & 0xffff;
	dev->device = id >> 16;
	nt_pci_cfg_read(bus, devfn, 8, 4, &v);
	dev->revision = v & 0xff;
	dev->class = v >> 8;
	nt_pci_cfg_read(bus, devfn, 0x2c, 4, &v);
	dev->subsystem_vendor = v & 0xffff;
	dev->subsystem_device = v >> 16;
	nt_pci_cfg_read(bus, devfn, 0x0e, 1, &v);
	dev->hdr_type = v & 0x7f;
	nt_pci_cfg_read(bus, devfn, 0x3c, 1, &v);
	dev->irq = v;
	dev->multifunction = 0;
	device_initialize(&dev->dev);
	dev_set_name(&dev->dev, "%04x:%02x:%02x.%d", 0, bus, devfn >> 3, devfn & 7);
	dev->dev.bus = &pci_bus_type;
	dev->dma_mask = ~0ULL;
	dev->dev.dma_mask = &dev->dma_mask;
	dev->dev.coherent_dma_mask = ~0ULL;

	/* BAR sizing with decoding disabled */
	nt_pci_cfg_read(bus, devfn, 4, 2, &cmd);
	nt_pci_cfg_write(bus, devfn, 4, 2, cmd & ~3u);
	for (i = 0; i < 6 && dev->hdr_type == 0; i++) {
		unsigned int bar, size, hi = 0, hisize = 0;
		u64 start, sz;
		bool io, is64;
		struct resource *r = &dev->resource[i];

		nt_pci_cfg_read(bus, devfn, 0x10 + i * 4, 4, &bar);
		if (!bar)
			continue;
		io = bar & 1;
		is64 = !io && ((bar & 6) == 4);
		nt_pci_cfg_write(bus, devfn, 0x10 + i * 4, 4, 0xffffffff);
		nt_pci_cfg_read(bus, devfn, 0x10 + i * 4, 4, &size);
		nt_pci_cfg_write(bus, devfn, 0x10 + i * 4, 4, bar);
		start = bar & (io ? ~3u : ~0xfu);
		sz = io ? (~(size & ~3u) + 1) & 0xffff : (~(size & ~0xfu) + 1);
		if (is64) {
			nt_pci_cfg_read(bus, devfn, 0x14 + i * 4, 4, &hi);
			nt_pci_cfg_write(bus, devfn, 0x14 + i * 4, 4, 0xffffffff);
			nt_pci_cfg_read(bus, devfn, 0x14 + i * 4, 4, &hisize);
			nt_pci_cfg_write(bus, devfn, 0x14 + i * 4, 4, hi);
			start |= (u64)hi << 32;
			sz = (~(((u64)hisize << 32) | (size & ~0xfu)) + 1);
		}
		r->start = start;
		r->end = start + sz - 1;
		r->flags = io ? IORESOURCE_IO : (IORESOURCE_MEM | ((bar & 8) ? IORESOURCE_PREFETCH : 0) | (is64 ? IORESOURCE_MEM_64 : 0));
		r->name = dev_name(&dev->dev);
		if (is64)
			i++;
	}
	nt_pci_cfg_write(bus, devfn, 4, 2, cmd);
	return dev;
}

static bool kpi_pci_match(const struct pci_device_id *id, struct pci_dev *dev)
{
	return (id->vendor == PCI_ANY_ID || id->vendor == dev->vendor) &&
	       (id->device == PCI_ANY_ID || id->device == dev->device) &&
	       (id->subvendor == PCI_ANY_ID || id->subvendor == dev->subsystem_vendor) &&
	       (id->subdevice == PCI_ANY_ID || id->subdevice == dev->subsystem_device) &&
	       !((id->class ^ dev->class) & id->class_mask);
}

int __pci_register_driver(struct pci_driver *drv, struct module *owner, const char *mod_name)
{
	unsigned int bus, devfn;
	int bound = 0;

	drv->driver.name = drv->name;
	for (bus = 0; bus < KPI_MAX_BUS; bus++) {
		for (devfn = 0; devfn < 256; devfn++) {
			struct pci_dev *dev;
			const struct pci_device_id *id;

			if (nt_pci_cfg_read(bus, devfn, 0, 4, &(unsigned int){0}) )
				continue;
			dev = kpi_pci_make_dev(bus, devfn);
			if (!dev)
				continue;
			for (id = drv->id_table; id && (id->vendor || id->subvendor || id->class_mask); id++) {
				if (!kpi_pci_match(id, dev))
					continue;
				pr_info("kpi2_pci: %s matches %04x:%04x at %s\n", drv->name, dev->vendor, dev->device, dev_name(&dev->dev));
				dev->driver = drv;
				dev->dev.driver = &drv->driver;
				if (drv->probe(dev, id) == 0) {
					bound++;
				} else {
					dev->driver = NULL;
					dev->dev.driver = NULL;
				}
				break;
			}
		}
	}
	pr_info("kpi2_pci: %s bound %d device(s)\n", drv->name, bound);
	return 0;
}

void pci_unregister_driver(struct pci_driver *drv) { }
int pcim_enable_device(struct pci_dev *pdev) { atomic_inc(&pdev->enable_cnt); return 0; }
int pci_enable_device(struct pci_dev *dev) { return pcim_enable_device(dev); }
int pci_enable_device_mem(struct pci_dev *dev) { return pcim_enable_device(dev); }
void pci_disable_device(struct pci_dev *dev) { atomic_dec(&dev->enable_cnt); }
void pci_set_master(struct pci_dev *dev)
{
	u16 cmd = cfg_read(dev, 4, 2);
	cfg_write(dev, 4, 2, cmd | 7);   /* IO, memory, bus master */
}
void pci_clear_master(struct pci_dev *dev)
{
	u16 cmd = cfg_read(dev, 4, 2);
	cfg_write(dev, 4, 2, cmd & ~4u);
}
int pci_request_region(struct pci_dev *pdev, int bar, const char *name) { return 0; }
void pci_release_regions(struct pci_dev *pdev) { }
int pci_request_regions(struct pci_dev *pdev, const char *name) { return 0; }
void pci_release_region(struct pci_dev *pdev, int bar) { }
void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen)
{
	struct resource *r = &dev->resource[bar];
	unsigned long len = resource_size(r);

	if (!r->flags || !len) return NULL;
	if (maxlen && len > maxlen) len = maxlen;
	if (r->flags & IORESOURCE_IO)
		return (void __iomem *)(unsigned long)r->start;
	return ioremap(r->start, len);
}
void pci_iounmap(struct pci_dev *dev, void __iomem *p) { if ((unsigned long)p > 0xffff) iounmap(p); }
u8 pci_find_capability(struct pci_dev *dev, int cap)
{
	u8 pos = cfg_read(dev, 0x34, 1);
	int guard = 48;

	while (pos >= 0x40 && guard--) {
		u8 id = cfg_read(dev, pos, 1);
		if (id == cap) return pos;
		pos = cfg_read(dev, pos + 1, 1);
	}
	return 0;
}
struct pci_dev *pci_dev_get(struct pci_dev *dev) { if (dev) get_device(&dev->dev); return dev; }
void pci_dev_put(struct pci_dev *dev) { if (dev) put_device(&dev->dev); }
