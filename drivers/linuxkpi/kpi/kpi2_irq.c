// SPDX-License-Identifier: GPL-2.0
/* LinuxKPI IRQ and firmware API on top of the host bridges (legacy interrupt lines only; no MSI yet). */
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/mm.h>

extern void *nt_irq_connect(int irq, irq_handler_t handler, irq_handler_t thread, void *dev, int shared);
extern void nt_irq_disconnect(void *handle);
extern void *nt_read_firmware(const char *name, unsigned long *size);

struct kpi_irq { struct list_head entry; int irq; void *dev; void *handle; };
static LIST_HEAD(kpi_irqs);

int request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn, unsigned long flags, const char *name, void *dev)
{
	struct kpi_irq *k = kzalloc(sizeof(*k), GFP_KERNEL);

	if (!k) return -ENOMEM;
	k->irq = irq; k->dev = dev;
	k->handle = nt_irq_connect(irq, handler, thread_fn, dev, !!(flags & IRQF_SHARED));
	if (!k->handle) { kfree(k); return -EINVAL; }
	list_add(&k->entry, &kpi_irqs);
	return 0;
}

const void *free_irq(unsigned int irq, void *dev)
{
	struct kpi_irq *k;

	list_for_each_entry(k, &kpi_irqs, entry) {
		if (k->irq == irq && k->dev == dev) {
			list_del(&k->entry);
			nt_irq_disconnect(k->handle);
			kfree(k);
			break;
		}
	}
	return NULL;
}
void enable_irq(unsigned int irq) { }
void disable_irq(unsigned int irq) { }

int request_firmware(const struct firmware **firmware_p, const char *name, struct device *device)
{
	struct firmware *fw = kzalloc(sizeof(*fw), GFP_KERNEL);
	unsigned long size = 0;
	void *data;

	if (!fw) return -ENOMEM;
	data = nt_read_firmware(name, &size);
	if (!data) { kfree(fw); *firmware_p = NULL; return -ENOENT; }
	fw->data = data;
	fw->size = size;
	*firmware_p = fw;
	return 0;
}
void release_firmware(const struct firmware *fw)
{
	if (fw) { kfree(fw->data); kfree(fw); }
}

bool has_capability_noaudit(struct task_struct *t, int cap) { return true; }
