// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI part 6: a minimal SCSI mid-layer, just enough for usb-storage (and later uas / other SCSI low-level drivers).
 *
 * There is no sd/sr: the NT side already speaks SCSI (SRBs with CDBs), so every device found by scsi_scan_host() is
 * reported to the host (kpi_scsi_set_hook) which exposes it as a disk and passes the CDBs straight through
 * kpi_scsi_submit(). The mid-layer's job here: scan LUNs (INQUIRY), call the driver's slave_alloc/slave_configure,
 * build scsi_cmnd + scatterlist for each command and complete it when the driver calls scsi_done().
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/blkdev.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <scsi/scsi.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_common.h>

extern void nt_dbgprint(const char *s);


static LIST_HEAD(kpi_hosts);
static atomic_t kpi_host_no = ATOMIC_INIT(0);
void (*kpi_scsi_hook)(struct scsi_device *sdev, int added);

/* ---- hosts ------------------------------------------------------------------------------------------ */
struct kpi_shost {
	struct list_head entry;
	struct Scsi_Host *sh;
};
static LIST_HEAD(kpi_shosts);

struct Scsi_Host *scsi_host_alloc(const struct scsi_host_template *sht, int privsize)
{
	struct Scsi_Host *sh = kzalloc(sizeof(*sh) + privsize, GFP_KERNEL);

	if (!sh)
		return NULL;
	sh->hostt = sht;
	sh->host_no = atomic_inc_return(&kpi_host_no) - 1;
	sh->unique_id = sh->host_no;
	sh->max_id = 8;
	sh->max_lun = 8;
	sh->max_channel = 0;
	sh->this_id = sht->this_id;
	sh->can_queue = sht->can_queue ? sht->can_queue : 1;
	sh->sg_tablesize = sht->sg_tablesize ? sht->sg_tablesize : SG_ALL;
	sh->cmd_per_lun = sht->cmd_per_lun ? sht->cmd_per_lun : 1;
	sh->max_sectors = sht->max_sectors ? sht->max_sectors : 1024;
	sh->dma_boundary = sht->dma_boundary;
	sh->host_lock = &sh->default_lock;
	spin_lock_init(&sh->default_lock);
	INIT_LIST_HEAD(&sh->__devices);
	INIT_LIST_HEAD(&sh->__targets);
	device_initialize(&sh->shost_gendev);
	dev_set_name(&sh->shost_gendev, "host%d", sh->host_no);
	return sh;
}
int scsi_add_host_with_dma(struct Scsi_Host *shost, struct device *dev, struct device *dma_dev)
{
	struct kpi_shost *k = kzalloc(sizeof(*k), GFP_KERNEL);

	if (!k)
		return -ENOMEM;
	shost->shost_gendev.parent = dev;
	shost->dma_dev = dma_dev;
	k->sh = shost;
	list_add_tail(&k->entry, &kpi_shosts);
	return 0;
}
void scsi_host_put(struct Scsi_Host *shost) { }
int scsi_is_host_device(const struct device *dev) { return 0; }

/* ---- command execution ------------------------------------------------------------------------------- */
struct kpi_cmd {
	struct work_struct done_work;
	void (*done)(void *ctx, int result, int resid, const unsigned char *sense);
	void *ctx;
	struct scatterlist *sgl;
	unsigned char sense[SCSI_SENSE_BUFFERSIZE];
	struct request rq;		/* scsi_cmd_to_rq() is (struct request *)cmd - 1 */
	struct scsi_cmnd cmd;		/* last: followed by the driver's private area (scsi_cmd_priv) */
};

static struct kpi_cmd *kpi_cmd_of(struct scsi_cmnd *c) { return container_of(c, struct kpi_cmd, cmd); }

/*
 * Completion is deferred to a worker, like Linux's blk-mq softirq completion: usb-storage calls scsi_done() *before* it
 * clears its in-flight command, so an immediate callback lets the next command arrive while the host still looks busy.
 */
static void kpi_scsi_done_work(struct work_struct *w)
{
	struct kpi_cmd *k = container_of(w, struct kpi_cmd, done_work);
	void (*done)(void *, int, int, const unsigned char *) = k->done;
	void *ctx = k->ctx;
	int result = k->cmd.result, resid = k->cmd.resid_len;
	unsigned char sense[SCSI_SENSE_BUFFERSIZE];

	memcpy(sense, k->sense, sizeof(sense));
	kfree(k->sgl);
	kfree(k);
	done(ctx, result, resid, sense);
}
void scsi_done(struct scsi_cmnd *cmd)
{
	struct kpi_cmd *k = kpi_cmd_of(cmd);

	INIT_WORK(&k->done_work, kpi_scsi_done_work);
	queue_work(system_bh_wq, &k->done_work);
}
void scsi_done_direct(struct scsi_cmnd *cmd) { scsi_done(cmd); }

/* done(ctx, result, resid, sense): result is the SCSI result word (host<<16 | status). `buf` must stay valid until done. */
int kpi_scsi_submit(struct scsi_device *sdev, const unsigned char *cdb, int cdb_len, int dir, void *buf, unsigned int len,
		    void (*done)(void *, int, int, const unsigned char *), void *ctx)
{
	struct Scsi_Host *sh = sdev->host;
	unsigned int nr = 0, i, left = len;
	struct kpi_cmd *k = kzalloc(sizeof(*k) + sh->hostt->cmd_size, GFP_KERNEL);
	struct scsi_cmnd *c;
	int ret;

	if (!k)
		return -ENOMEM;
	c = &k->cmd;
	if (len) {
		unsigned long va = (unsigned long)buf, off = va & ~PAGE_MASK;

		nr = DIV_ROUND_UP(off + len, PAGE_SIZE);
		k->sgl = kcalloc(nr, sizeof(struct scatterlist), GFP_KERNEL);
		if (!k->sgl) {
			kfree(k);
			return -ENOMEM;
		}
		sg_init_table(k->sgl, nr);
		for (i = 0; i < nr; i++) {
			unsigned long pva = (va & PAGE_MASK) + i * PAGE_SIZE;
			unsigned int chunk = min_t(unsigned int, left, PAGE_SIZE - (i ? 0 : off));

			sg_set_page(&k->sgl[i], virt_to_page(pva), chunk, i ? 0 : off);
			left -= chunk;
		}
	}
	k->done = done;
	k->ctx = ctx;
	c->device = sdev;
	memcpy(c->cmnd, cdb, cdb_len);
	c->cmd_len = cdb_len;
	c->sc_data_direction = dir;
	c->sdb.table.sgl = k->sgl;
	c->sdb.table.nents = c->sdb.table.orig_nents = nr;
	c->sdb.length = len;
	c->resid_len = 0;
	c->sense_buffer = k->sense;
	c->allowed = 1;
	c->underflow = 0;
	c->result = 0;
	for (i = 0; i < 10000; i++) {		/* host busy (previous command still being retired): wait */
		ret = sh->hostt->queuecommand(sh, c);
		if (!ret)
			break;
		msleep(1);
	}
	if (ret) {
		/* SCSI_MLQUEUE_*_BUSY: report "device busy" to the caller */
		kfree(k->sgl);
		kfree(k);
		return -EBUSY;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(kpi_scsi_submit);

struct kpi_sync { struct completion done; int result; int resid; unsigned char sense[SCSI_SENSE_BUFFERSIZE]; };
static void kpi_sync_done(void *ctx, int result, int resid, const unsigned char *sense)
{
	struct kpi_sync *s = ctx;

	s->result = result;
	s->resid = resid;
	memcpy(s->sense, sense, sizeof(s->sense));
	complete(&s->done);
}
/* Synchronous internal command (scan). Returns the SCSI result word, or -1 on timeout/submit failure. */
static int kpi_exec(struct scsi_device *sdev, const unsigned char *cdb, int cdb_len, int dir, void *buf, unsigned int len)
{
	struct kpi_sync s;
	int ret;

	init_completion(&s.done);
	ret = kpi_scsi_submit(sdev, cdb, cdb_len, dir, buf, len, kpi_sync_done, &s);
	if (ret)
		return -1;
	if (!wait_for_completion_timeout(&s.done, 15 * HZ))
		return -1;
	return s.result;
}

/* ---- scanning ----------------------------------------------------------------------------------------- */
static void kpi_copy_field(char *dst, const unsigned char *src, int n)
{
	memcpy(dst, src, n);
}

static void kpi_scan_lun(struct Scsi_Host *sh, unsigned int id, u64 lun)
{
	struct scsi_device *sdev;
	unsigned char inq_cdb[6] = { INQUIRY, 0, 0, 0, 36, 0 };
	unsigned char tur[6] = { TEST_UNIT_READY, 0, 0, 0, 0, 0 };
	unsigned char *inq = kzalloc(64, GFP_KERNEL);
	int res, tries;

	if (!inq)
		return;
	sdev = kzalloc(sizeof(*sdev), GFP_KERNEL);
	if (!sdev) {
		kfree(inq);
		return;
	}
	sdev->host = sh;
	sdev->id = id;
	sdev->lun = lun;
	sdev->channel = 0;
	sdev->sdev_state = SDEV_RUNNING;
	sdev->queue_depth = sh->cmd_per_lun;
	sdev->type = -1;
	/* drivers poke the block queue limits from slave_alloc/slave_configure (blk_queue_*): give them somewhere to write */
	sdev->request_queue = kzalloc(sizeof(struct request_queue), GFP_KERNEL);
	if (!sdev->request_queue)
		goto drop;
	mutex_init(&sdev->request_queue->limits_lock);
	sdev->scsi_level = SCSI_2;
	sdev->use_10_for_rw = 1;
	INIT_LIST_HEAD(&sdev->siblings);
	INIT_LIST_HEAD(&sdev->same_target_siblings);
	device_initialize(&sdev->sdev_gendev);
	sdev->sdev_gendev.parent = &sh->shost_gendev;
	dev_set_name(&sdev->sdev_gendev, "%d:0:%u:%llu", sh->host_no, id, (unsigned long long)lun);
	if (sh->hostt->slave_alloc && sh->hostt->slave_alloc(sdev))
		goto drop;
	/* a few TEST UNIT READYs let a device that is still spinning up wake (errors are ignored) */
	for (tries = 0; tries < 2; tries++)
		kpi_exec(sdev, tur, 6, DMA_NONE, NULL, 0);
	res = kpi_exec(sdev, inq_cdb, 6, DMA_FROM_DEVICE, inq, 36);
	if (res != 0 || (inq[0] & 0xe0) == 0x20 || (inq[0] & 0x1f) == 0x1f) {
		pr_info("kpi6_scsi: %d:0:%u:%llu: no device (result %x)\n", sh->host_no, id, (unsigned long long)lun, res);
		if (sh->hostt->slave_destroy)
			sh->hostt->slave_destroy(sdev);
		goto drop;
	}
	sdev->type = inq[0] & 0x1f;
	sdev->removable = !!(inq[1] & 0x80);
	sdev->scsi_level = inq[2] & 7;
	sdev->vendor = (const char *)kmemdup(inq + 8, 9, GFP_KERNEL);
	sdev->model = (const char *)kmemdup(inq + 16, 17, GFP_KERNEL);
	sdev->rev = (const char *)kmemdup(inq + 32, 5, GFP_KERNEL);
	if (sh->hostt->slave_configure && sh->hostt->slave_configure(sdev))
		pr_info("kpi6_scsi: slave_configure failed\n");
	pr_info("kpi6_scsi: %d:0:%u:%llu: type %u '%.8s' '%.16s' '%.4s'%s\n", sh->host_no, id, (unsigned long long)lun,
		sdev->type, sdev->vendor, sdev->model, sdev->rev, sdev->removable ? " removable" : "");
	kfree(inq);
	if (kpi_scsi_hook)
		kpi_scsi_hook(sdev, 1);
	return;
drop:
	kfree(inq);
	kfree(sdev);
}

void scsi_scan_host(struct Scsi_Host *sh)
{
	unsigned int id;
	u64 lun;

	for (id = 0; id < sh->max_id; id++) {
		if ((int)id == sh->this_id)
			continue;
		for (lun = 0; lun < sh->max_lun; lun++)
			kpi_scan_lun(sh, id, lun);
	}
}
void scsi_remove_host(struct Scsi_Host *sh) { }
void scsi_report_bus_reset(struct Scsi_Host *sh, int channel) { }
void scsi_report_device_reset(struct Scsi_Host *sh, int channel, int target) { }

/* host-side accessors (kpi_scsi_hook receives the scsi_device) */
void kpi_scsi_set_hook(void (*hook)(struct scsi_device *sdev, int added)) { kpi_scsi_hook = hook; }
EXPORT_SYMBOL_GPL(kpi_scsi_set_hook);
int kpi_scsi_dev_type(struct scsi_device *sdev) { return sdev->type; }
int kpi_scsi_dev_removable(struct scsi_device *sdev) { return sdev->removable; }
const char *kpi_scsi_dev_vendor(struct scsi_device *sdev) { return sdev->vendor; }
const char *kpi_scsi_dev_model(struct scsi_device *sdev) { return sdev->model; }
const char *kpi_scsi_dev_rev(struct scsi_device *sdev) { return sdev->rev; }
unsigned int kpi_scsi_dev_max_bytes(struct scsi_device *sdev) { return sdev->host->max_sectors * 512; }

/* ---- error handling helpers ----------------------------------------------------------------------------- */
void scsi_eh_prep_cmnd(struct scsi_cmnd *scmd, struct scsi_eh_save *ses, unsigned char *cmnd, int cmnd_size, unsigned sense_bytes)
{
	struct scsi_device *sdev = scmd->device;

	/* same state save/restore as the real mid-layer (drivers/scsi/scsi_error.c) */
	ses->cmd_len = scmd->cmd_len;
	memcpy(ses->cmnd, scmd->cmnd, sizeof(ses->cmnd));
	ses->data_direction = scmd->sc_data_direction;
	ses->sdb = scmd->sdb;
	ses->result = scmd->result;
	ses->resid_len = scmd->resid_len;
	ses->underflow = scmd->underflow;
	ses->prot_op = scmd->prot_op;
	ses->eh_eflags = scmd->eh_eflags;

	scmd->prot_op = SCSI_PROT_NORMAL;
	scmd->eh_eflags = 0;
	memset(&scmd->sdb, 0, sizeof(scmd->sdb));
	scmd->result = 0;
	if (sense_bytes) {
		scmd->sdb.length = min_t(unsigned, SCSI_SENSE_BUFFERSIZE, sense_bytes);
		sg_init_one(&ses->sense_sgl, scmd->sense_buffer, scmd->sdb.length);
		scmd->sdb.table.sgl = &ses->sense_sgl;
		scmd->sc_data_direction = DMA_FROM_DEVICE;
		scmd->sdb.table.nents = scmd->sdb.table.orig_nents = 1;
		memset(scmd->cmnd, 0, sizeof(scmd->cmnd));
		scmd->cmnd[0] = REQUEST_SENSE;
		scmd->cmnd[4] = scmd->sdb.length;
		scmd->cmd_len = COMMAND_SIZE(scmd->cmnd[0]);
	} else {
		scmd->sc_data_direction = DMA_NONE;
		if (cmnd) {
			memset(scmd->cmnd, 0, sizeof(scmd->cmnd));
			memcpy(scmd->cmnd, cmnd, cmnd_size);
			scmd->cmd_len = COMMAND_SIZE(scmd->cmnd[0]);
		}
	}
	scmd->underflow = 0;
	if (sdev->scsi_level <= SCSI_2 && sdev->scsi_level != SCSI_UNKNOWN)
		scmd->cmnd[1] = (scmd->cmnd[1] & 0x1f) | (sdev->lun << 5 & 0xe0);
	/* the sense buffer is the data buffer of the REQUEST SENSE: clear it */
	memset(scmd->sense_buffer, 0, SCSI_SENSE_BUFFERSIZE);
}
void scsi_eh_restore_cmnd(struct scsi_cmnd *scmd, struct scsi_eh_save *ses)
{
	scmd->cmd_len = ses->cmd_len;
	memcpy(scmd->cmnd, ses->cmnd, sizeof(scmd->cmnd));
	scmd->sc_data_direction = ses->data_direction;
	scmd->sdb = ses->sdb;
	scmd->result = ses->result;
	scmd->resid_len = ses->resid_len;
	scmd->underflow = ses->underflow;
	scmd->prot_op = ses->prot_op;
	scmd->eh_eflags = ses->eh_eflags;
}

const u8 *scsi_sense_desc_find(const u8 *sense_buffer, int sb_len, int desc_type)
{
	int add_sen_len, add_len, desc_len, k;
	const u8 *descp;

	if (sb_len < 8 || (sense_buffer[0] & 0x7f) < 0x72)
		return NULL;
	add_sen_len = sb_len < 8 ? 0 : sense_buffer[7];
	add_sen_len = (add_sen_len < (sb_len - 8)) ? add_sen_len : (sb_len - 8);
	descp = &sense_buffer[8];
	for (desc_len = 0, k = 0; k < add_sen_len; k += desc_len) {
		descp += desc_len;
		add_len = (k < (add_sen_len - 1)) ? descp[1] : -1;
		desc_len = add_len + 2;
		if (descp[0] == desc_type)
			return descp;
		if (add_len < 0)
			break;
	}
	return NULL;
}
bool scsi_normalize_sense(const u8 *sense_buffer, int sb_len, struct scsi_sense_hdr *sshdr)
{
	memset(sshdr, 0, sizeof(*sshdr));
	if (!sense_buffer || !sb_len)
		return false;
	sshdr->response_code = sense_buffer[0] & 0x7f;
	if (!scsi_sense_valid(sshdr))
		return false;
	if (sshdr->response_code >= 0x72) {
		if (sb_len > 1)
			sshdr->sense_key = sense_buffer[1] & 0xf;
		if (sb_len > 2)
			sshdr->asc = sense_buffer[2];
		if (sb_len > 3)
			sshdr->ascq = sense_buffer[3];
		if (sb_len > 7)
			sshdr->additional_length = sense_buffer[7];
	} else {
		if (sb_len > 2)
			sshdr->sense_key = sense_buffer[2] & 0xf;
		if (sb_len > 7) {
			sb_len = (sb_len < (sense_buffer[7] + 8)) ? sb_len : (sense_buffer[7] + 8);
			if (sb_len > 12)
				sshdr->asc = sense_buffer[12];
			if (sb_len > 13)
				sshdr->ascq = sense_buffer[13];
		}
	}
	return true;
}

u64 int_pow(u64 base, unsigned int exp)
{
	u64 result = 1;

	while (exp) {
		if (exp & 1)
			result *= base;
		exp >>= 1;
		base *= base;
	}
	return result;
}
static int __init kpi6_init_dummy(void) { return 0; }

const unsigned char scsi_command_size_tbl[8] = { 6, 10, 10, 12, 16, 12, 10, 10 };
EXPORT_SYMBOL(scsi_command_size_tbl);
