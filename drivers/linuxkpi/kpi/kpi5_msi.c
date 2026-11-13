// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI part 5: PCI MSI / MSI-X. The host (shim.c + the APIC HAL) hands out IDT vectors for "MSI slots";
 * here the device is programmed to send them. Linux irq numbers >= 0x1000 are MSI slots (see nt_irq_connect).
 * One message target (the boot CPU), no affinity, no per-vector masking from the driver side (mask bits are left clear).
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/interrupt.h>

extern int nt_msi_alloc(unsigned long long *addr, unsigned int *data);
extern void *nt_ioremap(unsigned long phys, unsigned long size, int cache);
extern void nt_iounmap(void *va, unsigned long size);

struct kpi_msi {
	struct list_head entry;
	struct pci_dev *dev;
	bool msix;
	int nvec;
	int irq[64];
	void __iomem *table;
	unsigned long table_len;
	u8 cap;
};
static LIST_HEAD(kpi_msis);

static struct kpi_msi *kpi_msi_find(struct pci_dev *dev)
{
	struct kpi_msi *m;

	list_for_each_entry(m, &kpi_msis, entry)
		if (m->dev == dev)
			return m;
	return NULL;
}

static void kpi_intx(struct pci_dev *dev, bool enable)
{
	u16 cmd;

	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	if (enable)
		cmd &= ~PCI_COMMAND_INTX_DISABLE;
	else
		cmd |= PCI_COMMAND_INTX_DISABLE;
	pci_write_config_word(dev, PCI_COMMAND, cmd);
}

static int kpi_try_msix(struct pci_dev *dev, int min_vecs, int max_vecs)
{
	u8 cap = pci_find_capability(dev, PCI_CAP_ID_MSIX);
	u16 ctl;
	u32 tbl;
	int total, n, i, bir;
	struct kpi_msi *m;
	unsigned long base;

	if (!cap)
		return -ENODEV;
	pci_read_config_word(dev, cap + PCI_MSIX_FLAGS, &ctl);
	total = (ctl & PCI_MSIX_FLAGS_QSIZE) + 1;
	n = min(max_vecs, total);
	if (n > 62)
		n = 62;
	if (n < min_vecs)
		return -ENOSPC;
	pci_read_config_dword(dev, cap + PCI_MSIX_TABLE, &tbl);
	bir = tbl & PCI_MSIX_TABLE_BIR;
	base = pci_resource_start(dev, bir) + (tbl & PCI_MSIX_TABLE_OFFSET);
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->table_len = n * PCI_MSIX_ENTRY_SIZE;
	m->table = nt_ioremap(base, m->table_len, 0);
	if (!m->table) {
		kfree(m);
		return -ENOMEM;
	}
	/* mask the whole function while the table is filled in */
	pci_write_config_word(dev, cap + PCI_MSIX_FLAGS, ctl | PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL);
	for (i = 0; i < n; i++) {
		unsigned long long addr;
		unsigned int data;
		int irq = nt_msi_alloc(&addr, &data);
		void __iomem *e = m->table + i * PCI_MSIX_ENTRY_SIZE;

		if (irq < 0) {
			n = i;
			break;
		}
		m->irq[i] = irq;
		writel((u32)addr, e + PCI_MSIX_ENTRY_LOWER_ADDR);
		writel((u32)(addr >> 32), e + PCI_MSIX_ENTRY_UPPER_ADDR);
		writel(data, e + PCI_MSIX_ENTRY_DATA);
		writel(0, e + PCI_MSIX_ENTRY_VECTOR_CTRL);	/* unmasked */
	}
	if (n < min_vecs) {
		pci_write_config_word(dev, cap + PCI_MSIX_FLAGS, ctl & ~PCI_MSIX_FLAGS_ENABLE);
		nt_iounmap(m->table, m->table_len);
		kfree(m);
		return -ENOSPC;
	}
	m->dev = dev;
	m->msix = true;
	m->nvec = n;
	m->cap = cap;
	list_add(&m->entry, &kpi_msis);
	kpi_intx(dev, false);
	pci_write_config_word(dev, cap + PCI_MSIX_FLAGS, (ctl | PCI_MSIX_FLAGS_ENABLE) & ~PCI_MSIX_FLAGS_MASKALL);
	pr_info("kpi5_msi: %s: %d MSI-X vector(s)\n", pci_name(dev), n);
	return n;
}

static int kpi_try_msi(struct pci_dev *dev, int min_vecs)
{
	u8 cap = pci_find_capability(dev, PCI_CAP_ID_MSI);
	u16 ctl;
	unsigned long long addr;
	unsigned int data;
	int irq;
	struct kpi_msi *m;

	if (!cap || min_vecs > 1)
		return -ENODEV;
	irq = nt_msi_alloc(&addr, &data);
	if (irq < 0)
		return -ENOSPC;
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	pci_read_config_word(dev, cap + PCI_MSI_FLAGS, &ctl);
	pci_write_config_dword(dev, cap + PCI_MSI_ADDRESS_LO, (u32)addr);
	if (ctl & PCI_MSI_FLAGS_64BIT) {
		pci_write_config_dword(dev, cap + PCI_MSI_ADDRESS_HI, (u32)(addr >> 32));
		pci_write_config_word(dev, cap + PCI_MSI_DATA_64, data);
		if (ctl & PCI_MSI_FLAGS_MASKBIT)
			pci_write_config_dword(dev, cap + PCI_MSI_MASK_64, 0);
	} else {
		pci_write_config_word(dev, cap + PCI_MSI_DATA_32, data);
		if (ctl & PCI_MSI_FLAGS_MASKBIT)
			pci_write_config_dword(dev, cap + PCI_MSI_MASK_32, 0);
	}
	ctl &= ~PCI_MSI_FLAGS_QSIZE;		/* one message */
	pci_write_config_word(dev, cap + PCI_MSI_FLAGS, ctl | PCI_MSI_FLAGS_ENABLE);
	kpi_intx(dev, false);
	m->dev = dev;
	m->nvec = 1;
	m->irq[0] = irq;
	m->cap = cap;
	list_add(&m->entry, &kpi_msis);
	pr_info("kpi5_msi: %s: MSI\n", pci_name(dev));
	return 1;
}

int pci_alloc_irq_vectors_affinity(struct pci_dev *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags, struct irq_affinity *affd)
{
	int ret = -ENOSPC;

	if (kpi_msi_find(dev))
		return -EINVAL;
	if (flags & PCI_IRQ_MSIX) {
		ret = kpi_try_msix(dev, min_vecs, max_vecs);
		if (ret > 0)
			return ret;
	}
	if (flags & PCI_IRQ_MSI) {
		ret = kpi_try_msi(dev, min_vecs);
		if (ret > 0)
			return ret;
	}
	if ((flags & PCI_IRQ_INTX) && min_vecs <= 1)
		return 1;
	return ret < 0 ? ret : -ENOSPC;
}
int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags)
{
	return pci_alloc_irq_vectors_affinity(dev, min_vecs, max_vecs, flags, NULL);
}

void pci_free_irq_vectors(struct pci_dev *dev)
{
	struct kpi_msi *m = kpi_msi_find(dev);
	u16 ctl;

	if (!m)
		return;
	if (m->msix) {
		pci_read_config_word(dev, m->cap + PCI_MSIX_FLAGS, &ctl);
		pci_write_config_word(dev, m->cap + PCI_MSIX_FLAGS, ctl & ~PCI_MSIX_FLAGS_ENABLE);
		nt_iounmap(m->table, m->table_len);
	} else {
		pci_read_config_word(dev, m->cap + PCI_MSI_FLAGS, &ctl);
		pci_write_config_word(dev, m->cap + PCI_MSI_FLAGS, ctl & ~PCI_MSI_FLAGS_ENABLE);
	}
	kpi_intx(dev, true);
	list_del(&m->entry);
	kfree(m);
}

int pci_irq_vector(struct pci_dev *dev, unsigned int nr)
{
	struct kpi_msi *m = kpi_msi_find(dev);

	if (!m)
		return nr == 0 ? dev->irq : -EINVAL;
	return nr < m->nvec ? m->irq[nr] : -EINVAL;
}

int pci_request_irq(struct pci_dev *dev, unsigned int nr, irq_handler_t handler, irq_handler_t thread_fn, void *dev_id, const char *fmt, ...)
{
	int irq = pci_irq_vector(dev, nr);

	if (irq < 0)
		return irq;
	return request_threaded_irq(irq, handler, thread_fn, kpi_msi_find(dev) ? 0 : IRQF_SHARED, "pci", dev_id);
}
void pci_free_irq(struct pci_dev *dev, unsigned int nr, void *dev_id)
{
	int irq = pci_irq_vector(dev, nr);

	if (irq >= 0)
		free_irq(irq, dev_id);
}
