// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI part 4: a minimal blk-mq. Just enough block layer for the NVMe driver:
 *   tag sets with preallocated requests, request queues with one hctx per hardware queue,
 *   request allocation/execution/completion, kernel-buffer mapping, gendisk bookkeeping,
 *   plus kpi_blk_rw() as the synchronous entry point the NT side uses to read/write a disk.
 * No I/O scheduler, no merging, no plugging, no cgroups, no polling, no timeouts (yet).
 */
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include <linux/blk-mq-pci.h>
#include <linux/slab.h>
#include <linux/mempool.h>
#include <linux/srcu.h>
#include <linux/pci.h>
#include <linux/async.h>
#include <linux/once.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/pm_qos.h>
#include <linux/cdev.h>
#include <linux/io_uring/cmd.h>
#include <linux/trace_seq.h>
#include <linux/uuid.h>
#include <linux/suspend.h>
#include <linux/dmi.h>
#include <linux/moduleparam.h>

extern void nt_dbgprint(const char *s);
extern void nt_sleep_ms(unsigned int ms);
extern phys_addr_t kpi_page_phys(struct page *page, size_t offset);

/* ---- tags ------------------------------------------------------------------------------------- */
struct kpi_tags {
	struct blk_mq_tags t;
	unsigned long *busy;
	spinlock_t lock;
};

static struct kpi_tags *kpi_tags_alloc(struct blk_mq_tag_set *set, unsigned int hctx_idx)
{
	struct kpi_tags *kt = kzalloc(sizeof(*kt), GFP_KERNEL);
	unsigned int i, depth = set->queue_depth;
	size_t sz = ALIGN(sizeof(struct request) + set->cmd_size, 64);

	if (!kt)
		return NULL;
	kt->t.nr_tags = depth;
	kt->t.nr_reserved_tags = set->reserved_tags;
	kt->busy = kcalloc(BITS_TO_LONGS(depth), sizeof(long), GFP_KERNEL);
	kt->t.rqs = kcalloc(depth, sizeof(void *), GFP_KERNEL);
	kt->t.static_rqs = kcalloc(depth, sizeof(void *), GFP_KERNEL);
	spin_lock_init(&kt->lock);
	if (!kt->busy || !kt->t.rqs || !kt->t.static_rqs)
		return NULL;
	for (i = 0; i < depth; i++) {
		struct request *rq = kzalloc(sz, GFP_KERNEL);

		if (!rq)
			return NULL;
		rq->tag = i;
		rq->internal_tag = -1;
		kt->t.static_rqs[i] = rq;
		if (set->ops->init_request && set->ops->init_request(set, rq, hctx_idx, 0))
			return NULL;
	}
	return kt;
}

int blk_mq_alloc_tag_set(struct blk_mq_tag_set *set)
{
	unsigned int i, m;

	if (!set->nr_hw_queues || !set->queue_depth || !set->ops->queue_rq)
		return -EINVAL;
	if (!set->nr_maps)
		set->nr_maps = 1;
	set->tags = kcalloc(set->nr_hw_queues, sizeof(void *), GFP_KERNEL);
	if (!set->tags)
		return -ENOMEM;
	for (m = 0; m < set->nr_maps; m++) {
		struct blk_mq_queue_map *map = &set->map[m];

		map->mq_map = kcalloc(nr_cpu_ids, sizeof(unsigned int), GFP_KERNEL);
		if (!map->mq_map)
			return -ENOMEM;
		if (!map->nr_queues && m == 0)
			map->nr_queues = set->nr_hw_queues;
	}
	if (set->ops->map_queues)
		set->ops->map_queues(set);
	else
		blk_mq_map_queues(&set->map[0]);
	for (i = 0; i < set->nr_hw_queues; i++) {
		struct kpi_tags *kt = kpi_tags_alloc(set, i);

		if (!kt)
			return -ENOMEM;
		set->tags[i] = &kt->t;
	}
	pr_info("kpi4_blk: tag set: %u hw queues, depth %u, cmd_size %u\n", set->nr_hw_queues, set->queue_depth, set->cmd_size);
	return 0;
}
void blk_mq_free_tag_set(struct blk_mq_tag_set *set) { }
void blk_mq_map_queues(struct blk_mq_queue_map *qmap)
{
	unsigned int cpu;

	for (cpu = 0; cpu < nr_cpu_ids; cpu++)
		qmap->mq_map[cpu] = qmap->queue_offset + (qmap->nr_queues ? cpu % qmap->nr_queues : 0);
}
void blk_mq_pci_map_queues(struct blk_mq_queue_map *qmap, struct pci_dev *pdev, int offset) { blk_mq_map_queues(qmap); }
void blk_mq_update_nr_hw_queues(struct blk_mq_tag_set *set, int nr_hw_queues) { }
void blk_mq_tagset_busy_iter(struct blk_mq_tag_set *set, busy_tag_iter_fn *fn, void *priv)
{
	unsigned int h, t;

	for (h = 0; h < set->nr_hw_queues; h++) {
		struct kpi_tags *kt = container_of(set->tags[h], struct kpi_tags, t);

		for (t = 0; t < kt->t.nr_tags; t++)
			if (test_bit(t, kt->busy) && kt->t.rqs[t])
				fn(kt->t.rqs[t], priv);
	}
}
void blk_mq_tagset_wait_completed_request(struct blk_mq_tag_set *set) { }

/* ---- queues and disks -------------------------------------------------------------------------- */
struct request_queue *blk_mq_alloc_queue(struct blk_mq_tag_set *set, struct queue_limits *lim, void *queuedata)
{
	struct request_queue *q = kzalloc(sizeof(*q), GFP_KERNEL);
	unsigned int i;

	if (!q)
		return ERR_PTR(-ENOMEM);
	mutex_init(&q->limits_lock);
	mutex_init(&q->sysfs_lock);
	q->mq_ops = set->ops;
	q->tag_set = set;
	q->queuedata = queuedata;
	q->nr_hw_queues = set->nr_hw_queues;
	if (lim)
		q->limits = *lim;
	xa_init(&q->hctx_table);
	for (i = 0; i < set->nr_hw_queues; i++) {
		struct blk_mq_hw_ctx *h = kzalloc(sizeof(*h), GFP_KERNEL);

		if (!h)
			return ERR_PTR(-ENOMEM);
		h->queue = q;
		h->queue_num = i;
		h->tags = set->tags[i];
		h->nr_ctx = 1;
		xa_store(&q->hctx_table, i, h, GFP_KERNEL);
		if (set->ops->init_hctx && set->ops->init_hctx(h, set->driver_data, i))
			return ERR_PTR(-ENOMEM);
	}
	return q;
}
void blk_mq_destroy_queue(struct request_queue *q) { }
void blk_put_queue(struct request_queue *q) { }
int queue_limits_commit_update(struct request_queue *q, struct queue_limits *lim)
{
	q->limits = *lim;
	mutex_unlock(&q->limits_lock);
	return 0;
}

#define KPI_MAX_DISKS 16
static struct gendisk *kpi_disks[KPI_MAX_DISKS];

struct gendisk *__blk_mq_alloc_disk(struct blk_mq_tag_set *set, struct queue_limits *lim, void *queuedata, struct lock_class_key *lkclass)
{
	struct request_queue *q = blk_mq_alloc_queue(set, lim, queuedata);
	struct gendisk *d;

	if (IS_ERR(q))
		return ERR_CAST(q);
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return ERR_PTR(-ENOMEM);
	d->part0 = kzalloc(sizeof(*d->part0), GFP_KERNEL);
	if (!d->part0)
		return ERR_PTR(-ENOMEM);
	d->part0->bd_disk = d;
	d->part0->bd_queue = q;
	d->queue = q;
	q->disk = d;
	d->private_data = NULL;
	return d;
}
void set_capacity(struct gendisk *disk, sector_t sectors) { disk->part0->bd_nr_sectors = sectors; }
bool set_capacity_and_notify(struct gendisk *disk, sector_t size)
{
	bool changed = get_capacity(disk) != size;

	set_capacity(disk, size);
	return changed;
}
void set_disk_ro(struct gendisk *disk, bool read_only) { }
bool disk_live(struct gendisk *disk) { return test_bit(GD_ADDED, &disk->state); }
void blk_mark_disk_dead(struct gendisk *disk) { clear_bit(GD_ADDED, &disk->state); }
void put_disk(struct gendisk *disk) { }
void del_gendisk(struct gendisk *disk) { clear_bit(GD_ADDED, &disk->state); }

extern void (*kpi_blk_disk_hook)(struct gendisk *disk, int added);
void (*kpi_blk_disk_hook)(struct gendisk *disk, int added);
EXPORT_SYMBOL_GPL(kpi_blk_disk_hook);

/* Boot-time self test: read the first 4 KiB through the whole stack and log what came back. */
static struct work_struct kpi_selftest_work;
static struct gendisk *kpi_selftest_disk;
static void kpi_selftest(struct work_struct *w)
{
	extern int kpi_blk_rw(struct gendisk *, int, unsigned long long, void *, unsigned int);
	unsigned char *buf = kzalloc(4096, GFP_KERNEL);
	int ret;

	if (!buf)
		return;
	ret = kpi_blk_rw(kpi_selftest_disk, 0, 0, buf, 4096);
	pr_info("kpi4_blk: selftest read sector 0-7: ret %d, bytes: %02x %02x %02x %02x %02x %02x %02x %02x, sig %02x%02x, marker '%.8s'\n",
		ret, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[511], buf[510], buf + 512);
	kfree(buf);
}

int device_add_disk(struct device *parent, struct gendisk *disk, const struct attribute_group **groups)
{
	int i;

	set_bit(GD_ADDED, &disk->state);
	for (i = 0; i < KPI_MAX_DISKS; i++)
		if (!kpi_disks[i]) { kpi_disks[i] = disk; break; }
	pr_info("kpi4_blk: disk '%s' added, %llu sectors, lbs %u\n", disk->disk_name,
		(unsigned long long)get_capacity(disk), queue_logical_block_size(disk->queue));
	if (kpi_blk_disk_hook)
		kpi_blk_disk_hook(disk, 1);
	else if (!kpi_selftest_disk) {
		kpi_selftest_disk = disk;
		INIT_WORK(&kpi_selftest_work, kpi_selftest);
		queue_work(system_wq, &kpi_selftest_work);
	}
	return 0;
}
int blkdev_compat_ptr_ioctl(struct block_device *bdev, blk_mode_t mode, unsigned int cmd, unsigned long arg) { return -ENOIOCTLCMD; }

/* ---- freeze / quiesce / sync: single-threaded dispatch, so nothing to wait for -------------------- */
void blk_freeze_queue_start(struct request_queue *q) { }
void blk_mq_freeze_queue(struct request_queue *q) { }
void blk_mq_freeze_queue_wait(struct request_queue *q) { }
int blk_mq_freeze_queue_wait_timeout(struct request_queue *q, unsigned long timeout) { return 1; }
void blk_mq_unfreeze_queue(struct request_queue *q) { }
void blk_mq_quiesce_queue(struct request_queue *q) { }
void blk_mq_unquiesce_queue(struct request_queue *q) { }
void blk_mq_quiesce_tagset(struct blk_mq_tag_set *set) { }
void blk_mq_unquiesce_tagset(struct blk_mq_tag_set *set) { }
void blk_mq_wait_quiesce_done(struct blk_mq_tag_set *set) { }
void blk_sync_queue(struct request_queue *q) { }
void blk_mq_delay_kick_requeue_list(struct request_queue *q, unsigned long msecs) { }
int init_srcu_struct(struct srcu_struct *ssp) { return 0; }
void cleanup_srcu_struct(struct srcu_struct *ssp) { }

/* ---- requests ---------------------------------------------------------------------------------- */
static unsigned int kpi_rr;

static struct request *kpi_rq_get(struct request_queue *q, unsigned int hctx_idx, blk_opf_t opf)
{
	struct blk_mq_hw_ctx *h = xa_load(&q->hctx_table, hctx_idx);
	struct kpi_tags *kt = container_of(h->tags, struct kpi_tags, t);
	struct request *rq;
	unsigned long flags;
	unsigned int tag;

	for (;;) {
		spin_lock_irqsave(&kt->lock, flags);
		tag = find_first_zero_bit(kt->busy, kt->t.nr_tags);
		if (tag < kt->t.nr_tags) {
			set_bit(tag, kt->busy);
			spin_unlock_irqrestore(&kt->lock, flags);
			break;
		}
		spin_unlock_irqrestore(&kt->lock, flags);
		msleep(1);
	}
	rq = kt->t.static_rqs[tag];
	kt->t.rqs[tag] = rq;
	rq->q = q;
	rq->mq_hctx = h;
	rq->cmd_flags = opf;
	rq->rq_flags = 0;
	rq->tag = tag;
	rq->bio = rq->biotail = NULL;
	rq->__data_len = 0;
	rq->__sector = 0;
	rq->nr_phys_segments = 0;
	rq->end_io = NULL;
	rq->end_io_data = NULL;
	rq->state = MQ_RQ_IDLE;
	rq->timeout = q->tag_set->timeout;
	return rq;
}

struct request *blk_mq_alloc_request(struct request_queue *q, blk_opf_t opf, blk_mq_req_flags_t flags)
{
	return kpi_rq_get(q, kpi_rr++ % q->nr_hw_queues, opf);
}
struct request *blk_mq_alloc_request_hctx(struct request_queue *q, blk_opf_t opf, blk_mq_req_flags_t flags, unsigned int hctx_idx)
{
	return kpi_rq_get(q, hctx_idx, opf);
}
void blk_mq_free_request(struct request *rq)
{
	struct kpi_tags *kt = container_of(rq->mq_hctx->tags, struct kpi_tags, t);

	rq->state = MQ_RQ_IDLE;
	clear_bit(rq->tag, kt->busy);
}

static void kpi_free_bios(struct request *rq)
{
	struct bio *b = rq->bio, *n;

	for (; b; b = n) {
		n = b->bi_next;
		kfree(b);
	}
	rq->bio = rq->biotail = NULL;
}

/* Map a kernel buffer (kmalloc/vmalloc/direct map) into a one-bio request, one bvec per page. */
int blk_rq_map_kern(struct request_queue *q, struct request *rq, void *kbuf, unsigned int len, gfp_t gfp_mask)
{
	unsigned long va = (unsigned long)kbuf, off = va & ~PAGE_MASK;
	unsigned int nr = DIV_ROUND_UP(off + len, PAGE_SIZE), i, left = len;
	struct bio *bio = kzalloc(sizeof(*bio) + nr * sizeof(struct bio_vec), gfp_mask);

	if (!bio)
		return -ENOMEM;
	bio->bi_io_vec = (struct bio_vec *)(bio + 1);
	bio->bi_max_vecs = nr;
	bio->bi_vcnt = nr;
	bio->bi_opf = rq->cmd_flags;
	bio->bi_iter.bi_size = len;
	for (i = 0; i < nr; i++) {
		unsigned long pva = (va & PAGE_MASK) + i * PAGE_SIZE;
		unsigned int chunk = min_t(unsigned int, left, PAGE_SIZE - (i ? 0 : off));

		/* virt_to_page(): works for direct-map and arena memory alike (see kpi_page_phys) */
		bio->bi_io_vec[i].bv_page = virt_to_page(pva);
		bio->bi_io_vec[i].bv_offset = i ? 0 : off;
		bio->bi_io_vec[i].bv_len = chunk;
		left -= chunk;
	}
	rq->bio = rq->biotail = bio;
	rq->__data_len = len;
	rq->nr_phys_segments = nr;
	return 0;
}
int blk_rq_map_user_io(struct request *req, struct rq_map_data *map_data, void __user *ubuf, unsigned long buf_len, gfp_t gfp_mask, bool vec, int iov_count, bool check_iter_count, int rw) { return -EOPNOTSUPP; }
int blk_rq_map_user_iov(struct request_queue *q, struct request *rq, struct rq_map_data *map_data, const struct iov_iter *iter, gfp_t gfp_mask) { return -EOPNOTSUPP; }
int blk_rq_unmap_user(struct bio *bio) { return 0; }

int __blk_rq_map_sg(struct request_queue *q, struct request *rq, struct scatterlist *sglist, struct scatterlist **last_sg)
{
	struct bio *bio;
	int n = 0;
	struct scatterlist *sg = NULL;

	for (bio = rq->bio; bio; bio = bio->bi_next) {
		struct bvec_iter it;
		struct bio_vec bv;

		bio_for_each_bvec(bv, bio, it) {
			sg = sg ? sg_next(sg) : sglist;
			sg_set_page(sg, bv.bv_page, bv.bv_len, bv.bv_offset);
			n++;
		}
	}
	if (sg)
		sg_mark_end(sg);
	*last_sg = sg;
	return n;
}

static void kpi_rq_end(struct request *rq, blk_status_t status)
{
	struct bio *b;

	for (b = rq->bio; b; b = b->bi_next) {
		b->bi_status = status;
		if (b->bi_end_io)
			b->bi_end_io(b);
	}
	rq->state = MQ_RQ_IDLE;
	if (rq->end_io) {
		if (rq->end_io(rq, status) == RQ_END_IO_FREE)
			blk_mq_free_request(rq);
	} else {
		kpi_free_bios(rq);
		blk_mq_free_request(rq);
	}
}
void blk_mq_end_request(struct request *rq, blk_status_t status) { kpi_rq_end(rq, status); }
void blk_mq_end_request_batch(struct io_comp_batch *iob)
{
	struct request *rq;

	while ((rq = rq_list_pop(&iob->req_list)))
		kpi_rq_end(rq, BLK_STS_OK);
}
void blk_mq_start_request(struct request *rq) { rq->state = MQ_RQ_IN_FLIGHT; }
void blk_mq_complete_request(struct request *rq)
{
	if (rq->q->mq_ops->complete)
		rq->q->mq_ops->complete(rq);
	else
		kpi_rq_end(rq, BLK_STS_OK);
}
bool blk_mq_complete_request_remote(struct request *rq)
{
	WRITE_ONCE(rq->state, MQ_RQ_COMPLETE);
	return false;			/* the caller completes it inline */
}
void blk_mq_requeue_request(struct request *rq, bool kick_requeue_list) { rq->state = MQ_RQ_IDLE; }
bool blk_rq_is_poll(struct request *rq) { return false; }
int blk_rq_poll(struct request *rq, struct io_comp_batch *iob, unsigned int poll_flags) { return 0; }

static void kpi_dispatch(struct request *rq)
{
	struct blk_mq_queue_data bd = { .rq = rq, .last = true };
	blk_status_t ret;

	for (;;) {
		ret = rq->q->mq_ops->queue_rq(rq->mq_hctx, &bd);
		if (ret == BLK_STS_RESOURCE || ret == BLK_STS_DEV_RESOURCE) {
			msleep(1);
			continue;
		}
		break;
	}
	if (ret != BLK_STS_OK)
		kpi_rq_end(rq, ret);
}
void blk_execute_rq_nowait(struct request *rq, bool at_head) { kpi_dispatch(rq); }

struct kpi_sync { struct completion done; blk_status_t status; };
static enum rq_end_io_ret kpi_sync_end(struct request *rq, blk_status_t err)
{
	struct kpi_sync *s = rq->end_io_data;

	s->status = err;
	complete(&s->done);
	return RQ_END_IO_NONE;
}
blk_status_t blk_execute_rq(struct request *rq, bool at_head)
{
	struct kpi_sync s;
	unsigned long to = (rq->timeout ? rq->timeout : 30 * HZ) * 2;

	init_completion(&s.done);
	rq->end_io = kpi_sync_end;
	rq->end_io_data = &s;
	kpi_dispatch(rq);
	if (!wait_for_completion_timeout(&s.done, to)) {
		pr_err("kpi4_blk: request tag %d timed out\n", rq->tag);
		if (rq->q->mq_ops->timeout)
			rq->q->mq_ops->timeout(rq);
		wait_for_completion_timeout(&s.done, to);
	}
	return s.status;
}
unsigned long wait_for_completion_io_timeout(struct completion *x, unsigned long timeout) { return wait_for_completion_timeout(x, timeout); }

blk_status_t errno_to_blk_status(int errno) { return errno ? BLK_STS_IOERR : BLK_STS_OK; }
int blk_status_to_errno(blk_status_t status)
{
	switch (status) {
	case BLK_STS_OK: return 0;
	case BLK_STS_NOSPC: return -ENOSPC;
	case BLK_STS_TIMEOUT: return -ETIMEDOUT;
	case BLK_STS_NOTSUPP: return -EOPNOTSUPP;
	case BLK_STS_RESOURCE: case BLK_STS_DEV_RESOURCE: return -EAGAIN;
	default: return -EIO;
	}
}
const char *blk_op_str(enum req_op op)
{
	switch (op) {
	case REQ_OP_READ: return "READ";
	case REQ_OP_WRITE: return "WRITE";
	case REQ_OP_FLUSH: return "FLUSH";
	case REQ_OP_DISCARD: return "DISCARD";
	default: return "?";
	}
}
void bio_associate_blkg(struct bio *bio) { }

/* ---- the NT-side entry point: synchronous read/write of whole sectors ------------------------------- */
int kpi_blk_disk_count(void)
{
	int i, n = 0;

	for (i = 0; i < KPI_MAX_DISKS; i++)
		n += kpi_disks[i] != NULL;
	return n;
}
struct gendisk *kpi_blk_disk(int idx)
{
	int i;

	for (i = 0; i < KPI_MAX_DISKS; i++)
		if (kpi_disks[i] && idx-- == 0)
			return kpi_disks[i];
	return NULL;
}
unsigned long long kpi_blk_capacity(struct gendisk *d) { return get_capacity(d); }
unsigned int kpi_blk_lbs(struct gendisk *d) { return queue_logical_block_size(d->queue); }
/* `buf` is a kernel VA (any pool); length in bytes, multiple of the logical block size. 0 = ok. */
int kpi_blk_rw(struct gendisk *d, int write, unsigned long long byte_off, void *buf, unsigned int len)
{
	struct request *rq = blk_mq_alloc_request(d->queue, write ? REQ_OP_WRITE : REQ_OP_READ, 0);
	blk_status_t st;
	int ret = blk_rq_map_kern(d->queue, rq, buf, len, GFP_KERNEL);

	if (ret) {
		blk_mq_free_request(rq);
		return ret;
	}
	rq->__sector = byte_off >> 9;
	rq->bio->bi_iter.bi_sector = rq->__sector;
	rq->bio->bi_bdev = d->part0;
	st = blk_execute_rq(rq, false);
	kpi_free_bios(rq);
	blk_mq_free_request(rq);
	return blk_status_to_errno(st);
}
EXPORT_SYMBOL_GPL(kpi_blk_rw);

/* Asynchronous variants for the NT bridge. `done(ctx, errno)` runs in the completion context (an ISR
 * for NVMe): it must only queue work. Call from a thread that may sleep (tag allocation can wait). */
struct kpi_async { void (*done)(void *ctx, int err); void *ctx; };
static enum rq_end_io_ret kpi_async_end(struct request *rq, blk_status_t err)
{
	struct kpi_async *a = rq->end_io_data;
	void (*done)(void *, int) = a->done;
	void *ctx = a->ctx;

	kpi_free_bios(rq);
	kfree(a);
	done(ctx, blk_status_to_errno(err));
	return RQ_END_IO_FREE;
}
int kpi_blk_submit(struct gendisk *d, int write, unsigned long long byte_off, void *buf, unsigned int len,
		   void (*done)(void *ctx, int err), void *ctx)
{
	struct kpi_async *a = kmalloc(sizeof(*a), GFP_KERNEL);
	struct request *rq;
	int ret;

	if (!a)
		return -ENOMEM;
	rq = blk_mq_alloc_request(d->queue, write ? REQ_OP_WRITE : REQ_OP_READ, 0);
	ret = blk_rq_map_kern(d->queue, rq, buf, len, GFP_KERNEL);
	if (ret) {
		blk_mq_free_request(rq);
		kfree(a);
		return ret;
	}
	rq->__sector = byte_off >> 9;
	rq->bio->bi_iter.bi_sector = rq->__sector;
	rq->bio->bi_bdev = d->part0;
	a->done = done;
	a->ctx = ctx;
	rq->end_io = kpi_async_end;
	rq->end_io_data = a;
	kpi_dispatch(rq);
	return 0;
}
int kpi_blk_flush(struct gendisk *d, void (*done)(void *ctx, int err), void *ctx)
{
	struct kpi_async *a = kmalloc(sizeof(*a), GFP_KERNEL);
	struct request *rq;

	if (!a)
		return -ENOMEM;
	rq = blk_mq_alloc_request(d->queue, REQ_OP_FLUSH | REQ_PREFLUSH, 0);
	a->done = done;
	a->ctx = ctx;
	rq->end_io = kpi_async_end;
	rq->end_io_data = a;
	kpi_dispatch(rq);
	return 0;
}
/* The bridge learns about new disks through this hook, called from the disk-registration context. */
void kpi_blk_set_hook(void (*hook)(struct gendisk *disk, int added)) { kpi_blk_disk_hook = hook; }
EXPORT_SYMBOL_GPL(kpi_blk_set_hook);

/* ---- NVMe's other odds and ends ---------------------------------------------------------------------- */
bool __do_once_start(bool *done, unsigned long *flags) { if (*done) return false; *done = true; return true; }
void __do_once_done(bool *done, struct static_key_true *once_key, unsigned long *flags, struct module *mod) { }
async_cookie_t async_schedule_node_domain(async_func_t func, void *data, int node, struct async_domain *domain)
{
	func(data, 0);
	return 0;
}
void async_synchronize_full_domain(struct async_domain *domain) { }
bool acpi_storage_d3(struct device *dev) { return false; }
int cdev_device_add(struct cdev *cdev, struct device *dev) { return 0; }
void cdev_device_del(struct cdev *cdev, struct device *dev) { }
int dev_pm_qos_expose_latency_tolerance(struct device *dev) { return 0; }
void dev_pm_qos_hide_latency_tolerance(struct device *dev) { }
int dev_pm_qos_update_user_latency_tolerance(struct device *dev, s32 val) { return 0; }
bool device_remove_file_self(struct device *dev, const struct device_attribute *attr) { return false; }
size_t dma_opt_mapping_size(struct device *dev) { return SIZE_MAX; }
bool dma_pci_p2pdma_supported(struct device *dev) { return false; }
bool dmi_match(enum dmi_field f, const char *str) { return false; }
unsigned int pm_suspend_global_flags;
const uuid_t uuid_null;
void pcibios_resource_to_bus(struct pci_bus *bus, struct pci_bus_region *region, struct resource *res)
{
	region->start = res->start;
	region->end = res->end;
}
void trace_seq_printf(struct trace_seq *s, const char *fmt, ...) { }
void trace_seq_putc(struct trace_seq *s, unsigned char c) { }
void __io_uring_cmd_do_in_task(struct io_uring_cmd *ioucmd, void (*task_work_cb)(struct io_uring_cmd *, unsigned), unsigned flags) { }
void io_uring_cmd_done(struct io_uring_cmd *ioucmd, ssize_t ret, ssize_t res2, unsigned issue_flags) { }
int io_uring_cmd_import_fixed(u64 ubuf, unsigned long len, int rw, struct iov_iter *iter, void *ioucmd) { return -EOPNOTSUPP; }

mempool_t *mempool_create_node_noprof(int min_nr, mempool_alloc_t *alloc_fn, mempool_free_t *free_fn, void *pool_data, gfp_t gfp_mask, int node_id)
{
	mempool_t *p = kzalloc(sizeof(*p), gfp_mask);

	if (p) {
		p->alloc = alloc_fn;
		p->free = free_fn;
		p->pool_data = pool_data;
		p->min_nr = min_nr;
	}
	return p;
}
void mempool_destroy(mempool_t *pool) { kfree(pool); }
void *mempool_alloc_noprof(mempool_t *pool, gfp_t gfp_mask) { return pool->alloc(gfp_mask, pool->pool_data); }
void mempool_free(void *element, mempool_t *pool) { if (element) pool->free(element, pool->pool_data); }
void *mempool_kmalloc(gfp_t gfp_mask, void *pool_data) { return kmalloc((size_t)pool_data, gfp_mask); }
void mempool_kfree(void *element, void *pool_data) { kfree(element); }

/* module parameters (defaults are baked into the variables; nothing parses a command line here) */
int param_get_uint(char *buffer, const struct kernel_param *kp) { return scnprintf(buffer, PAGE_SIZE, "%u\n", *(unsigned int *)kp->arg); }
int param_set_uint(const char *val, const struct kernel_param *kp) { return kstrtouint(val, 0, kp->arg); }
int param_set_uint_minmax(const char *val, const struct kernel_param *kp, unsigned int min, unsigned int max)
{
	unsigned int v;
	int ret = kstrtouint(val, 0, &v);

	if (ret || v < min || v > max)
		return -EINVAL;
	*(unsigned int *)kp->arg = v;
	return 0;
}
static int kpi_param_set_byte(const char *val, const struct kernel_param *kp) { return kstrtou8(val, 0, kp->arg); }
static int kpi_param_get_byte(char *buffer, const struct kernel_param *kp) { return scnprintf(buffer, PAGE_SIZE, "%u\n", *(unsigned char *)kp->arg); }
const struct kernel_param_ops param_ops_byte = { .set = kpi_param_set_byte, .get = kpi_param_get_byte };

/* PCI: legacy INTx is the only interrupt type for now: one vector, the device's irq line */
int pci_alloc_irq_vectors_affinity(struct pci_dev *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags, struct irq_affinity *affd)
{
	if (min_vecs > 1)
		return -ENOSPC;
	return 1;
}
int pci_request_irq(struct pci_dev *dev, unsigned int nr, irq_handler_t handler, irq_handler_t thread_fn, void *dev_id, const char *fmt, ...)
{
	return request_threaded_irq(dev->irq, handler, thread_fn, IRQF_SHARED, "pci", dev_id);
}
void pci_free_irq(struct pci_dev *dev, unsigned int nr, void *dev_id) { free_irq(dev->irq, dev_id); }
int pci_request_selected_regions(struct pci_dev *pdev, int bars, const char *name) { return 0; }
void pci_release_selected_regions(struct pci_dev *pdev, int bars) { }
int pci_select_bars(struct pci_dev *dev, unsigned long flags)
{
	int i, bars = 0;

	for (i = 0; i < PCI_STD_NUM_BARS; i++)
		if (pci_resource_len(dev, i) && (pci_resource_flags(dev, i) & flags))
			bars |= 1 << i;
	return bars;
}
