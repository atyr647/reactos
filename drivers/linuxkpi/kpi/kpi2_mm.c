// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI memory layer: pages, direct map, vmap, ioremap, DMA API.
 *
 * All Linux "system memory" comes from ONE physically contiguous region taken from NT at start
 * (kpi_mem_mb module-level default below). That makes Linux's direct map trivial: va = pa + const,
 * exactly what page_address()/__va()/__pa() assume (page_offset_base). struct page descriptors live
 * in a plain array indexed by pfn - region_base_pfn (vmemmap_base is biased so pfn_to_page() works).
 * Limitations (README "Still to do"): the region size is fixed, no NUMA, no highmem, no IOMMU
 * (dma address == physical address), cache attributes (WC/UC) are not applied to system pages yet.
 */
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/vmalloc.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/highmem.h>
#include <linux/shrinker.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/pgtable.h>
#include <linux/rwsem.h>
#include <linux/hdmi.h>
#include <linux/agp_backend.h>
#include <linux/eventfd.h>
#include <linux/sync_file.h>
#include <linux/dma-buf.h>
#include <asm/processor.h>

extern void *nt_contig_alloc(unsigned long size, unsigned long *phys);
extern void nt_contig_free(void *va, unsigned long size);
extern void *nt_ioremap(unsigned long phys, unsigned long size, int cache);
extern void nt_iounmap(void *va, unsigned long size);
extern void *nt_map_pfns(const unsigned long *pfns, unsigned int count, int cache, void **mdl);
extern void nt_unmap_pfns(void *va, void *mdl);
extern unsigned long nt_virt_to_phys(void *va);
extern void nt_dbgprint(const char *s);
extern void nt_sleep_ms(unsigned int ms);

#define KPI_REGION_MB 256UL

static unsigned long region_pa, region_size, region_pfn0, region_pages;
static void *region_va;
static struct page *page_array;
static unsigned long *page_bitmap;           /* 1 = used */
static DEFINE_SPINLOCK(page_lock);

struct cpuinfo_x86 boot_cpu_data = { .x86_phys_bits = 48, .x86_virt_bits = 48, .x86_cache_alignment = 64, .x86_clflush_size = 64 };
struct static_key_false init_on_free;
unsigned long pgprot_writecombine_dummy;

int kpi_mm_init(void)
{
	unsigned long i;

	region_size = KPI_REGION_MB << 20;
	region_va = nt_contig_alloc(region_size, &region_pa);
	if (!region_va) {
		nt_dbgprint("kpi2_mm: cannot allocate the contiguous memory region\n");
		return -ENOMEM;
	}
	region_pfn0 = region_pa >> PAGE_SHIFT;
	region_pages = region_size >> PAGE_SHIFT;
	page_offset_base = (unsigned long)region_va - region_pa;
	page_array = vzalloc(region_pages * sizeof(struct page));
	page_bitmap = vzalloc(BITS_TO_LONGS(region_pages) * sizeof(long));
	if (!page_array || !page_bitmap)
		return -ENOMEM;
	vmemmap_base = (unsigned long)page_array - region_pfn0 * sizeof(struct page);
	for (i = 0; i < region_pages; i++)
		set_page_count(&page_array[i], 0);
	pr_info("kpi2_mm: %lu MiB at pa %lx va %p (page_offset_base %lx)\n", KPI_REGION_MB, region_pa, region_va, page_offset_base);
	return 0;
}

/* ---- page allocator: first-fit over the bitmap, naturally aligned runs ----------------------- */
struct page *__alloc_pages_noprof(gfp_t gfp, unsigned int order, int nid, nodemask_t *nodemask)
{
	unsigned long n = 1UL << order, start, i, flags;
	struct page *page = NULL;

	spin_lock_irqsave(&page_lock, flags);
	for (start = 0; start + n <= region_pages; start += n) {
		bool free = true;

		for (i = 0; i < n; i++)
			if (test_bit(start + i, page_bitmap)) { free = false; break; }
		if (!free)
			continue;
		for (i = 0; i < n; i++)
			__set_bit(start + i, page_bitmap);
		page = &page_array[start];
		break;
	}
	spin_unlock_irqrestore(&page_lock, flags);
	if (!page)
		return NULL;
	for (i = 0; i < n; i++) {
		set_page_count(&page[i], i == 0 ? 1 : 0);
		page[i].private = 0;
		page[i].flags = 0;
	}
	if (order)
		page->private = order;
	if (gfp & __GFP_ZERO)
		memset(page_address(page), 0, n << PAGE_SHIFT);
	return page;
}

void __free_pages(struct page *page, unsigned int order)
{
	unsigned long start = page - page_array, n = 1UL << order, i, flags;

	if (!page)
		return;
	spin_lock_irqsave(&page_lock, flags);
	for (i = 0; i < n; i++)
		__clear_bit(start + i, page_bitmap);
	spin_unlock_irqrestore(&page_lock, flags);
}

unsigned long get_free_pages_noprof(gfp_t gfp, unsigned int order)
{
	struct page *p = __alloc_pages_noprof(gfp, order, 0, NULL);
	return p ? (unsigned long)page_address(p) : 0;
}

void free_pages(unsigned long addr, unsigned int order)
{
	if (addr)
		__free_pages(virt_to_page((void *)addr), order);
}

void __folio_put(struct folio *folio)
{
	struct page *p = &folio->page;
	__free_pages(p, p->private ? p->private : 0);
}
void __folio_batch_release(struct folio_batch *pvec) { folio_batch_reinit(pvec); }
void copy_page(void *to, void *from) { memcpy(to, from, PAGE_SIZE); }
void mark_page_accessed(struct page *page) { }
void folio_mark_accessed(struct folio *folio) { }
bool folio_mark_dirty(struct folio *folio) { return false; }
bool set_page_dirty(struct page *page) { return false; }
void check_move_unevictable_folios(struct folio_batch *fbatch) { }

/* clear_page() calls this with %rcx = bytes, %rdi = destination, %al = 0 */
__asm__(".globl rep_stos_alternative\n"
	"rep_stos_alternative:\n"
	"	rep stosb\n"
	"	ret\n");

/* ---- caching attributes: not applied to system pages (the region is cached memory) ---------- */
int set_pages_array_wc(struct page **pages, int addrinarray) { return 0; }
int set_pages_array_uc(struct page **pages, int addrinarray) { return 0; }
int set_pages_wb(struct page *page, int numpages) { return 0; }
pgprot_t pgprot_writecombine(pgprot_t prot) { return prot; }
pgprot_t vm_get_page_prot(unsigned long vm_flags) { return __pgprot(0); }
vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, pgprot_t pgprot) { return VM_FAULT_SIGBUS; }

/* ---- vmap: contiguous runs use the direct map, everything else an MDL mapping ------------------ */
struct kpi_vmap {
	struct list_head entry;
	void *va, *mdl;
	unsigned int count;
	struct page **pages;
};
static LIST_HEAD(kpi_vmaps);
static DEFINE_SPINLOCK(kpi_vmap_lock);

void *vmap(struct page **pages, unsigned int count, unsigned long flags, pgprot_t prot)
{
	unsigned int i;
	bool contig = true;
	unsigned long *pfns;
	struct kpi_vmap *v;
	unsigned long irq;

	for (i = 1; i < count; i++)
		if (pages[i] != pages[i - 1] + 1) { contig = false; break; }
	if (contig && count)
		return page_address(pages[0]);
	pfns = kmalloc_array(count, sizeof(*pfns), GFP_KERNEL);
	v = kzalloc(sizeof(*v), GFP_KERNEL);
	if (!pfns || !v) { kfree(pfns); kfree(v); return NULL; }
	for (i = 0; i < count; i++)
		pfns[i] = page_to_pfn(pages[i]);
	v->va = nt_map_pfns(pfns, count, (flags & VM_MAP_PUT_PAGES) ? 1 : 1, &v->mdl);
	kfree(pfns);
	if (!v->va) { kfree(v); return NULL; }
	v->count = count;
	v->pages = kmemdup(pages, count * sizeof(*pages), GFP_KERNEL);
	spin_lock_irqsave(&kpi_vmap_lock, irq);
	list_add(&v->entry, &kpi_vmaps);
	spin_unlock_irqrestore(&kpi_vmap_lock, irq);
	return v->va;
}

void vunmap(const void *addr)
{
	struct kpi_vmap *v;
	unsigned long irq;

	spin_lock_irqsave(&kpi_vmap_lock, irq);
	list_for_each_entry(v, &kpi_vmaps, entry) {
		if (v->va == addr) {
			list_del(&v->entry);
			spin_unlock_irqrestore(&kpi_vmap_lock, irq);
			nt_unmap_pfns(v->va, v->mdl);
			kfree(v->pages);
			kfree(v);
			return;
		}
	}
	spin_unlock_irqrestore(&kpi_vmap_lock, irq);
}

struct page *vmalloc_to_page(const void *addr)
{
	struct kpi_vmap *v;
	unsigned long irq, off;
	struct page *res = NULL;

	if ((unsigned long)addr >= (unsigned long)region_va && (unsigned long)addr < (unsigned long)region_va + region_size)
		return virt_to_page(addr);
	spin_lock_irqsave(&kpi_vmap_lock, irq);
	list_for_each_entry(v, &kpi_vmaps, entry) {
		off = (unsigned long)addr - (unsigned long)v->va;
		if ((unsigned long)addr >= (unsigned long)v->va && off < (unsigned long)v->count * PAGE_SIZE) {
			res = v->pages[off >> PAGE_SHIFT];
			break;
		}
	}
	spin_unlock_irqrestore(&kpi_vmap_lock, irq);
	return res;
}
int is_vmalloc_or_module_addr(const void *x) { return 0; }

/* ---- ioremap ------------------------------------------------------------------------------ */
struct kpi_iomap { struct list_head entry; void *va; unsigned long size; };
static LIST_HEAD(kpi_iomaps);
static DEFINE_SPINLOCK(kpi_iomap_lock);

static void __iomem *kpi_ioremap(resource_size_t phys, unsigned long size, int cache)
{
	struct kpi_iomap *m = kzalloc(sizeof(*m), GFP_KERNEL);
	unsigned long irq;

	if (!m) return NULL;
	m->va = nt_ioremap(phys, size, cache);
	if (!m->va) { kfree(m); return NULL; }
	m->size = size;
	spin_lock_irqsave(&kpi_iomap_lock, irq);
	list_add(&m->entry, &kpi_iomaps);
	spin_unlock_irqrestore(&kpi_iomap_lock, irq);
	return (void __iomem *)m->va;
}
void __iomem *ioremap(resource_size_t phys, unsigned long size) { return kpi_ioremap(phys, size, 0); }
void __iomem *ioremap_cache(resource_size_t phys, unsigned long size) { return kpi_ioremap(phys, size, 1); }
void __iomem *ioremap_wc(resource_size_t phys, unsigned long size) { return kpi_ioremap(phys, size, 2); }
void iounmap(volatile void __iomem *addr)
{
	struct kpi_iomap *m;
	unsigned long irq;

	spin_lock_irqsave(&kpi_iomap_lock, irq);
	list_for_each_entry(m, &kpi_iomaps, entry) {
		if (m->va == (void *)addr) {
			list_del(&m->entry);
			spin_unlock_irqrestore(&kpi_iomap_lock, irq);
			nt_iounmap(m->va, m->size);
			kfree(m);
			return;
		}
	}
	spin_unlock_irqrestore(&kpi_iomap_lock, irq);
}
void *memremap(resource_size_t offset, size_t size, unsigned long flags) { return (void __force *)kpi_ioremap(offset, size, 1); }
void memunmap(void *addr) { iounmap((void __iomem *)addr); }

/* ---- DMA API: bus address == physical address, always coherent ------------------------------- */
void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs)
{
	unsigned int order = get_order(size);
	struct page *p = __alloc_pages_noprof(gfp | __GFP_ZERO, order, 0, NULL);

	if (!p) return NULL;
	*dma_handle = page_to_phys(p);
	return page_address(p);
}
void dma_free_attrs(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle, unsigned long attrs)
{
	free_pages((unsigned long)cpu_addr, get_order(size));
}
/*
 * Translate a struct page + offset to a bus address. Pages from the direct-map region are plain
 * arithmetic. kmalloc() memory comes from the allocator arena instead (outside the region), where
 * virt_to_page() yields a bogus-but-invertible page pointer: recover the kernel VA from it and ask the
 * memory manager for the real physical address. (Valid while the buffer sits inside one page, which
 * holds for USB control/HID traffic; large kmalloc DMA buffers need region-backed allocations.)
 */
phys_addr_t kpi_page_phys(struct page *page, size_t offset)
{
	unsigned long pfn = ((unsigned long)page - vmemmap_base) / sizeof(struct page);
	unsigned long va = page_offset_base + (pfn << PAGE_SHIFT) + offset;

	if (va >= (unsigned long)region_va && va < (unsigned long)region_va + region_size)
		return va - page_offset_base;
	return nt_virt_to_phys((void *)va);
}
dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page, size_t offset, size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	return kpi_page_phys(page, offset);
}
void dma_unmap_page_attrs(struct device *dev, dma_addr_t addr, size_t size, enum dma_data_direction dir, unsigned long attrs) { }
int dma_map_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sgtable_sg(sgt, sg, i) {
		sg->dma_address = kpi_page_phys(sg_page(sg), sg->offset);
		sg_dma_len(sg) = sg->length;
	}
	sgt->nents = sgt->orig_nents;
	return 0;
}
void dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sg, int nents, enum dma_data_direction dir, unsigned long attrs) { }
size_t dma_max_mapping_size(struct device *dev) { return SIZE_MAX; }

/* ---- shrinkers: nothing is ever reclaimed -------------------------------------------------- */
struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...) { return kzalloc(sizeof(struct shrinker), GFP_KERNEL); }
void shrinker_register(struct shrinker *shrinker) { }
void shrinker_free(struct shrinker *shrinker) { kfree(shrinker); }

/* ---- shmem (swap-backed GEM pages), dma-buf, sync_file, eventfd, agp: not supported yet --------- */
struct folio *shmem_read_folio_gfp(struct address_space *mapping, pgoff_t index, gfp_t gfp) { return ERR_PTR(-ENOSYS); }
struct page *shmem_read_mapping_page_gfp(struct address_space *mapping, pgoff_t index, gfp_t gfp) { return ERR_PTR(-ENOSYS); }
struct dma_buf *dma_buf_export(const struct dma_buf_export_info *exp_info) { return ERR_PTR(-ENOSYS); }
struct dma_buf *dma_buf_get(int fd) { return ERR_PTR(-ENOSYS); }
void dma_buf_put(struct dma_buf *dmabuf) { }
struct dma_buf_attachment *dma_buf_attach(struct dma_buf *dmabuf, struct device *dev) { return ERR_PTR(-ENOSYS); }
void dma_buf_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach) { }
int dma_buf_begin_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction dir) { return -ENOSYS; }
int dma_buf_end_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction dir) { return -ENOSYS; }
struct sg_table *dma_buf_map_attachment_unlocked(struct dma_buf_attachment *attach, enum dma_data_direction dir) { return ERR_PTR(-ENOSYS); }
void dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *attach, struct sg_table *sg_table, enum dma_data_direction dir) { }
struct sync_file *sync_file_create(struct dma_fence *fence) { return NULL; }
struct dma_fence *sync_file_get_fence(int fd) { return NULL; }
struct eventfd_ctx *eventfd_ctx_fdget(int fd) { return ERR_PTR(-ENOSYS); }
void eventfd_ctx_put(struct eventfd_ctx *ctx) { }
void eventfd_signal_mask(struct eventfd_ctx *ctx, __poll_t mask) { }
struct agp_memory *agp_allocate_memory(struct agp_bridge_data *bridge, size_t page_count, u32 type) { return NULL; }
int agp_bind_memory(struct agp_memory *curr, off_t pg_start) { return -ENODEV; }
int agp_unbind_memory(struct agp_memory *curr) { return -ENODEV; }
void agp_free_memory(struct agp_memory *curr) { }

/* ---- tracepoint left over from mmap_lock.h --------------------------------------------------- */
struct tracepoint __tracepoint_mmap_lock_released;
void __mmap_lock_do_trace_released(struct mm_struct *mm, bool write) { }

int page_is_ram(unsigned long pfn) { return pfn >= region_pfn0 && pfn < region_pfn0 + region_pages; }
