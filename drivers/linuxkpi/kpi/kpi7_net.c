// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI part 7: a minimal network core, enough for PCI Ethernet drivers (e1000e, igb, igc, r8169).
 *
 * There is no Linux protocol stack: every net_device registered by a driver is reported to the host
 * (kpi_net_set_hook), which exposes it as an NDIS miniport. Frames go straight between the driver and the host:
 *   TX: host -> kpi_net_xmit() -> skb -> ndo_start_xmit()
 *   RX: driver NAPI poll -> napi_gro_receive()/netif_rx() -> host rx callback (whole Ethernet frame)
 * NAPI polls run in a host DPC (the NT counterpart of the NET_RX softirq); link changes are handed to the host from a
 * work item, after the link speed has been read through ethtool (as Linux's linkwatch does, outside driver locks).
 *
 * Address lists (dev_addr_*, dev_uc_*, dev_mc_*, __hw_addr_sync_dev) come from net/core/dev_addr_lists.c, BQL from
 * lib/dynamic_queue_limits.c, both compiled unmodified into this module (see build2.sh).
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/skbuff.h>
#include <linux/skbuff_ref.h>
#include <linux/if_vlan.h>
#include <linux/rtnetlink.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/pm_qos.h>
#include <linux/pm_runtime.h>
#include <linux/timekeeping.h>
#include <linux/crc32.h>
#include <linux/i2c.h>
#include <linux/phy.h>
#include <linux/ethtool_netlink.h>
#include <linux/fwnode.h>
#include <linux/property.h>
#include <linux/acpi.h>
#include <net/netdev_rx_queue.h>
#include <net/checksum.h>
#include <net/ip6_checksum.h>
#include <net/xdp.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>
#include <linux/filter.h>
#include <trace/events/xdp.h>
#include <trace/events/napi.h>
#include "dev.h"	/* net/core/dev.h: dev_addr_init(), dev_addr_flush() */

/* net/core/xdp.c */
#define REG_STATE_REGISTERED	0x1
#define REG_STATE_UNREGISTERED	0x2

extern void nt_dbgprint(const char *s);
extern void *nt_dpc_alloc(void (*fn)(void *), void *ctx);
extern int nt_dpc_queue(void *dpc);
extern unsigned long long nt_ktime_ns(void);
extern unsigned long long nt_real_ns(void);

/* ---- data objects the drivers reference ------------------------------------------------------------------- */
DEFINE_PER_CPU_ALIGNED(struct softnet_data, softnet_data);
enum system_states system_state = SYSTEM_RUNNING;
DEFINE_STATIC_KEY_FALSE(bpf_master_redirect_enabled_key);
DEFINE_STATIC_KEY_FALSE(bpf_stats_enabled_key);
const struct phylib_stubs *phylib_stubs;
/* the xdp_exception tracepoint (never enabled: nothing registers probes) */
DEFINE_TRACE(dql_stall_detected,
	     TP_PROTO(unsigned short thrs, unsigned int len, unsigned long last_reap, unsigned long hist_head,
		      unsigned long now, unsigned long *hist),
	     TP_ARGS(thrs, len, last_reap, hist_head, now, hist));
DEFINE_TRACE(xdp_exception, TP_PROTO(const struct net_device *dev, const struct bpf_prog *xdp, u32 act), TP_ARGS(dev, xdp, act));

/* ---- host bridge -------------------------------------------------------------------------------------------- */
struct kpi_net_host_ops {
	void (*rx)(void *ctx, const void *frame, unsigned int len);
	void (*link)(void *ctx, int up, unsigned int mbps);
	void (*tx_wake)(void *ctx);
};

struct kpi_net_info {
	unsigned char mac[8];
	unsigned int mtu;
	unsigned int link;
	unsigned int mbps;
	char driver[32];
	char ifname[16];
};

#define KPI_NET_MAX 8
#define KPI_NAPI_MAX 64
#define KPI_RX_FRAME_MAX (ETH_FRAME_LEN + VLAN_HLEN + ETH_FCS_LEN)

struct kpi_netdev {
	struct net_device *dev;
	const struct kpi_net_host_ops *ops;
	void *ctx;
	unsigned int mbps;
	struct work_struct link_work;
	unsigned char *rxbuf;		/* netif_rx() (no NAPI context) bounce buffer, under rx_lock */
	spinlock_t rx_lock;
};
static struct kpi_netdev kpi_nets[KPI_NET_MAX];
static DEFINE_SPINLOCK(kpi_nets_lock);
static void (*kpi_net_hook)(struct net_device *dev, int added);
static atomic_t kpi_ifindex = ATOMIC_INIT(0);

struct kpi_napi {
	struct napi_struct *napi;
	void *dpc;
	unsigned char *rxbuf;		/* linearised frames of this NAPI context (its poll never runs concurrently) */
};
static struct kpi_napi kpi_napis[KPI_NAPI_MAX];
static DEFINE_SPINLOCK(kpi_napi_lock);

static struct kpi_netdev *kpi_netdev_of(const struct net_device *dev)
{
	int i;

	for (i = 0; i < KPI_NET_MAX; i++)
		if (kpi_nets[i].dev == dev)
			return &kpi_nets[i];
	return NULL;
}

static struct kpi_napi *kpi_napi_of(const struct napi_struct *n)
{
	unsigned int idx = n->napi_id;

	return idx && idx <= KPI_NAPI_MAX && kpi_napis[idx - 1].napi == n ? &kpi_napis[idx - 1] : NULL;
}

/* ---- sk_buff ------------------------------------------------------------------------------------------------ */
static void kpi_finalize_skb_around(struct sk_buff *skb, void *data, unsigned int size)
{
	struct skb_shared_info *shinfo;

	size -= SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
	skb->truesize = SKB_TRUESIZE(size);
	refcount_set(&skb->users, 1);
	skb->head = data;
	skb->data = data;
	skb_reset_tail_pointer(skb);
	skb_set_end_offset(skb, size);
	skb->mac_header = (typeof(skb->mac_header))~0U;
	skb->transport_header = (typeof(skb->transport_header))~0U;
	shinfo = skb_shinfo(skb);
	memset(shinfo, 0, offsetof(struct skb_shared_info, dataref));
	atomic_set(&shinfo->dataref, 1);
}

struct sk_buff *__alloc_skb(unsigned int size, gfp_t gfp, int flags, int node)
{
	struct sk_buff *skb = kzalloc(sizeof(*skb), gfp & ~__GFP_DMA);
	unsigned int total;
	u8 *data;

	if (!skb)
		return NULL;
	total = SKB_DATA_ALIGN(size) + SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
	data = kmalloc(total, gfp & ~__GFP_DMA);
	if (!data) {
		kfree(skb);
		return NULL;
	}
	kpi_finalize_skb_around(skb, data, total);
	return skb;
}
EXPORT_SYMBOL(__alloc_skb);

struct sk_buff *__netdev_alloc_skb(struct net_device *dev, unsigned int len, gfp_t gfp)
{
	struct sk_buff *skb = __alloc_skb(len + NET_SKB_PAD, gfp, 0, NUMA_NO_NODE);

	if (!skb)
		return NULL;
	skb_reserve(skb, NET_SKB_PAD);
	skb->dev = dev;
	return skb;
}
EXPORT_SYMBOL(__netdev_alloc_skb);

struct sk_buff *napi_alloc_skb(struct napi_struct *napi, unsigned int len)
{
	struct sk_buff *skb = __alloc_skb(len + NET_SKB_PAD + NET_IP_ALIGN, GFP_ATOMIC, 0, NUMA_NO_NODE);

	if (!skb)
		return NULL;
	skb_reserve(skb, NET_SKB_PAD + NET_IP_ALIGN);
	skb->dev = napi->dev;
	return skb;
}
EXPORT_SYMBOL(napi_alloc_skb);

/* data is a page fragment of frag_size bytes (the driver's receive buffer): the skb takes over one page reference */
struct sk_buff *napi_build_skb(void *data, unsigned int frag_size)
{
	struct sk_buff *skb = kzalloc(sizeof(*skb), GFP_ATOMIC);

	if (!skb)
		return NULL;
	kpi_finalize_skb_around(skb, data, frag_size);
	skb->head_frag = 1;
	return skb;
}
EXPORT_SYMBOL(napi_build_skb);

static void kpi_skb_free_head(struct sk_buff *skb)
{
	if (skb->head_frag)
		put_page(virt_to_page(skb->head));
	else
		kfree(skb->head);
}

static void kpi_skb_free(struct sk_buff *skb)
{
	struct skb_shared_info *sh;
	int i;

	if (!skb)
		return;
	if (!refcount_dec_and_test(&skb->users))
		return;
	if (skb->destructor)
		skb->destructor(skb);
	sh = skb_shinfo(skb);
	for (i = 0; i < sh->nr_frags; i++)
		skb_frag_unref(skb, i);
	kpi_skb_free_head(skb);
	kfree(skb);
}

void consume_skb(struct sk_buff *skb) { kpi_skb_free(skb); }
EXPORT_SYMBOL(consume_skb);
void __kfree_skb(struct sk_buff *skb) { kpi_skb_free(skb); }
EXPORT_SYMBOL(__kfree_skb);
void sk_skb_reason_drop(struct sock *sk, struct sk_buff *skb, enum skb_drop_reason reason) { kpi_skb_free(skb); }
EXPORT_SYMBOL(sk_skb_reason_drop);
void dev_kfree_skb_any_reason(struct sk_buff *skb, enum skb_drop_reason reason) { kpi_skb_free(skb); }
EXPORT_SYMBOL(dev_kfree_skb_any_reason);
void dev_kfree_skb_irq_reason(struct sk_buff *skb, enum skb_drop_reason reason) { kpi_skb_free(skb); }
EXPORT_SYMBOL(dev_kfree_skb_irq_reason);
void napi_consume_skb(struct sk_buff *skb, int budget) { kpi_skb_free(skb); }
EXPORT_SYMBOL(napi_consume_skb);

void *skb_put(struct sk_buff *skb, unsigned int len)
{
	void *tmp = skb_tail_pointer(skb);

	SKB_LINEAR_ASSERT(skb);
	skb->tail += len;
	skb->len += len;
	if (unlikely(skb->tail > skb->end)) {
		pr_err("kpi7_net: skb_put over the end (len %u, size %u)\n", skb->len, skb_end_offset(skb));
		BUG();
	}
	return tmp;
}
EXPORT_SYMBOL(skb_put);

void skb_trim(struct sk_buff *skb, unsigned int len)
{
	if (skb->len > len)
		__skb_trim(skb, len);
}
EXPORT_SYMBOL(skb_trim);

void skb_add_rx_frag_netmem(struct sk_buff *skb, int i, netmem_ref netmem, int off, int size, unsigned int truesize)
{
	skb_fill_netmem_desc(skb, i, netmem, off, size);
	skb->len += size;
	skb->data_len += size;
	skb->truesize += truesize;
}
EXPORT_SYMBOL(skb_add_rx_frag_netmem);

int skb_copy_bits(const struct sk_buff *skb, int offset, void *to, int len)
{
	int start = skb_headlen(skb), i, copy;

	if (offset < 0 || offset > (int)skb->len - len)
		return -EFAULT;
	copy = start - offset;
	if (copy > 0) {
		if (copy > len)
			copy = len;
		memcpy(to, skb->data + offset, copy);
		len -= copy;
		if (!len)
			return 0;
		offset += copy;
		to += copy;
	}
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		const skb_frag_t *f = &skb_shinfo(skb)->frags[i];
		int end = start + skb_frag_size(f);

		copy = end - offset;
		if (copy > 0) {
			if (copy > len)
				copy = len;
			memcpy(to, (u8 *)skb_frag_address(f) + offset - start, copy);
			len -= copy;
			if (!len)
				return 0;
			offset += copy;
			to += copy;
		}
		start = end;
	}
	return len ? -EFAULT : 0;
}
EXPORT_SYMBOL(skb_copy_bits);

/* Re-allocate the linear part with nhead/ntail more bytes (the skb must not be shared or cloned: none are here). */
int pskb_expand_head(struct sk_buff *skb, int nhead, int ntail, gfp_t gfp)
{
	unsigned int osize = skb_end_offset(skb);
	unsigned int size = SKB_DATA_ALIGN(osize + nhead + ntail);
	unsigned int total = size + SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
	long off;
	u8 *data;

	BUG_ON(nhead < 0);
	data = kmalloc(total, gfp & ~__GFP_DMA);
	if (!data)
		return -ENOMEM;
	memcpy(data + nhead, skb->head, skb_tail_pointer(skb) - skb->head);
	memcpy(data + size, skb_shinfo(skb), offsetof(struct skb_shared_info, frags[skb_shinfo(skb)->nr_frags]));
	kpi_skb_free_head(skb);
	off = (data + nhead) - skb->head;
	skb->head = data;
	skb->head_frag = 0;
	skb->data += off;
	skb_set_end_offset(skb, size);
	skb->tail += nhead;		/* NET_SKBUFF_DATA_USES_OFFSET: offsets move by nhead only */
	skb_headers_offset_update(skb, nhead);
	skb->cloned = 0;
	skb->hdr_len = 0;
	skb->nohdr = 0;
	atomic_set(&skb_shinfo(skb)->dataref, 1);
	skb->truesize += size - osize;
	return 0;
}
EXPORT_SYMBOL(pskb_expand_head);

/* Move delta bytes from the fragments into the linear part. */
void *__pskb_pull_tail(struct sk_buff *skb, int delta)
{
	struct skb_shared_info *sh;
	int eat = (skb->tail + delta) - skb->end, i, k, n;

	if (eat > 0 && pskb_expand_head(skb, 0, eat + 128, GFP_ATOMIC))
		return NULL;
	if (skb_copy_bits(skb, skb_headlen(skb), skb_tail_pointer(skb), delta))
		BUG();
	sh = skb_shinfo(skb);
	n = sh->nr_frags;
	eat = delta;
	for (i = 0; i < n && eat; i++) {
		int size = skb_frag_size(&sh->frags[i]);

		if (size > eat)
			break;
		skb_frag_unref(skb, i);
		eat -= size;
	}
	for (k = 0; i < n; i++, k++) {
		sh->frags[k] = sh->frags[i];
		if (eat) {
			skb_frag_off_add(&sh->frags[k], eat);
			skb_frag_size_sub(&sh->frags[k], eat);
			eat = 0;
		}
	}
	sh->nr_frags = k;
	skb->tail += delta;
	skb->data_len -= delta;
	return skb_tail_pointer(skb);
}
EXPORT_SYMBOL(__pskb_pull_tail);

int ___pskb_trim(struct sk_buff *skb, unsigned int len)
{
	struct skb_shared_info *sh = skb_shinfo(skb);
	unsigned int offset = skb_headlen(skb);
	int i, keep = 0;

	if (offset >= len) {
		for (i = 0; i < sh->nr_frags; i++)
			skb_frag_unref(skb, i);
		sh->nr_frags = 0;
		skb->data_len = 0;
		skb->len = len;
		skb_set_tail_pointer(skb, len);
		return 0;
	}
	for (i = 0; i < sh->nr_frags; i++) {
		unsigned int end = offset + skb_frag_size(&sh->frags[i]);

		if (offset >= len) {
			skb_frag_unref(skb, i);
			continue;
		}
		if (end > len)
			skb_frag_size_set(&sh->frags[i], len - offset);
		keep = i + 1;
		offset = end;
	}
	sh->nr_frags = keep;
	skb->data_len -= skb->len - len;
	skb->len = len;
	return 0;
}
EXPORT_SYMBOL(___pskb_trim);

int __skb_pad(struct sk_buff *skb, int pad, bool free_on_error)
{
	int err;

	if (!skb_cloned(skb) && skb_tailroom(skb) >= pad) {
		memset(skb->data + skb->len, 0, pad);
		return 0;
	}
	err = pskb_expand_head(skb, 0, skb->data_len + pad, GFP_ATOMIC);
	if (!err)
		err = skb_linearize(skb);
	if (err) {
		if (free_on_error)
			kpi_skb_free(skb);
		return err;
	}
	memset(skb->data + skb->len, 0, pad);
	return 0;
}
EXPORT_SYMBOL(__skb_pad);

/* Software checksum for a CHECKSUM_PARTIAL skb (the host never sends those, the drivers may still call it). */
int skb_checksum_help(struct sk_buff *skb)
{
	int offset, ret;
	__wsum csum;

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;
	if (skb_is_nonlinear(skb) && skb_linearize(skb))
		return -ENOMEM;
	offset = skb_checksum_start_offset(skb);
	ret = -EINVAL;
	if (offset < 0 || offset + skb->csum_offset + 2 > (int)skb_headlen(skb))
		return ret;
	csum = csum_partial(skb->data + offset, skb->len - offset, 0);
	*(__sum16 *)(skb->data + offset + skb->csum_offset) = csum_fold(csum) ?: CSUM_MANGLED_0;
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}
EXPORT_SYMBOL(skb_checksum_help);

void skb_headers_offset_update(struct sk_buff *skb, int off)
{
	if (skb->ip_summed == CHECKSUM_PARTIAL)
		skb->csum_start += off;
	skb->transport_header += off;
	skb->network_header += off;
	if (skb_mac_header_was_set(skb))
		skb->mac_header += off;
	skb->inner_transport_header += off;
	skb->inner_network_header += off;
	skb->inner_mac_header += off;
}
EXPORT_SYMBOL(skb_headers_offset_update);

void skb_tstamp_tx(struct sk_buff *orig_skb, struct skb_shared_hwtstamps *hwtstamps) { }
EXPORT_SYMBOL(skb_tstamp_tx);

void __page_frag_cache_drain(struct page *page, unsigned int count)
{
	if (page_ref_sub_and_test(page, count))
		__free_pages(page, page->private);
}
EXPORT_SYMBOL(__page_frag_cache_drain);

/* x86 DMA is cache coherent and the host maps buffers without bouncing: nothing to synchronise */
void __dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr, size_t size, enum dma_data_direction dir) { }
EXPORT_SYMBOL(__dma_sync_single_for_cpu);
void __dma_sync_single_for_device(struct device *dev, dma_addr_t addr, size_t size, enum dma_data_direction dir) { }
EXPORT_SYMBOL(__dma_sync_single_for_device);

/* per-CPU data: the modules are built for one CPU (CONFIG_SMP=n), a per-CPU object is a single zeroed object */
void __percpu *pcpu_alloc_noprof(size_t size, size_t align, bool reserved, gfp_t gfp)
{
	return (void __percpu *)kzalloc(max_t(size_t, size, align), gfp | __GFP_ZERO);
}
EXPORT_SYMBOL(pcpu_alloc_noprof);
void free_percpu(void __percpu *ptr) { kfree((void __force *)ptr); }
EXPORT_SYMBOL(free_percpu);
void *vzalloc_node_noprof(unsigned long size, int node) { return kvzalloc(size, GFP_KERNEL); }
EXPORT_SYMBOL(vzalloc_node_noprof);

/* ---- checksums / CRC ---------------------------------------------------------------------------------------- */
__wsum csum_partial(const void *buff, int len, __wsum wsum)
{
	const u8 *p = buff;
	u64 sum = (__force u32)wsum;

	while (len >= 2) {
		sum += *(const u16 *)p;
		p += 2;
		len -= 2;
	}
	if (len)
		sum += *p;		/* little endian: the odd byte is the low byte of its 16-bit word */
	while (sum >> 32)
		sum = (sum & 0xffffffff) + (sum >> 32);
	return (__force __wsum)sum;
}
EXPORT_SYMBOL(csum_partial);

__sum16 csum_ipv6_magic(const struct in6_addr *saddr, const struct in6_addr *daddr, __u32 len, __u8 proto, __wsum csum)
{
	u64 sum = (__force u32)csum;
	int i;

	for (i = 0; i < 4; i++)
		sum += (__force u32)saddr->s6_addr32[i] + (u64)(__force u32)daddr->s6_addr32[i];
	sum += (__force u32)htonl(len);
	sum += (__force u32)htonl(proto);
	while (sum >> 32)
		sum = (sum & 0xffffffff) + (sum >> 32);
	return csum_fold((__force __wsum)sum);
}
EXPORT_SYMBOL(csum_ipv6_magic);

u32 __pure crc32_le(u32 crc, unsigned char const *p, size_t len)
{
	int k;

	while (len--) {
		crc ^= *p++;
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
	}
	return crc;
}
EXPORT_SYMBOL(crc32_le);

/* ---- Ethernet helpers --------------------------------------------------------------------------------------- */
__be16 eth_type_trans(struct sk_buff *skb, struct net_device *dev)
{
	const struct ethhdr *eth;

	skb->dev = dev;
	skb_reset_mac_header(skb);
	eth = (struct ethhdr *)skb->data;
	skb_pull_inline(skb, ETH_HLEN);
	if (unlikely(!ether_addr_equal_64bits(eth->h_dest, dev->dev_addr))) {
		if (is_multicast_ether_addr_64bits(eth->h_dest))
			skb->pkt_type = ether_addr_equal_64bits(eth->h_dest, dev->broadcast) ? PACKET_BROADCAST : PACKET_MULTICAST;
		else
			skb->pkt_type = PACKET_OTHERHOST;
	}
	if (likely(eth_proto_is_802_3(eth->h_proto)))
		return eth->h_proto;
	if (skb->len >= 2 && *(const u16 *)skb->data == 0xFFFF)
		return htons(ETH_P_802_3);
	return htons(ETH_P_802_2);
}
EXPORT_SYMBOL(eth_type_trans);

/* How many bytes of a received frame the driver should copy into the linear area: the Ethernet header is enough
 * (eth_type_trans() pulls it), the rest may stay in page fragments. */
u32 eth_get_headlen(const struct net_device *dev, const void *data, u32 len)
{
	return min_t(u32, len, ETH_HLEN);
}
EXPORT_SYMBOL(eth_get_headlen);

int eth_validate_addr(struct net_device *dev)
{
	return is_valid_ether_addr(dev->dev_addr) ? 0 : -EADDRNOTAVAIL;
}
EXPORT_SYMBOL(eth_validate_addr);

int eth_mac_addr(struct net_device *dev, void *p)
{
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;
	eth_hw_addr_set(dev, addr->sa_data);
	return 0;
}
EXPORT_SYMBOL(eth_mac_addr);

int eth_platform_get_mac_address(struct device *dev, u8 *mac_addr) { return -ENODEV; }
EXPORT_SYMBOL(eth_platform_get_mac_address);

static void kpi_ether_setup(struct net_device *dev)
{
	dev->type = ARPHRD_ETHER;
	dev->hard_header_len = ETH_HLEN;
	dev->min_header_len = ETH_HLEN;
	dev->mtu = ETH_DATA_LEN;
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = ETH_DATA_LEN;
	dev->addr_len = ETH_ALEN;
	dev->tx_queue_len = DEFAULT_TX_QUEUE_LEN;
	dev->flags = IFF_BROADCAST | IFF_MULTICAST;
	dev->priv_flags |= IFF_TX_SKB_SHARING;
	eth_broadcast_addr(dev->broadcast);
}

/* ---- net_device ----------------------------------------------------------------------------------------------- */
struct net_device *alloc_etherdev_mqs(int sizeof_priv, unsigned int txqs, unsigned int rxqs)
{
	struct net_device *dev;
	unsigned int i;

	if (!txqs || !rxqs)
		return NULL;
	dev = kvzalloc(struct_size(dev, priv, sizeof_priv), GFP_KERNEL);
	if (!dev)
		return NULL;
	dev->priv_len = sizeof_priv;
	refcount_set(&dev->dev_refcnt, 1);
	if (dev_addr_init(dev))
		goto fail;
	dev_mc_init(dev);
	dev_uc_init(dev);
	dev->gso_max_size = GSO_LEGACY_MAX_SIZE;
	dev->gso_max_segs = GSO_MAX_SEGS;
	dev->gro_max_size = GRO_LEGACY_MAX_SIZE;
	dev->gso_ipv4_max_size = GSO_LEGACY_MAX_SIZE;
	dev->gro_ipv4_max_size = GRO_LEGACY_MAX_SIZE;
	dev->tso_max_size = TSO_LEGACY_MAX_SIZE;
	dev->tso_max_segs = TSO_MAX_SEGS;
	dev->upper_level = 1;
	dev->lower_level = 1;
	INIT_LIST_HEAD(&dev->napi_list);
	INIT_LIST_HEAD(&dev->unreg_list);
	INIT_LIST_HEAD(&dev->close_list);
	INIT_LIST_HEAD(&dev->link_watch_list);
	INIT_LIST_HEAD(&dev->adj_list.upper);
	INIT_LIST_HEAD(&dev->adj_list.lower);
	INIT_LIST_HEAD(&dev->ptype_all);
	INIT_LIST_HEAD(&dev->ptype_specific);
	dev->priv_flags = IFF_XMIT_DST_RELEASE | IFF_XMIT_DST_RELEASE_PERM;
	kpi_ether_setup(dev);
	dev->num_tx_queues = dev->real_num_tx_queues = txqs;
	dev->_tx = kcalloc(txqs, sizeof(*dev->_tx), GFP_KERNEL);
	dev->num_rx_queues = dev->real_num_rx_queues = rxqs;
	dev->_rx = kcalloc(rxqs, sizeof(*dev->_rx), GFP_KERNEL);
	dev->ethtool = kzalloc(sizeof(*dev->ethtool), GFP_KERNEL);
	if (!dev->_tx || !dev->_rx || !dev->ethtool)
		goto fail;
	for (i = 0; i < txqs; i++) {
		struct netdev_queue *q = &dev->_tx[i];

		q->dev = dev;
		spin_lock_init(&q->_xmit_lock);
		q->xmit_lock_owner = -1;
		dql_init(&q->dql, HZ);
	}
	for (i = 0; i < rxqs; i++)
		dev->_rx[i].dev = dev;
	spin_lock_init(&dev->addr_list_lock);
	strscpy(dev->name, "eth%d", sizeof(dev->name));
	return dev;
fail:
	free_netdev(dev);
	return NULL;
}
EXPORT_SYMBOL(alloc_etherdev_mqs);

static void devm_free_netdev(struct device *dev, void *res) { free_netdev(*(struct net_device **)res); }

struct net_device *devm_alloc_etherdev_mqs(struct device *dev, int sizeof_priv, unsigned int txqs, unsigned int rxqs)
{
	struct net_device **dr = devres_alloc(devm_free_netdev, sizeof(*dr), GFP_KERNEL);
	struct net_device *ndev;

	if (!dr)
		return NULL;
	ndev = alloc_etherdev_mqs(sizeof_priv, txqs, rxqs);
	if (!ndev) {
		devres_free(dr);
		return NULL;
	}
	*dr = ndev;
	devres_add(dev, dr);
	return ndev;
}
EXPORT_SYMBOL(devm_alloc_etherdev_mqs);

void free_netdev(struct net_device *dev)
{
	if (!dev)
		return;
	dev_addr_flush(dev);
	kfree(dev->_tx);
	kfree(dev->_rx);
	kfree(dev->ethtool);
	free_percpu(dev->tstats);
	kvfree(dev);
}
EXPORT_SYMBOL(free_netdev);

static DEFINE_MUTEX(kpi_rtnl);
void rtnl_lock(void) { mutex_lock(&kpi_rtnl); }
EXPORT_SYMBOL(rtnl_lock);
void rtnl_unlock(void) { mutex_unlock(&kpi_rtnl); }
EXPORT_SYMBOL(rtnl_unlock);
int rtnl_is_locked(void) { return mutex_is_locked(&kpi_rtnl); }
EXPORT_SYMBOL(rtnl_is_locked);
void synchronize_net(void) { synchronize_rcu(); }
EXPORT_SYMBOL(synchronize_net);

int register_netdev(struct net_device *dev)
{
	struct kpi_netdev *k = NULL;
	unsigned long flags;
	int i, idx;

	spin_lock_irqsave(&kpi_nets_lock, flags);
	for (i = 0; i < KPI_NET_MAX; i++)
		if (!kpi_nets[i].dev) {
			k = &kpi_nets[i];
			k->dev = dev;
			break;
		}
	spin_unlock_irqrestore(&kpi_nets_lock, flags);
	if (!k)
		return -ENOSPC;
	idx = atomic_inc_return(&kpi_ifindex) - 1;
	if (strchr(dev->name, '%'))
		snprintf(dev->name, sizeof(dev->name), "eth%d", idx);
	dev->ifindex = idx + 1;
	if (dev->pcpu_stat_type == NETDEV_PCPU_STAT_TSTATS) {
		dev->tstats = __alloc_percpu(sizeof(struct pcpu_sw_netstats), 8);
		if (!dev->tstats)
			goto fail;
	} else if (dev->pcpu_stat_type == NETDEV_PCPU_STAT_LSTATS) {
		dev->lstats = __alloc_percpu(sizeof(struct pcpu_lstats), 8);
		if (!dev->lstats)
			goto fail;
	}
	/*
	 * The host hands over whole frames: keep VLAN tags in the frame and do not ask for receive coalescing.
	 * Transmit offloads stay as they are; the host only sends linear frames without checksum offload.
	 */
	dev->features &= ~(NETIF_F_HW_VLAN_CTAG_RX | NETIF_F_HW_VLAN_STAG_RX | NETIF_F_LRO | NETIF_F_GRO_HW);
	dev->wanted_features = dev->features;
	if (!dev->tx_queue_len)
		dev->tx_queue_len = DEFAULT_TX_QUEUE_LEN;
	k->rxbuf = kmalloc(KPI_RX_FRAME_MAX, GFP_KERNEL);
	spin_lock_init(&k->rx_lock);
	if (!k->rxbuf)
		goto fail;
	dev->reg_state = NETREG_REGISTERED;
	set_bit(__LINK_STATE_PRESENT, &dev->state);
	pr_info("kpi7_net: %s: %s %pM\n", dev->name, dev->dev.parent && dev->dev.parent->driver ? dev->dev.parent->driver->name : "?",
		dev->dev_addr);
	if (kpi_net_hook)
		kpi_net_hook(dev, 1);
	return 0;
fail:
	kfree(k->rxbuf);
	k->rxbuf = NULL;
	k->dev = NULL;
	return -ENOMEM;
}
EXPORT_SYMBOL(register_netdev);

void unregister_netdev(struct net_device *dev)
{
	struct kpi_netdev *k = kpi_netdev_of(dev);

	if (kpi_net_hook)
		kpi_net_hook(dev, 0);
	if (netif_running(dev) && dev->netdev_ops->ndo_stop) {
		rtnl_lock();
		dev->netdev_ops->ndo_stop(dev);
		clear_bit(__LINK_STATE_START, &dev->state);
		rtnl_unlock();
	}
	dev->reg_state = NETREG_UNREGISTERED;
	if (k) {
		cancel_work_sync(&k->link_work);
		kfree(k->rxbuf);
		memset(k, 0, sizeof(*k));
	}
}
EXPORT_SYMBOL(unregister_netdev);

void netif_device_detach(struct net_device *dev)
{
	if (test_and_clear_bit(__LINK_STATE_PRESENT, &dev->state) && netif_running(dev))
		netif_tx_stop_all_queues(dev);
}
EXPORT_SYMBOL(netif_device_detach);

void netif_device_attach(struct net_device *dev)
{
	if (!test_and_set_bit(__LINK_STATE_PRESENT, &dev->state) && netif_running(dev))
		netif_tx_wake_all_queues(dev);
}
EXPORT_SYMBOL(netif_device_attach);

/* dev_addr_lists.c callbacks: there are no netdevice notifier users here */
int call_netdevice_notifiers(unsigned long val, struct net_device *dev) { return 0; }
EXPORT_SYMBOL(call_netdevice_notifiers);
int dev_pre_changeaddr_notify(struct net_device *dev, const char *addr, struct netlink_ext_ack *extack) { return 0; }
EXPORT_SYMBOL(dev_pre_changeaddr_notify);

/* Unicast filtering falls back to promiscuous mode when the hardware cannot filter, as in net/core/dev.c */
void __dev_set_rx_mode(struct net_device *dev)
{
	const struct net_device_ops *ops = dev->netdev_ops;

	if (!(dev->flags & IFF_UP) || !netif_device_present(dev))
		return;
	if (!(dev->priv_flags & IFF_UNICAST_FLT)) {
		if (!netdev_uc_empty(dev) && !dev->uc_promisc) {
			dev->flags |= IFF_PROMISC;
			dev->uc_promisc = true;
		} else if (netdev_uc_empty(dev) && dev->uc_promisc) {
			dev->flags &= ~IFF_PROMISC;
			dev->uc_promisc = false;
		}
	}
	if (ops->ndo_set_rx_mode)
		ops->ndo_set_rx_mode(dev);
}
EXPORT_SYMBOL(__dev_set_rx_mode);

/* ---- link state -------------------------------------------------------------------------------------------- */
static void kpi_link_work(struct work_struct *w)
{
	struct kpi_netdev *k = container_of(w, struct kpi_netdev, link_work);
	struct net_device *dev = k->dev;
	int up;

	if (!dev)
		return;
	up = netif_carrier_ok(dev);
	k->mbps = 0;
	if (up && dev->ethtool_ops && dev->ethtool_ops->get_link_ksettings) {
		struct ethtool_link_ksettings ks;

		memset(&ks, 0, sizeof(ks));
		if (!dev->ethtool_ops->get_link_ksettings(dev, &ks) && ks.base.speed != SPEED_UNKNOWN)
			k->mbps = ks.base.speed;
	}
	pr_info("kpi7_net: %s: link %s, %u Mbps\n", dev->name, up ? "up" : "down", k->mbps);
	if (k->ops && k->ops->link)
		k->ops->link(k->ctx, up, k->mbps);
}

void linkwatch_fire_event(struct net_device *dev)
{
	struct kpi_netdev *k = kpi_netdev_of(dev);

	if (k && k->link_work.func)
		schedule_work(&k->link_work);
}
EXPORT_SYMBOL(linkwatch_fire_event);

void netif_carrier_on(struct net_device *dev)
{
	if (test_and_clear_bit(__LINK_STATE_NOCARRIER, &dev->state)) {
		atomic_inc(&dev->carrier_up_count);
		linkwatch_fire_event(dev);
	}
}
EXPORT_SYMBOL(netif_carrier_on);

void netif_carrier_off(struct net_device *dev)
{
	if (!test_and_set_bit(__LINK_STATE_NOCARRIER, &dev->state)) {
		atomic_inc(&dev->carrier_down_count);
		linkwatch_fire_event(dev);
	}
}
EXPORT_SYMBOL(netif_carrier_off);

/* ---- transmit queues ---------------------------------------------------------------------------------------- */
static void kpi_tx_wake_host(struct net_device *dev)
{
	struct kpi_netdev *k = kpi_netdev_of(dev);

	if (k && k->ops && k->ops->tx_wake)
		k->ops->tx_wake(k->ctx);
}

void netif_tx_wake_queue(struct netdev_queue *q)
{
	if (test_and_clear_bit(__QUEUE_STATE_DRV_XOFF, &q->state))
		kpi_tx_wake_host(q->dev);
}
EXPORT_SYMBOL(netif_tx_wake_queue);

void netif_schedule_queue(struct netdev_queue *q)
{
	if (!(q->state & QUEUE_STATE_ANY_XOFF))
		kpi_tx_wake_host(q->dev);
}
EXPORT_SYMBOL(netif_schedule_queue);

void netif_tx_stop_all_queues(struct net_device *dev)
{
	unsigned int i;

	for (i = 0; i < dev->num_tx_queues; i++)
		netif_tx_stop_queue(netdev_get_tx_queue(dev, i));
}
EXPORT_SYMBOL(netif_tx_stop_all_queues);

int netif_set_real_num_tx_queues(struct net_device *dev, unsigned int txq)
{
	if (txq < 1 || txq > dev->num_tx_queues)
		return -EINVAL;
	dev->real_num_tx_queues = txq;
	return 0;
}
EXPORT_SYMBOL(netif_set_real_num_tx_queues);

int netif_set_real_num_rx_queues(struct net_device *dev, unsigned int rxq)
{
	if (rxq < 1 || rxq > dev->num_rx_queues)
		return -EINVAL;
	dev->real_num_rx_queues = rxq;
	return 0;
}
EXPORT_SYMBOL(netif_set_real_num_rx_queues);

int netif_set_real_num_queues(struct net_device *dev, unsigned int txq, unsigned int rxq)
{
	return netif_set_real_num_tx_queues(dev, txq) ?: netif_set_real_num_rx_queues(dev, rxq);
}
EXPORT_SYMBOL(netif_set_real_num_queues);

unsigned long dev_trans_start(struct net_device *dev)
{
	unsigned long res = READ_ONCE(netdev_get_tx_queue(dev, 0)->trans_start);
	unsigned int i;

	for (i = 1; i < dev->num_tx_queues; i++) {
		unsigned long v = READ_ONCE(netdev_get_tx_queue(dev, i)->trans_start);

		if (v && time_after(v, res))
			res = v;
	}
	return res;
}
EXPORT_SYMBOL(dev_trans_start);

netdev_features_t passthru_features_check(struct sk_buff *skb, struct net_device *dev, netdev_features_t features) { return features; }
EXPORT_SYMBOL(passthru_features_check);

void netif_set_tso_max_size(struct net_device *dev, unsigned int size)
{
	dev->tso_max_size = min(GSO_MAX_SIZE, size);
	if (size < READ_ONCE(dev->gso_max_size))
		dev->gso_max_size = size;
}
EXPORT_SYMBOL(netif_set_tso_max_size);

void netif_set_tso_max_segs(struct net_device *dev, unsigned int segs)
{
	dev->tso_max_segs = segs;
	if (segs < READ_ONCE(dev->gso_max_segs))
		dev->gso_max_segs = segs;
}
EXPORT_SYMBOL(netif_set_tso_max_segs);

void netdev_update_features(struct net_device *dev) { }
EXPORT_SYMBOL(netdev_update_features);
void netdev_sw_irq_coalesce_default_on(struct net_device *dev) { }
EXPORT_SYMBOL(netdev_sw_irq_coalesce_default_on);
int netdev_set_num_tc(struct net_device *dev, u8 num_tc) { return num_tc ? -EOPNOTSUPP : 0; }
EXPORT_SYMBOL(netdev_set_num_tc);
void netdev_reset_tc(struct net_device *dev) { dev->num_tc = 0; }
EXPORT_SYMBOL(netdev_reset_tc);
int netdev_set_tc_queue(struct net_device *dev, u8 tc, u16 count, u16 offset) { return -EOPNOTSUPP; }
EXPORT_SYMBOL(netdev_set_tc_queue);

/* ---- statistics --------------------------------------------------------------------------------------------- */
void netdev_stats_to_stats64(struct rtnl_link_stats64 *stats64, const struct net_device_stats *netdev_stats)
{
	size_t i, n = min(sizeof(*stats64), sizeof(*netdev_stats)) / sizeof(u64);
	const atomic_long_t *src = (atomic_long_t *)netdev_stats;
	u64 *dst = (u64 *)stats64;

	for (i = 0; i < n; i++)
		dst[i] = (unsigned long)atomic_long_read(&src[i]);
	memset((char *)stats64 + n * sizeof(u64), 0, sizeof(*stats64) - n * sizeof(u64));
}
EXPORT_SYMBOL(netdev_stats_to_stats64);

void dev_fetch_sw_netstats(struct rtnl_link_stats64 *s, const struct pcpu_sw_netstats __percpu *netstats)
{
	const struct pcpu_sw_netstats *st = per_cpu_ptr(netstats, 0);

	s->rx_packets += u64_stats_read(&st->rx_packets);
	s->rx_bytes += u64_stats_read(&st->rx_bytes);
	s->tx_packets += u64_stats_read(&st->tx_packets);
	s->tx_bytes += u64_stats_read(&st->tx_bytes);
}
EXPORT_SYMBOL(dev_fetch_sw_netstats);

struct rtnl_link_stats64 *dev_get_stats(struct net_device *dev, struct rtnl_link_stats64 *storage)
{
	const struct net_device_ops *ops = dev->netdev_ops;

	memset(storage, 0, sizeof(*storage));
	if (ops->ndo_get_stats64)
		ops->ndo_get_stats64(dev, storage);
	else if (ops->ndo_get_stats)
		netdev_stats_to_stats64(storage, ops->ndo_get_stats(dev));
	else
		netdev_stats_to_stats64(storage, &dev->stats);
	return storage;
}
EXPORT_SYMBOL(dev_get_stats);

/* ---- NAPI: each context runs in a host DPC ------------------------------------------------------------------ */
static void kpi_napi_dpc(void *arg)
{
	struct kpi_napi *kn = arg;
	struct napi_struct *n = kn->napi;
	int work;

	if (!n || !test_bit(NAPI_STATE_SCHED, &n->state))
		return;
	work = n->poll(n, n->weight);
	if (work < n->weight)
		return;			/* the driver completed (or will reschedule) the context itself */
	/* budget used up: the context is still scheduled, poll again (unless it is being disabled) */
	if (test_bit(NAPI_STATE_DISABLE, &n->state)) {
		napi_complete(n);
		return;
	}
	nt_dpc_queue(kn->dpc);
}

void netif_napi_add_weight(struct net_device *dev, struct napi_struct *napi, int (*poll)(struct napi_struct *, int), int weight)
{
	unsigned long flags;
	int i;

	INIT_LIST_HEAD(&napi->poll_list);
	napi->timer.function = NULL;
	napi->gro_bitmask = 0;
	napi->skb = NULL;
	napi->poll = poll;
	napi->weight = weight ? weight : NAPI_POLL_WEIGHT;
	napi->dev = dev;
	napi->napi_id = 0;
	set_bit(NAPI_STATE_SCHED, &napi->state);
	set_bit(NAPI_STATE_NPSVC, &napi->state);
	list_add(&napi->dev_list, &dev->napi_list);
	spin_lock_irqsave(&kpi_napi_lock, flags);
	for (i = 0; i < KPI_NAPI_MAX; i++)
		if (!kpi_napis[i].napi) {
			kpi_napis[i].napi = napi;
			napi->napi_id = i + 1;
			break;
		}
	spin_unlock_irqrestore(&kpi_napi_lock, flags);
	if (!napi->napi_id) {
		pr_err("kpi7_net: too many NAPI contexts\n");
		return;
	}
	if (!kpi_napis[i].dpc)
		kpi_napis[i].dpc = nt_dpc_alloc(kpi_napi_dpc, &kpi_napis[i]);
	if (!kpi_napis[i].rxbuf)
		kpi_napis[i].rxbuf = kmalloc(KPI_RX_FRAME_MAX, GFP_KERNEL);
}
EXPORT_SYMBOL(netif_napi_add_weight);

void __netif_napi_del(struct napi_struct *napi)
{
	struct kpi_napi *kn = kpi_napi_of(napi);

	list_del_init(&napi->dev_list);
	if (kn)
		kn->napi = NULL;	/* the DPC and buffer are kept for the next context in this slot */
	napi->napi_id = 0;
}
EXPORT_SYMBOL(__netif_napi_del);

bool napi_schedule_prep(struct napi_struct *n)
{
	unsigned long new, val = READ_ONCE(n->state);

	do {
		if (unlikely(val & NAPIF_STATE_DISABLE))
			return false;
		new = val | NAPIF_STATE_SCHED;
		/* a schedule while polling: make the poll run once more */
		new |= (val & NAPIF_STATE_SCHED) / NAPIF_STATE_SCHED * NAPIF_STATE_MISSED;
	} while (!try_cmpxchg(&n->state, &val, new));
	return !(val & NAPIF_STATE_SCHED);
}
EXPORT_SYMBOL(napi_schedule_prep);

void __napi_schedule(struct napi_struct *n)
{
	struct kpi_napi *kn = kpi_napi_of(n);

	if (kn && kn->dpc)
		nt_dpc_queue(kn->dpc);
}
EXPORT_SYMBOL(__napi_schedule);

bool napi_complete_done(struct napi_struct *n, int work_done)
{
	unsigned long val, new;

	if (unlikely(n->state & (NAPIF_STATE_NPSVC | NAPIF_STATE_IN_BUSY_POLL)))
		return false;
	val = READ_ONCE(n->state);
	do {
		new = val & ~(NAPIF_STATE_MISSED | NAPIF_STATE_SCHED | NAPIF_STATE_SCHED_THREADED | NAPIF_STATE_PREFER_BUSY_POLL);
		/* MISSED: keep SCHED, poll() runs once more */
		new |= (val & NAPIF_STATE_MISSED) / NAPIF_STATE_MISSED * NAPIF_STATE_SCHED;
	} while (!try_cmpxchg(&n->state, &val, new));
	if (unlikely(val & NAPIF_STATE_MISSED)) {
		__napi_schedule(n);
		return false;
	}
	return true;
}
EXPORT_SYMBOL(napi_complete_done);

void napi_enable(struct napi_struct *n)
{
	unsigned long new, val = READ_ONCE(n->state);

	do {
		new = val & ~(NAPIF_STATE_SCHED | NAPIF_STATE_NPSVC);
	} while (!try_cmpxchg(&n->state, &val, new));
}
EXPORT_SYMBOL(napi_enable);

void napi_disable(struct napi_struct *n)
{
	unsigned long val, new;

	set_bit(NAPI_STATE_DISABLE, &n->state);
	val = READ_ONCE(n->state);
	do {
		while (val & (NAPIF_STATE_SCHED | NAPIF_STATE_NPSVC)) {
			msleep(1);
			val = READ_ONCE(n->state);
		}
		new = val | NAPIF_STATE_SCHED | NAPIF_STATE_NPSVC;
	} while (!try_cmpxchg(&n->state, &val, new));
	clear_bit(NAPI_STATE_DISABLE, &n->state);
}
EXPORT_SYMBOL(napi_disable);

/* ---- receive: hand the whole Ethernet frame to the host ----------------------------------------------------------- */
static void kpi_deliver(struct sk_buff *skb, unsigned char *bounce, spinlock_t *bounce_lock)
{
	struct net_device *dev = skb->dev;
	struct kpi_netdev *k = dev ? kpi_netdev_of(dev) : NULL;
	unsigned int hlen, total;
	unsigned long flags = 0;
	unsigned char *frame;

	if (!k || !k->ops || !k->ops->rx || !skb_mac_header_was_set(skb))
		goto out;
	hlen = skb->data - skb_mac_header(skb);
	total = hlen + skb->len;
	if (skb_vlan_tag_present(skb))
		total += VLAN_HLEN;
	if (hlen < ETH_HLEN || total > KPI_RX_FRAME_MAX)
		goto out;
	if (!skb_is_nonlinear(skb) && !skb_vlan_tag_present(skb)) {
		k->ops->rx(k->ctx, skb_mac_header(skb), total);	/* contiguous already */
		goto out;
	}
	if (!bounce)
		goto out;
	if (bounce_lock)
		spin_lock_irqsave(bounce_lock, flags);
	frame = bounce;
	if (skb_vlan_tag_present(skb)) {
		struct vlan_ethhdr *veth = (struct vlan_ethhdr *)frame;

		memcpy(frame, skb_mac_header(skb), 2 * ETH_ALEN);
		veth->h_vlan_proto = skb->vlan_proto;
		veth->h_vlan_TCI = htons(skb_vlan_tag_get(skb));
		memcpy(frame + 2 * ETH_ALEN + VLAN_HLEN, skb_mac_header(skb) + 2 * ETH_ALEN, hlen - 2 * ETH_ALEN);
		frame += hlen + VLAN_HLEN;
	} else {
		memcpy(frame, skb_mac_header(skb), hlen);
		frame += hlen;
	}
	if (!skb_copy_bits(skb, 0, frame, skb->len))
		k->ops->rx(k->ctx, bounce, total);
	if (bounce_lock)
		spin_unlock_irqrestore(bounce_lock, flags);
out:
	kpi_skb_free(skb);
}

gro_result_t napi_gro_receive(struct napi_struct *napi, struct sk_buff *skb)
{
	struct kpi_napi *kn = kpi_napi_of(napi);

	kpi_deliver(skb, kn ? kn->rxbuf : NULL, NULL);
	return GRO_NORMAL;
}
EXPORT_SYMBOL(napi_gro_receive);

int netif_rx(struct sk_buff *skb)
{
	struct kpi_netdev *k = skb->dev ? kpi_netdev_of(skb->dev) : NULL;

	kpi_deliver(skb, k ? k->rxbuf : NULL, k ? &k->rx_lock : NULL);
	return NET_RX_SUCCESS;
}
EXPORT_SYMBOL(netif_rx);

int netif_receive_skb(struct sk_buff *skb) { return netif_rx(skb); }
EXPORT_SYMBOL(netif_receive_skb);

/* ---- entry points for the host (linuxkpi.sys netbus.c) ------------------------------------------------------ */
void kpi_net_set_hook(void (*hook)(struct net_device *dev, int added))
{
	int i;

	kpi_net_hook = hook;
	for (i = 0; i < KPI_NET_MAX; i++)	/* devices registered before the host was ready */
		if (kpi_nets[i].dev && hook)
			hook(kpi_nets[i].dev, 1);
}
EXPORT_SYMBOL(kpi_net_set_hook);

/* PASSIVE_LEVEL. Brings the interface up (ndo_open) and starts reporting frames and link changes. */
int kpi_net_open(struct net_device *dev, const struct kpi_net_host_ops *ops, void *ctx)
{
	struct kpi_netdev *k = kpi_netdev_of(dev);
	int ret = 0;

	if (!k)
		return -ENODEV;
	rtnl_lock();
	INIT_WORK(&k->link_work, kpi_link_work);
	k->ctx = ctx;
	k->ops = ops;
	if (!netif_running(dev)) {
		set_bit(__LINK_STATE_START, &dev->state);
		ret = dev->netdev_ops->ndo_open ? dev->netdev_ops->ndo_open(dev) : 0;
		if (ret) {
			clear_bit(__LINK_STATE_START, &dev->state);
			k->ops = NULL;
		} else {
			dev->flags |= IFF_UP | IFF_ALLMULTI;
			netif_addr_lock_bh(dev);
			if (dev->netdev_ops->ndo_set_rx_mode)
				dev->netdev_ops->ndo_set_rx_mode(dev);
			netif_addr_unlock_bh(dev);
		}
	}
	rtnl_unlock();
	pr_info("kpi7_net: %s: open -> %d\n", dev->name, ret);
	if (!ret)
		schedule_work(&k->link_work);	/* report the current link state */
	return ret;
}
EXPORT_SYMBOL(kpi_net_open);

void kpi_net_close(struct net_device *dev)
{
	struct kpi_netdev *k = kpi_netdev_of(dev);

	if (!k)
		return;
	rtnl_lock();
	k->ops = NULL;
	if (netif_running(dev)) {
		if (dev->netdev_ops->ndo_stop)
			dev->netdev_ops->ndo_stop(dev);
		clear_bit(__LINK_STATE_START, &dev->state);
		dev->flags &= ~IFF_UP;
	}
	rtnl_unlock();
	cancel_work_sync(&k->link_work);
}
EXPORT_SYMBOL(kpi_net_close);

/* Any IRQL <= DISPATCH. Returns 0 (sent), 1 (queue full: retry after tx_wake), or a negative errno (dropped). */
int kpi_net_xmit(struct net_device *dev, const void *frame, unsigned int len)
{
	struct netdev_queue *txq = netdev_get_tx_queue(dev, 0);
	struct sk_buff *skb;
	netdev_tx_t rc;

	if (!netif_running(dev) || !netif_device_present(dev))
		return -ENETDOWN;
	if (len < ETH_HLEN || len > dev->mtu + ETH_HLEN + VLAN_HLEN)
		return -EMSGSIZE;
	if (netif_xmit_frozen_or_stopped(txq))
		return 1;
	skb = __netdev_alloc_skb(dev, max_t(unsigned int, len, ETH_ZLEN), GFP_ATOMIC);
	if (!skb)
		return 1;
	skb_put_data(skb, frame, len);
	skb_reset_mac_header(skb);
	skb_set_network_header(skb, ETH_HLEN);
	skb->protocol = ((const struct ethhdr *)frame)->h_proto;
	skb->ip_summed = CHECKSUM_NONE;
	skb_set_queue_mapping(skb, 0);
	__netif_tx_lock(txq, smp_processor_id());
	if (netif_xmit_frozen_or_stopped(txq)) {
		__netif_tx_unlock(txq);
		kpi_skb_free(skb);
		return 1;
	}
	rc = dev->netdev_ops->ndo_start_xmit(skb, dev);
	if (rc == NETDEV_TX_OK)
		txq_trans_update(txq);
	__netif_tx_unlock(txq);
	if (rc != NETDEV_TX_OK) {	/* NETDEV_TX_BUSY: the driver did not take the skb */
		kpi_skb_free(skb);
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(kpi_net_xmit);

void kpi_net_get_info(struct net_device *dev, struct kpi_net_info *info)
{
	struct kpi_netdev *k = kpi_netdev_of(dev);
	struct device *parent = dev->dev.parent;

	memset(info, 0, sizeof(*info));
	memcpy(info->mac, dev->dev_addr, ETH_ALEN);
	info->mtu = dev->mtu;
	info->link = netif_running(dev) && netif_carrier_ok(dev);
	info->mbps = k ? k->mbps : 0;
	strscpy(info->driver, parent && parent->driver ? parent->driver->name : "linux", sizeof(info->driver));
	strscpy(info->ifname, dev->name, sizeof(info->ifname));
}
EXPORT_SYMBOL(kpi_net_get_info);

/* flags: bit 0 promiscuous, bit 1 all multicast (always set on open), bit 2 broadcast */
void kpi_net_set_rx_flags(struct net_device *dev, unsigned int flags)
{
	unsigned int f = dev->flags & ~(IFF_PROMISC | IFF_ALLMULTI);

	if (flags & 1)
		f |= IFF_PROMISC;
	if (flags & 2)
		f |= IFF_ALLMULTI;
	if (f == dev->flags || !netif_running(dev))
		return;
	dev->flags = f;
	netif_addr_lock_bh(dev);
	if (dev->netdev_ops->ndo_set_rx_mode)
		dev->netdev_ops->ndo_set_rx_mode(dev);
	netif_addr_unlock_bh(dev);
}
EXPORT_SYMBOL(kpi_net_set_rx_flags);

/* out[0..5]: rx packets, tx packets, rx bytes, tx bytes, rx errors, tx errors */
void kpi_net_get_stats(struct net_device *dev, unsigned long long *out)
{
	struct rtnl_link_stats64 s;

	dev_get_stats(dev, &s);
	out[0] = s.rx_packets;
	out[1] = s.tx_packets;
	out[2] = s.rx_bytes;
	out[3] = s.tx_bytes;
	out[4] = s.rx_errors + s.rx_dropped;
	out[5] = s.tx_errors + s.tx_dropped;
}
EXPORT_SYMBOL(kpi_net_get_stats);

/* ---- printing ------------------------------------------------------------------------------------------------------ */
void netdev_printk(const char *level, const struct net_device *dev, const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;
	printk("%s%s: %pV", level, dev ? dev->name : "(null)", &vaf);
	va_end(args);
}
EXPORT_SYMBOL(netdev_printk);

#define KPI_NETDEV_LEVEL(func, level)						\
void func(const struct net_device *dev, const char *fmt, ...)			\
{										\
	struct va_format vaf;							\
	va_list args;								\
										\
	va_start(args, fmt);							\
	vaf.fmt = fmt;								\
	vaf.va = &args;								\
	printk(level "%s: %pV", dev ? dev->name : "(null)", &vaf);		\
	va_end(args);								\
}										\
EXPORT_SYMBOL(func);
KPI_NETDEV_LEVEL(netdev_err, KERN_ERR)
KPI_NETDEV_LEVEL(netdev_warn, KERN_WARNING)
KPI_NETDEV_LEVEL(netdev_info, KERN_INFO)

static DEFINE_RATELIMIT_STATE(kpi_net_ratelimit, 5 * HZ, 10);
int net_ratelimit(void) { return __ratelimit(&kpi_net_ratelimit); }
EXPORT_SYMBOL(net_ratelimit);

void netdev_rss_key_fill(void *buffer, size_t len) { get_random_bytes(buffer, len); }
EXPORT_SYMBOL(netdev_rss_key_fill);

/* ---- ethtool helpers ----------------------------------------------------------------------------------------------- */
u32 ethtool_op_get_link(struct net_device *dev) { return netif_carrier_ok(dev) ? 1 : 0; }
EXPORT_SYMBOL(ethtool_op_get_link);

int ethtool_op_get_ts_info(struct net_device *dev, struct kernel_ethtool_ts_info *info)
{
	info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
	info->phc_index = -1;
	return 0;
}
EXPORT_SYMBOL(ethtool_op_get_ts_info);

void ethtool_convert_legacy_u32_to_link_mode(unsigned long *dst, u32 legacy_u32)
{
	linkmode_zero(dst);
	dst[0] = legacy_u32;
}
EXPORT_SYMBOL(ethtool_convert_legacy_u32_to_link_mode);

bool ethtool_convert_link_mode_to_legacy_u32(u32 *legacy_u32, const unsigned long *src)
{
	bool retval = true;

	if (find_next_bit(src, __ETHTOOL_LINK_MODE_MASK_NBITS, 32) != __ETHTOOL_LINK_MODE_MASK_NBITS)
		retval = false;
	*legacy_u32 = src[0];
	return retval;
}
EXPORT_SYMBOL(ethtool_convert_link_mode_to_legacy_u32);

void ethtool_sprintf(u8 **data, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsnprintf(*data, ETH_GSTRING_LEN, fmt, args);
	va_end(args);
	*data += ETH_GSTRING_LEN;
}
EXPORT_SYMBOL(ethtool_sprintf);

void ethtool_puts(u8 **data, const char *str)
{
	strscpy(*data, str, ETH_GSTRING_LEN);
	*data += ETH_GSTRING_LEN;
}
EXPORT_SYMBOL(ethtool_puts);

void ethtool_set_ethtool_phy_ops(const struct ethtool_phy_ops *ops) { }
EXPORT_SYMBOL(ethtool_set_ethtool_phy_ops);
int ethnl_cable_test_alloc(struct phy_device *phydev, u8 cmd) { return -EOPNOTSUPP; }
EXPORT_SYMBOL(ethnl_cable_test_alloc);
void ethnl_cable_test_free(struct phy_device *phydev) { }
EXPORT_SYMBOL(ethnl_cable_test_free);
void ethnl_cable_test_finished(struct phy_device *phydev) { }
EXPORT_SYMBOL(ethnl_cable_test_finished);
void do_trace_netlink_extack(const char *msg) { }
EXPORT_SYMBOL(do_trace_netlink_extack);
int ndo_dflt_fdb_add(struct ndmsg *ndm, struct nlattr *tb[], struct net_device *dev, const unsigned char *addr, u16 vid,
		     u16 flags) { return -EINVAL; }
EXPORT_SYMBOL(ndo_dflt_fdb_add);

/* ---- XDP / traffic control: no programs, no offloads ------------------------------------------------------------- */
int __xdp_rxq_info_reg(struct xdp_rxq_info *xdp_rxq, struct net_device *dev, u32 queue_index, unsigned int napi_id,
		       u32 frag_size)
{
	memset(xdp_rxq, 0, sizeof(*xdp_rxq));
	xdp_rxq->dev = dev;
	xdp_rxq->queue_index = queue_index;
	xdp_rxq->frag_size = frag_size;
	xdp_rxq->reg_state = REG_STATE_REGISTERED;
	return 0;
}
EXPORT_SYMBOL(__xdp_rxq_info_reg);
void xdp_rxq_info_unreg(struct xdp_rxq_info *xdp_rxq) { xdp_rxq->reg_state = REG_STATE_UNREGISTERED; }
EXPORT_SYMBOL(xdp_rxq_info_unreg);
bool xdp_rxq_info_is_reg(struct xdp_rxq_info *xdp_rxq) { return xdp_rxq->reg_state == REG_STATE_REGISTERED; }
EXPORT_SYMBOL(xdp_rxq_info_is_reg);
int xdp_rxq_info_reg_mem_model(struct xdp_rxq_info *xdp_rxq, enum xdp_mem_type type, void *allocator)
{
	xdp_rxq->mem.type = type;
	return 0;
}
EXPORT_SYMBOL(xdp_rxq_info_reg_mem_model);
void xdp_rxq_info_unreg_mem_model(struct xdp_rxq_info *xdp_rxq) { }
EXPORT_SYMBOL(xdp_rxq_info_unreg_mem_model);
void xdp_return_frame(struct xdp_frame *xdpf) { }
EXPORT_SYMBOL(xdp_return_frame);
struct xdp_frame *xdp_convert_zc_to_xdp_frame(struct xdp_buff *xdp) { return NULL; }
EXPORT_SYMBOL(xdp_convert_zc_to_xdp_frame);
int xdp_do_redirect(struct net_device *dev, struct xdp_buff *xdp, struct bpf_prog *xdp_prog) { return -EOPNOTSUPP; }
EXPORT_SYMBOL(xdp_do_redirect);
void xdp_do_flush(void) { }
EXPORT_SYMBOL(xdp_do_flush);
void xdp_features_set_redirect_target(struct net_device *dev, bool support_sg) { }
EXPORT_SYMBOL(xdp_features_set_redirect_target);
void xdp_features_clear_redirect_target(struct net_device *dev) { }
EXPORT_SYMBOL(xdp_features_clear_redirect_target);
void xdp_warn(const char *msg, const char *func, const int line) { pr_warn("XDP: %s (%s:%d)\n", msg, func, line); }
EXPORT_SYMBOL(xdp_warn);
u32 xdp_master_redirect(struct xdp_buff *xdp) { return XDP_ABORTED; }
EXPORT_SYMBOL(xdp_master_redirect);
void bpf_warn_invalid_xdp_action(struct net_device *dev, struct bpf_prog *prog, u32 act) { }
EXPORT_SYMBOL(bpf_warn_invalid_xdp_action);
int flow_block_cb_setup_simple(struct flow_block_offload *f, struct list_head *driver_list, flow_setup_cb_t *cb,
			       void *cb_ident, void *cb_priv, bool ingress_only) { return -EOPNOTSUPP; }
EXPORT_SYMBOL(flow_block_cb_setup_simple);
void flow_rule_match_basic(const struct flow_rule *rule, struct flow_match_basic *out) { memset(out, 0, sizeof(*out)); }
EXPORT_SYMBOL(flow_rule_match_basic);
void flow_rule_match_control(const struct flow_rule *rule, struct flow_match_control *out) { memset(out, 0, sizeof(*out)); }
EXPORT_SYMBOL(flow_rule_match_control);
void flow_rule_match_eth_addrs(const struct flow_rule *rule, struct flow_match_eth_addrs *out) { memset(out, 0, sizeof(*out)); }
EXPORT_SYMBOL(flow_rule_match_eth_addrs);
void flow_rule_match_vlan(const struct flow_rule *rule, struct flow_match_vlan *out) { memset(out, 0, sizeof(*out)); }
EXPORT_SYMBOL(flow_rule_match_vlan);

/* ---- PCI / PM / IRQ pieces the NIC drivers use ------------------------------------------------------------------- */
void cpu_latency_qos_add_request(struct pm_qos_request *req, s32 value) { }
EXPORT_SYMBOL(cpu_latency_qos_add_request);
void cpu_latency_qos_update_request(struct pm_qos_request *req, s32 new_value) { }
EXPORT_SYMBOL(cpu_latency_qos_update_request);
void cpu_latency_qos_remove_request(struct pm_qos_request *req) { }
EXPORT_SYMBOL(cpu_latency_qos_remove_request);
int pm_schedule_suspend(struct device *dev, unsigned int delay) { return -EAGAIN; }
EXPORT_SYMBOL(pm_schedule_suspend);
void pm_system_wakeup(void) { }
EXPORT_SYMBOL(pm_system_wakeup);
int pci_enable_wake(struct pci_dev *dev, pci_power_t state, bool enable) { return 0; }
EXPORT_SYMBOL(pci_enable_wake);
int pci_disable_link_state(struct pci_dev *pdev, int state) { return 0; }
EXPORT_SYMBOL(pci_disable_link_state);
int pci_disable_link_state_locked(struct pci_dev *pdev, int state) { return 0; }
EXPORT_SYMBOL(pci_disable_link_state_locked);
int pci_request_selected_regions_exclusive(struct pci_dev *pdev, int bars, const char *name) { return 0; }
EXPORT_SYMBOL(pci_request_selected_regions_exclusive);
int pci_reset_bus(struct pci_dev *dev) { return -ENOTTY; }
EXPORT_SYMBOL(pci_reset_bus);
int pci_status_get_and_clear_errors(struct pci_dev *pdev)
{
	u16 status;

	pci_read_config_word(pdev, PCI_STATUS, &status);
	status &= PCI_STATUS_ERROR_BITS;
	if (status)
		pci_write_config_word(pdev, PCI_STATUS, status);
	return status;
}
EXPORT_SYMBOL(pci_status_get_and_clear_errors);
void pcie_print_link_status(struct pci_dev *dev) { }
EXPORT_SYMBOL(pcie_print_link_status);

int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val)
{
	int cap = pci_find_capability(dev, PCI_CAP_ID_EXP);

	return cap ? pci_write_config_word(dev, cap + pos, val) : -EINVAL;
}
EXPORT_SYMBOL(pcie_capability_write_word);

/* pcim_iomap_*: the managed BAR mapping table, one per device */
struct kpi_pcim { struct pci_dev *pdev; void __iomem *table[PCI_STD_NUM_BARS]; };
static struct kpi_pcim kpi_pcims[KPI_NET_MAX];

void __iomem *const *pcim_iomap_table(struct pci_dev *pdev)
{
	int i;

	for (i = 0; i < KPI_NET_MAX; i++)
		if (kpi_pcims[i].pdev == pdev)
			return kpi_pcims[i].table;
	for (i = 0; i < KPI_NET_MAX; i++)
		if (!kpi_pcims[i].pdev) {
			kpi_pcims[i].pdev = pdev;
			return kpi_pcims[i].table;
		}
	return NULL;
}
EXPORT_SYMBOL(pcim_iomap_table);

int pcim_iomap_regions(struct pci_dev *pdev, int mask, const char *name)
{
	void __iomem **t = (void __iomem **)pcim_iomap_table(pdev);
	int bar;

	if (!t)
		return -ENOMEM;
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		if (!(mask & (1 << bar)) || !pci_resource_len(pdev, bar))
			continue;
		t[bar] = pci_iomap(pdev, bar, 0);
		if (!t[bar])
			return -ENOMEM;
	}
	return 0;
}
EXPORT_SYMBOL(pcim_iomap_regions);
int pcim_set_mwi(struct pci_dev *dev) { return 0; }
EXPORT_SYMBOL(pcim_set_mwi);

bool disable_hardirq(unsigned int irq) { disable_irq(irq); return true; }
EXPORT_SYMBOL(disable_hardirq);
void disable_irq_nosync(unsigned int irq) { disable_irq(irq); }
EXPORT_SYMBOL(disable_irq_nosync);
void irq_wake_thread(unsigned int irq, void *dev_id) { }
EXPORT_SYMBOL(irq_wake_thread);

int i2c_smbus_read_byte_data(const struct i2c_client *client, u8 command) { return -ENODEV; }
EXPORT_SYMBOL(i2c_smbus_read_byte_data);
int i2c_smbus_write_byte_data(const struct i2c_client *client, u8 command, u8 value) { return -ENODEV; }
EXPORT_SYMBOL(i2c_smbus_write_byte_data);

/* ---- device model pieces used by phylib --------------------------------------------------------------------------- */
struct device *class_find_device(const struct class *class, const struct device *start, const void *data,
				 int (*match)(struct device *, const void *)) { return NULL; }
EXPORT_SYMBOL(class_find_device);
void device_link_del(struct device_link *link) { }
EXPORT_SYMBOL(device_link_del);
int device_match_name(struct device *dev, const void *name) { return sysfs_streq(dev_name(dev), name); }
EXPORT_SYMBOL(device_match_name);
int device_match_fwnode(struct device *dev, const void *fwnode) { return dev_fwnode(dev) == fwnode; }
EXPORT_SYMBOL(device_match_fwnode);
struct fwnode_handle *fwnode_handle_get(struct fwnode_handle *fwnode) { return fwnode; }
EXPORT_SYMBOL(fwnode_handle_get);
int fwnode_property_read_string(const struct fwnode_handle *fwnode, const char *propname, const char **val) { return -EINVAL; }
EXPORT_SYMBOL(fwnode_property_read_string);
bool is_acpi_data_node(const struct fwnode_handle *fwnode) { return false; }
EXPORT_SYMBOL(is_acpi_data_node);
int sysfs_create_link_nowarn(struct kobject *kobj, struct kobject *target, const char *name) { return 0; }
EXPORT_SYMBOL(sysfs_create_link_nowarn);
int mdiobus_setup_mdiodev_from_board_info(struct mii_bus *bus,
					  int (*cb)(struct mii_bus *bus, struct mdio_board_info *bi)) { return 0; }
EXPORT_SYMBOL(mdiobus_setup_mdiodev_from_board_info);

/* ---- time ---------------------------------------------------------------------------------------------------------- */
void set_normalized_timespec64(struct timespec64 *ts, time64_t sec, s64 nsec)
{
	while (nsec >= NSEC_PER_SEC) {
		nsec -= NSEC_PER_SEC;
		++sec;
	}
	while (nsec < 0) {
		nsec += NSEC_PER_SEC;
		--sec;
	}
	ts->tv_sec = sec;
	ts->tv_nsec = nsec;
}
EXPORT_SYMBOL(set_normalized_timespec64);
void ktime_get_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64(nt_ktime_ns()); }
EXPORT_SYMBOL(ktime_get_ts64);
void ktime_get_raw_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64(nt_ktime_ns()); }
EXPORT_SYMBOL(ktime_get_raw_ts64);
void ktime_get_real_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64(nt_real_ns()); }
EXPORT_SYMBOL(ktime_get_real_ts64);
unsigned long long notrace sched_clock(void) { return nt_ktime_ns(); }
EXPORT_SYMBOL(sched_clock);
int get_device_system_crosststamp(int (*get_time_fn)(ktime_t *device_time, struct system_counterval_t *system_counterval,
						     void *ctx),
				  void *ctx, struct system_time_snapshot *history,
				  struct system_device_crosststamp *xtstamp) { return -EOPNOTSUPP; }
EXPORT_SYMBOL(get_device_system_crosststamp);
unsigned long round_jiffies(unsigned long j)
{
	unsigned long r = j % HZ;

	j -= r;
	if (r >= HZ / 4)
		j += HZ;
	return time_is_after_jiffies(j) ? j : j + HZ;
}
EXPORT_SYMBOL(round_jiffies);
