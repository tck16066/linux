// SPDX-License-Identifier: GPL-2.0-or-later
/*
* transport.c - Remote Numa transport generic functions.
*
* Copyright (C) 2025 Trevor Kemp
*/

#include <linux/mm.h>
#include <linux/random.h>
#include <linux/rculist.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>

#include "memory.h"
#include "transport.h"
#include "rn_counters.h"

#define REMOTE_NUMA_SUPPORTED_PROTO  remote_numa_protocol_0_1
#define REMOTE_NUMA_XFER_HASH_BITS 12
#define REMOTE_NUMA_REXMIT_CHECK_MS 10

#define REMOTE_NUMA_TRANSFER_TIMEOUT_MS 1000
static u32 retry_interval_ms = 5;
module_param(retry_interval_ms, uint, 0644);
MODULE_PARM_DESC(retry_interval_ms, "Base interval between retries in ms");
static u32 max_retry_count = 20;
module_param(max_retry_count, uint, 0644);
MODULE_PARM_DESC(max_retry_count, "Max number of retries");

DEFINE_HASHTABLE(xfer_table, REMOTE_NUMA_XFER_HASH_BITS);

#include <linux/atomic.h>
#include <linux/rcupdate.h>
/*
 * xfer_table is an RCU-protected hash table. Readers walk it under
 * rcu_read_lock() without taking a lock; only the rare insert/remove takes
 * xfer_table_lock, briefly, to serialize the list mutation (hash_add_rcu /
 * hash_del_rcu). Reclamation is deferred with call_rcu() so an in-flight
 * reader can never touch a freed xfer. The "hack" tag counter is an atomic.
 * Individual transfers' mutable state is protected by their own
 * main_xfer_state_t::xfer_lock. Lock ordering (no inversions):
 *     xfer_table_lock  (outer, brief, writers only)
 *       -> xfer->xfer_lock  (inner)
 */
static spinlock_t xfer_table_lock;
static atomic_t hack;


typedef struct main_xfer_state {
	refcount_t refcnt;
	int hack;
	u64 lookup_cookie;
	struct page *target;
	wait_queue_head_t waitq;
	ktime_t last_update;
	ktime_t retry_deadline;
	u16 retry_count;
	remote_numa_cached_page_t *cached_pg;
	bool is_main_node;
	u32 hdr_main_cookie;
	u32 hdr_donor_cookie;
	struct hlist_node node;
	enum remote_numa_msg_type transfer_type;
	union {
		struct remote_numa_main_trprt_if *main_trprt;
		struct remote_numa_donor_trprt_if *donor_trprt;
	};
	// Used when receiving a page (e.g., alloc/refetch)
	unsigned long received_bitmap[BITS_TO_LONGS(PAGE_SIZE)];

	// Used when sending a page (e.g., sync/satisfaction)
	unsigned long sent_bitmap[BITS_TO_LONGS(PAGE_SIZE)];

	void *return_info;

	/* RCU reclamation head; freed via call_rcu() after the last ref drops. */
	struct rcu_head rcu;

	/* Transient link used only while the retry worker collects candidates. */
	struct list_head retry_link;

	/*
	 * Per-transfer lock. Protects this xfer's mutable state: the
	 * received/sent bitmaps, retry_count/retry_deadline, and the hack tag.
	 * The global xfer_table_lock (outer) serializes hash-table mutations
	 * (hash_add_rcu / hash_del_rcu); whenever both are needed the table
	 * lock is taken first, then the xfer lock.
	 */
	spinlock_t xfer_lock;
} main_xfer_state_t;

struct retry_work_item {
	main_xfer_state_t *xfer;
	u16 seg_len;
	struct list_head list;
};

static void alloc_tx_buffer(main_xfer_state_t *xfer,
	size_t payload_len, void **obj, void**payload_start)
{
	if (xfer->is_main_node)
		xfer->main_trprt->alloc_tx_buffer(
			payload_len,
			obj,
			payload_start);
	else
		xfer->donor_trprt->alloc_tx_buffer(
			payload_len,
			obj,
			payload_start);
}

static int tx_msg(
	main_xfer_state_t *xfer, void *msg)
{
	if (xfer->is_main_node)
		return xfer->main_trprt->tx_msg(
			xfer->main_trprt->trprt_ctx,
			xfer->return_info,
			msg);
	else
		return xfer->donor_trprt->tx_msg(
			xfer->donor_trprt->trprt_ctx,
			xfer->return_info,
			msg);
}

static u16 xfer_state_max_payload(
	main_xfer_state_t *xfer)
{
	if (xfer->is_main_node)
		return xfer->main_trprt->get_max_payload_len();
	else
		return xfer->donor_trprt->get_max_payload_len();
}

static int remote_numa_rx_mem_pg_refetch(
	struct remote_numa_donor_trprt_if *donor_if,
	remote_numa_mem_refetch_t *refetch,
	u32 main_node_id);

static int remote_numa_rx_mem_alloc_from(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_alloc_t *alloc,
	u32 main_node_id);

static int remote_numa_rx_mem_pg_sync_xfer_from(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_pg_xfer_t *xfer,
	u32 main_node_id);

static int remote_numa_rx_mem_pg_free_from(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_free_t *pg_free,
	u32 main_node_id);

static remote_numa_node_t *
__remote_numa_get_or_add_node(
	remote_numa_trprt_ctx_t *context,
	unsigned int table_bits,
	spinlock_t *lock,
	u32 node_id,
	void *(*make_priv_return_info)(void *type),
	void *type);

static remote_numa_node_t *
	__remote_numa_get_node(struct hlist_head *table,
		       unsigned int table_bits,
		       u32 node_id);

static remote_numa_node_t *
	__remote_numa_get_node_locking(struct hlist_head *table,
			       unsigned int table_bits,
			       u32 node_id);

static u32 xfer_compute_max_contig(main_xfer_state_t *x)
{
	u32 ret = find_first_zero_bit(x->received_bitmap, PAGE_SIZE);
	return ret;
}

static inline u32 xfer_hash(u64 cookie)
{
	return hash_64(cookie, REMOTE_NUMA_XFER_HASH_BITS);
}

static void xfer_free_rcu(struct rcu_head *rcu)
{
	main_xfer_state_t *xfer = container_of(rcu, main_xfer_state_t, rcu);
	struct remote_numa_cached_page *cached = xfer->cached_pg;

	// Free cached_pg if it was allocated for mem_free tracking
	if (xfer->transfer_type == remote_numa_mem_free && cached)
	{
		kfree(cached->known_page);
		kfree(cached);
	}

	kfree(xfer);
}

static void xfer_put(main_xfer_state_t *xfer)
{
	if (!xfer)
		return;
	if (refcount_dec_and_test(&xfer->refcnt))
		call_rcu(&xfer->rcu, xfer_free_rcu);
}

static main_xfer_state_t *xfer_get(u64 cookie)
{
	main_xfer_state_t *xfer;

	/*
	 * RCU read-side: walk the table lock-free. refcount_inc_not_zero() is
	 * atomic and safe to call here; a concurrently-reclaimed xfer either
	 * still has the table reference (so the inc succeeds and our own
	 * reference keeps it alive) or is already freed (refcount 0 -> the inc
	 * fails and we keep walking). call_rcu() guarantees a freed xfer stays
	 * allocated for the duration of this RCU read section.
	 */
	rcu_read_lock();
	hash_for_each_possible_rcu(xfer_table, xfer, node, xfer_hash(cookie)) {
		if (((u64)cookie) != xfer->lookup_cookie)
			continue;
		if (!refcount_inc_not_zero(&xfer->refcnt))
			continue;
		rcu_read_unlock();
		return xfer;
	}
	rcu_read_unlock();
	return NULL;
}

static remote_numa_node_t *__remote_numa_get_or_add_node(
	remote_numa_trprt_ctx_t *context,
	unsigned int table_bits,
	spinlock_t *lock,
	u32 node_id,
	void *(*make_priv_return_info)(void *type),
	void *type)
{
	remote_numa_node_t *iter_node;

	rcu_read_lock();
	hash_for_each_possible_rcu(context->node_table, iter_node, hnode, node_id) {
		if (iter_node->node_id == node_id) {
			rcu_read_unlock();
			return iter_node;
		}
	}
	rcu_read_unlock();

	remote_numa_node_t *node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return NULL;

	spin_lock_init(&node->node_lock);
	node->node_id = node_id;
	node->priv_return_info = make_priv_return_info(type);

	spin_lock(lock);
	hash_for_each_possible_rcu(context->node_table, iter_node, hnode, node_id) {
		if (iter_node->node_id == node_id) {
			spin_unlock(lock);
			kfree(node);
			return iter_node;
		}
	}

	smp_wmb();
	hash_add_rcu(context->node_table, &node->hnode, node_id);
	spin_unlock(lock);

	return node;
}


static struct delayed_work remote_numa_retry_work;

static int remote_numa_send_segment(main_xfer_state_t *xfer, u32 offset, u16 seg_len)
{
	void *tx_buf;
	remote_numa_mem_pg_xfer_t *msg;
	void **v = (void **)&msg;

	if (offset >= PAGE_SIZE)
		return -EINVAL;

	u16 payload_len = min((u32)seg_len, PAGE_SIZE - offset);
	struct page *pg = xfer->target;
	void *data = page_address(pg);

	alloc_tx_buffer(xfer, sizeof(*msg) + payload_len, &tx_buf, v);
	if (!tx_buf || !msg)
		return -ENOMEM;
	spin_lock(&xfer->xfer_lock);

	msg->hdr.version = REMOTE_NUMA_SUPPORTED_PROTO;
	msg->hdr.type = xfer->transfer_type;
	msg->hdr.main_cookie = xfer->hdr_main_cookie;
	msg->hdr.donor_cookie = xfer->hdr_donor_cookie;
	msg->flags = (offset + payload_len >= PAGE_SIZE) ? remote_numa_xfer_end_of_pg : 0;
	msg->seq_num = offset;
	msg->payload_len = payload_len;
	msg->sender_pg_cookie = xfer->cached_pg->main_pg_cookie;
	msg->receiver_pg_cookie = xfer->cached_pg->known_page->donor_pg_cookie;
	msg->hack = xfer->hack;
	spin_unlock(&xfer->xfer_lock);

	void *payload = ((u8 *)msg) + sizeof(*msg);
	memcpy(payload, data + offset, payload_len);

	spin_lock(&xfer->xfer_lock);
	set_bit(offset / seg_len, xfer->sent_bitmap);
	spin_unlock(&xfer->xfer_lock);

	int ret = tx_msg(xfer, tx_buf);

	return ret;
}

static void remote_numa_retry_xfers(struct work_struct *work)
{
	if (hash_empty(xfer_table)) {
		schedule_delayed_work(&remote_numa_retry_work,
			msecs_to_jiffies(retry_interval_ms));
		return;
	}

	u64 now_ns = ktime_get_ns();
	main_xfer_state_t *xfer;
	main_xfer_state_t *tmp_xfer;
	int bkt;
	LIST_HEAD(work_list);
	LIST_HEAD(candidates);

	/*
	 * Phase 1: walk the table under RCU and grab a reference on every donor
	 * xfer. Keep this section short - no bitmap scans, no allocation. The
	 * reference keeps each xfer alive for the rest of this worker even if it
	 * is concurrently removed from the table and freed.
	 */
	rcu_read_lock();
	hash_for_each_rcu(xfer_table, bkt, xfer, node) {
		if (xfer->is_main_node)
			continue;
		/*
		 * Take a reference that keeps the xfer alive through phase 2.
		 * inc_not_zero: if a concurrent completion already dropped the
		 * last reference, the xfer is being reclaimed and we skip it.
		 */
		if (!refcount_inc_not_zero(&xfer->refcnt))
			continue;
		list_add(&xfer->retry_link, &candidates);
	}
	rcu_read_unlock();

	/*
	 * Phase 2: process the collected donor xfers. We hold a reference on
	 * each, so they cannot be freed under us. The per-xfer lock guards the
	 * mutable fields; the table lock is only taken briefly to remove a
	 * finished or timed-out xfer.
	 */
	list_for_each_entry_safe(xfer, tmp_xfer, &candidates, retry_link) {
		bool complete, timed_out, due;

		spin_lock(&xfer->xfer_lock);
		complete = xfer_compute_max_contig(xfer) >= PAGE_SIZE;
		timed_out = xfer->retry_count >= max_retry_count;
		due = ktime_to_ns(xfer->retry_deadline) < now_ns;
		spin_unlock(&xfer->xfer_lock);

		if (complete || timed_out) {
			bool removed = false;

			if (timed_out)
				printk(KERN_WARNING "remote_numa: donor xfer timeout\n");
			spin_lock(&xfer_table_lock);
			if (!hlist_unhashed(&xfer->node)) {
				hash_del_rcu(&xfer->node);
				removed = true;
			}
			spin_unlock(&xfer_table_lock);
			/* Drop our phase-1 ref; also the table ref if we removed it. */
			xfer_put(xfer);
			if (removed)
				xfer_put(xfer);
			continue;
		}

		if (!due) {
			/* Not due yet; drop our phase-1 reference. */
			xfer_put(xfer);
			continue;
		}

		struct retry_work_item *item = kmalloc(sizeof(*item), GFP_ATOMIC);
		if (item) {
			/* item lives beyond this iteration; take an extra ref. */
			refcount_inc(&xfer->refcnt);
			item->xfer = xfer;
			item->seg_len = xfer_state_max_payload(xfer) - sizeof(remote_numa_mem_pg_xfer_t);
			list_add(&item->list, &work_list);
		}

		spin_lock(&xfer->xfer_lock);
		xfer->retry_count++;
		u64 base_interval = ((u64)retry_interval_ms * NSEC_PER_MSEC) << xfer->retry_count;
		u64 jitter_ns = get_random_u32() % (base_interval / 10);
		xfer->retry_deadline = ktime_add_ns(ktime_get(), base_interval + jitter_ns);
		spin_unlock(&xfer->xfer_lock);

		/* Drop our phase-1 reference. */
		xfer_put(xfer);
	}

	struct retry_work_item *item, *item_tmp;
	list_for_each_entry_safe(item, item_tmp, &work_list, list) {
		for (u32 offset = 0; offset < PAGE_SIZE; offset += item->seg_len) {
			if (!test_bit(offset / item->seg_len, item->xfer->sent_bitmap))
				continue;
			if (test_bit(offset, item->xfer->received_bitmap))
				continue;

			remote_numa_send_segment(item->xfer, offset, item->seg_len);
		}
		list_del(&item->list);
		xfer_put(item->xfer);
		kfree(item);
	}

	schedule_delayed_work(&remote_numa_retry_work,
		msecs_to_jiffies(retry_interval_ms));
}

static void remote_numa_start_retry_worker(void)
{
	INIT_DELAYED_WORK(&remote_numa_retry_work, remote_numa_retry_xfers);
	schedule_delayed_work(&remote_numa_retry_work, msecs_to_jiffies(1));
}

static void remote_numa_stop_retry_worker(void)
{
	cancel_delayed_work_sync(&remote_numa_retry_work);
}

bool remote_numa_transport_is_transfer_complete(
	struct remote_numa_cached_page *cached_target)
{
	atomic_inc(&rn_tc_call);
	main_xfer_state_t *xfer = xfer_get(cached_target->xfer_cookie);
	if (!xfer) {
		bool r = atomic_read(&cached_target->transfer_in_progress) == 0;
		atomic_inc(&rn_tc_no_xfer);
		if (r)
			atomic_inc(&rn_tc_no_xfer_done);
		else
			atomic_inc(&rn_tc_no_xfer_wip);
		return r;
	}
	atomic_inc(&rn_tc_xfer_present);
	{
		unsigned long mc;
		bool done;
		bool stale;
		spin_lock(&xfer->xfer_lock);
		mc = xfer_compute_max_contig(xfer);
		done = mc >= PAGE_SIZE;
		stale = (xfer->cached_pg != cached_target);
		spin_unlock(&xfer->xfer_lock);
		if (done)
			atomic_inc(&rn_tc_xfer_done);
		else {
			atomic_inc(&rn_tc_xfer_notdone);
			atomic_long_set(&rn_tc_xfer_last_mc, (long)mc);
		}
		if (stale) {
			atomic_inc(&rn_tc_stale);
			if (done)
				atomic_inc(&rn_tc_stale_done);
			else
				atomic_inc(&rn_tc_stale_notdone);
		}
		if (done) {
			/*
			 * Remove the completed main-side xfer from the table,
			 * mirroring the alloc path
			 * (remote_numa_check_transfer_complete). Otherwise the xfer
			 * leaks and a reused entry address can make xfer_get()
			 * return a stale completed xfer, causing a
			 * premature-completion data corruption on a later transfer
			 * of the same page.
			 */
			bool removed = false;
			spin_lock(&xfer_table_lock);
			if (!hlist_unhashed(&xfer->node)) {
				hash_del_rcu(&xfer->node);
				removed = true;
			}
			spin_unlock(&xfer_table_lock);
			if (removed) {
				atomic_inc(&rn_tc_removed);
				if (xfer->cached_pg->xfer_cookie == xfer->lookup_cookie)
					xfer->cached_pg->xfer_cookie = 0;
				xfer_put(xfer); /* drop the table's reference */
			} else {
				atomic_inc(&rn_tc_removed_skip);
			}
		}
		xfer_put(xfer); /* drop our lookup reference */
		return done;
	}
}

/* Pure lookup: caller must hold rcu_read_lock() */
static remote_numa_node_t *__remote_numa_get_node(struct hlist_head *table,
		       unsigned int table_bits,
		       u32 node_id)
{
	remote_numa_node_t *node;
	struct hlist_head *head = &table[hash_min(node_id, table_bits)];

	hlist_for_each_entry_rcu(node, head, hnode) {
		if (node->node_id == node_id)
			return node;
	}
	return NULL;
}

static remote_numa_node_t *__remote_numa_get_node_locking(struct hlist_head *table,
			       unsigned int table_bits,
			       u32 node_id)
{
	remote_numa_node_t *node;

	rcu_read_lock();
	node = __remote_numa_get_node(table, table_bits, node_id);
	rcu_read_unlock();

	return node;
}

static void remote_numa_send_all_segments(main_xfer_state_t *xfer, u16 seg_len)
{
	for (u32 offset = 0; offset < PAGE_SIZE; offset += seg_len) {
		remote_numa_send_segment(xfer, offset, seg_len);
	}
}

static int remote_numa_rx_mem_pg_refetch(
	struct remote_numa_donor_trprt_if *donor_if,
	remote_numa_mem_refetch_t *refetch,
	u32 main_node_id)
{
	atomic_inc(&rn_dnh_enter);

	struct remote_numa_mem_mgr *mgr = donor_if->trprt_ctx->mem;
	if (!mgr)
		return -EIO;

	void *page_ptr;
	remote_numa_page_t *rn_pg;
	struct remote_numa_cached_page *cached_pg;
	if (remote_numa_mem_lookup_page(mgr, refetch->donor_pg_cookie,
	                                 &page_ptr, &rn_pg) != 0)
	{
		atomic_inc(&rn_dnh_noent);
		return -ENOENT;
	}
	cached_pg = &rn_pg->cached_pg;

	struct page *pg = virt_to_page(page_ptr);
	if (!pg)
	{
		return -EIO;
	}

	/* Track whether the donor is sending a zeroed page (corruption indicator). */
	atomic_inc(&rn_dnh_send);
	if (((u8 *)page_ptr)[0] == 0)
		atomic_inc(&rn_dnh_first_zero);

	remote_numa_node_t *main_node = __remote_numa_get_node_locking(
		donor_if->trprt_ctx->node_table,
		REMOTE_NUMA_HASH_TABLE_ORDER,
		main_node_id);
	if (!main_node)
	{
		return -ENOENT;
	}

	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer)
		return -ENOMEM;
	refcount_set(&xfer->refcnt, 1);
	spin_lock_init(&xfer->xfer_lock);

	// TODO, this is super confusing, need to rename some fields.
	// These donor/main pg cookies need to be backwards since
	// they're referred to by donor/main name in the send* function.
	
	cached_pg->main_pg_cookie = rn_pg->donor_pg_cookie;
	cached_pg->known_page->donor_pg_cookie = refetch->main_pg_cookie;
	cached_pg->known_page->donor_id = refetch->hdr.main_cookie; //gross. broke an abstraction.
	cached_pg->known_page->donor_cookie = refetch->hdr.donor_cookie;

	cached_pg->known_page->mm = NULL;
	cached_pg->known_page->addr = 0;

	/*
	 * The xfer is private (not yet in xfer_table), so initialize its fields
	 * without a lock. Only the global hack counter and the table insertion
	 * require the table lock. The wmb + table lock publish the fully
	 * initialized xfer to concurrent xfer_get() lookups.
	 */
	xfer->cached_pg		= cached_pg;
	xfer->lookup_cookie	= refetch->donor_pg_cookie;
	xfer->hdr_main_cookie	= cached_pg->known_page->donor_id;
	xfer->hdr_donor_cookie	= cached_pg->known_page->donor_cookie;
	xfer->target		= pg;
	xfer->transfer_type	= remote_numa_mem_refetch_sat;
	xfer->is_main_node	= false;
	xfer->donor_trprt	= donor_if;
	xfer->return_info	= main_node->priv_return_info;
	xfer->last_update	= ktime_get();
	init_waitqueue_head(&xfer->waitq);
	bitmap_zero(xfer->sent_bitmap, PAGE_SIZE);
	bitmap_zero(xfer->received_bitmap, PAGE_SIZE);
	u64 jitter_ns = get_random_u32() % (((u64)retry_interval_ms * NSEC_PER_MSEC) / 10);
	xfer->retry_deadline	= ktime_add_ns(ktime_get(),
					   ((u64)retry_interval_ms * NSEC_PER_MSEC) + jitter_ns);
	xfer->retry_count	= 0;
	smp_wmb();

	xfer->hack		= atomic_inc_return(&hack);
	spin_lock(&xfer_table_lock);
	hash_add_rcu(xfer_table, &xfer->node,
		 xfer_hash(refetch->donor_pg_cookie));
	spin_unlock(&xfer_table_lock);

	u16 seg_len = donor_if->get_max_payload_len() -
		      sizeof(remote_numa_mem_pg_xfer_t);
	remote_numa_send_all_segments(xfer, seg_len);

	return 0;
}

int remote_numa_transport_alloc_page_async(
	struct remote_numa_main_trprt_if *trprt,
	remote_numa_node_t *donor,
	struct remote_numa_cached_page *cached_target)
{
	u64 main_pg_cookie = (uintptr_t)cached_target;
	struct page *target = cached_target->known_page->page;
	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_ATOMIC);
	void *tx_buf;
	remote_numa_mem_alloc_t *req;
	void **v = (void **)&req;
	int xfer_hack;

	if (!xfer) {
		printk(KERN_DEBUG "Could not alloc xfer state.\n");
		return -ENOMEM;
	}
	refcount_set(&xfer->refcnt, 1);
	spin_lock_init(&xfer->xfer_lock);

	xfer->hack = atomic_inc_return(&hack);
	xfer_hack = xfer->hack;

	/* Prepare xfer state before publishing it in xfer_table. */
	xfer->cached_pg = cached_target;
	xfer->lookup_cookie = xfer->hack;
	cached_target->xfer_cookie = xfer->hack;
	xfer->cached_pg->known_page->donor_pg_cookie = 0;
	xfer->cached_pg->main_pg_cookie = main_pg_cookie;
	/* Keep a copy on the known_page so future frees can always recover it. */
	xfer->cached_pg->known_page->main_pg_cookie = main_pg_cookie;
	xfer->cached_pg->known_page->donor_id = donor->node_id;
	xfer->hdr_main_cookie = cached_target->known_page->donor_id;
	xfer->hdr_donor_cookie = cached_target->known_page->donor_cookie;
	xfer->target = target;
	xfer->transfer_type = remote_numa_mem_alloc;
	xfer->last_update = ktime_get();
	xfer->main_trprt = trprt;
	xfer->is_main_node = true;
	xfer->return_info = donor->priv_return_info;
	init_waitqueue_head(&xfer->waitq);
	bitmap_zero(xfer->received_bitmap, PAGE_SIZE);
	bitmap_zero(xfer->sent_bitmap, PAGE_SIZE);
	{
		u64 jitter_ns = get_random_u32() % (((u64)retry_interval_ms * NSEC_PER_MSEC) / 10);
		xfer->retry_deadline = ktime_add_ns(ktime_get(),
					  ((u64)retry_interval_ms * NSEC_PER_MSEC) + jitter_ns);
	}
	xfer->retry_count = 0;

	/*
	 * The xfer is private here (fields set above); only the table insertion
	 * needs the table lock. Do not hold a spinlock across
	 * alloc_tx_buffer() or tx_msg().
	 */
	spin_lock(&xfer_table_lock);
	hash_add_rcu(xfer_table, &xfer->node, xfer_hash(xfer->hack));
	spin_unlock(&xfer_table_lock);

	smp_wmb();
	trprt->alloc_tx_buffer(sizeof(*req), &tx_buf, v);
	if (!tx_buf || !req) {
		cached_target->xfer_cookie = 0;
		spin_lock(&xfer_table_lock);
		hash_del_rcu(&xfer->node);
		spin_unlock(&xfer_table_lock);
		xfer_put(xfer);
		return -ENOMEM;
	}

	req->hdr.version = remote_numa_protocol_0_1;
	req->hdr.type = remote_numa_mem_alloc;
	req->hdr.main_cookie = donor->node_id;
	req->hdr.donor_cookie = donor->donor_cookie;
	req->main_pg_cookie = xfer->hack;
	req->hack = xfer_hack;

	if (trprt->tx_msg(trprt->trprt_ctx, donor->priv_return_info, tx_buf)) {
		cached_target->xfer_cookie = 0;
		spin_lock(&xfer_table_lock);
		hash_del_rcu(&xfer->node);
		spin_unlock(&xfer_table_lock);
		xfer_put(xfer);
		return -EIO;
	}

	/* Return immediately - caller must check completion later */
	return 0;
}

int remote_numa_transport_refetch_page_async(
	struct remote_numa_main_trprt_if *trprt,
	u32 donor_node_id,
	u64 donor_pg_cookie,
	struct remote_numa_cached_page *cached_target)
{
	atomic_inc(&rn_mrs_send);

	remote_numa_node_t *donor = NULL;

	rcu_read_lock();
	hash_for_each_possible_rcu(trprt->trprt_ctx->node_table, donor, hnode, donor_node_id) {
		if (donor->node_id == donor_node_id)
			break;
		donor = NULL;
	}
	if (!donor)
		printk(KERN_WARNING "no donor found for donor_node_id %u\n", donor_node_id);
	rcu_read_unlock();

	if (!donor)
		return -ENODEV;

	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_ATOMIC);
	if (!xfer)
		return -ENOMEM;
	refcount_set(&xfer->refcnt, 1);
	spin_lock_init(&xfer->xfer_lock);

	xfer->cached_pg = cached_target;
	xfer->cached_pg->known_page->donor_pg_cookie = donor_pg_cookie;
	xfer->cached_pg->known_page->donor_id = donor_node_id;
	xfer->hdr_main_cookie = cached_target->known_page->donor_id;
	xfer->hdr_donor_cookie = cached_target->known_page->donor_cookie;
	xfer->target = cached_target->known_page->page;
	xfer->transfer_type = remote_numa_mem_refetch;
	xfer->last_update = ktime_get();
	xfer->cached_pg->main_pg_cookie = (uintptr_t)cached_target;
	xfer->cached_pg->known_page->main_pg_cookie = xfer->cached_pg->main_pg_cookie;
	xfer->hack = atomic_inc_return(&hack);
	xfer->lookup_cookie = xfer->hack;
	cached_target->xfer_cookie = xfer->hack;
	xfer->main_trprt = trprt;
	xfer->is_main_node = true;
	xfer->return_info = donor->priv_return_info;
	init_waitqueue_head(&xfer->waitq);
	bitmap_zero(xfer->sent_bitmap, PAGE_SIZE);
	bitmap_zero(xfer->received_bitmap, PAGE_SIZE);
	xfer->retry_deadline = ktime_add_ns(ktime_get(), ((u64)retry_interval_ms * NSEC_PER_MSEC));
	xfer->retry_count = 0;

	smp_wmb();
	spin_lock(&xfer_table_lock);
	hash_add_rcu(xfer_table, &xfer->node, xfer_hash(xfer->hack));
	spin_unlock(&xfer_table_lock);

	void *tx_buf;
	remote_numa_mem_refetch_t *refetch;
	void **v = (void **)&refetch;
	trprt->alloc_tx_buffer(sizeof(*refetch), &tx_buf, v);
	if (!tx_buf || !refetch) {
		cached_target->xfer_cookie = 0;
		spin_lock(&xfer_table_lock);
		hash_del_rcu(&xfer->node);
		spin_unlock(&xfer_table_lock);
		xfer_put(xfer);
		return -ENOMEM;
	}

	refetch->hdr.version       = remote_numa_protocol_0_1;
	refetch->hdr.type          = remote_numa_mem_refetch;
	refetch->hdr.main_cookie   = donor->node_id;
	refetch->hdr.donor_cookie  = donor->donor_cookie;
	refetch->donor_pg_cookie   = donor_pg_cookie;
	refetch->main_pg_cookie    = xfer->hack;

	if (trprt->tx_msg(trprt->trprt_ctx, donor->priv_return_info, tx_buf)) {
		cached_target->xfer_cookie = 0;
		spin_lock(&xfer_table_lock);
		hash_del_rcu(&xfer->node);
		spin_unlock(&xfer_table_lock);
		xfer_put(xfer);
		return -EIO;
	}

	/* Return immediately - caller polls for completion */
	return 0;
}

int remote_numa_rx_mem_pg_sync_xfer(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_pg_xfer_t *xfer)
{
	return remote_numa_rx_mem_pg_sync_xfer_from(donor_if, xfer, xfer->hdr.donor_cookie);
}

static int remote_numa_rx_mem_pg_sync_xfer_from(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_pg_xfer_t *xfer,
	u32 main_node_id)
{
	struct remote_numa_mem_mgr *mgr = donor_if->trprt_ctx->mem;
	if (!mgr)
		return -EIO;

	if (xfer->payload_len + xfer->seq_num > PAGE_SIZE)
		return -EOVERFLOW;

	/*
	 * Best-effort sync: if the receiver cookie is unknown or the page has
	 * already been freed on the donor, skip the copy but still send an ACK
	 * so the main side can complete its xfer state. This makes mem_sync
	 * more tolerant of races and retries, similar to mem_free.
	 */
	void *dst = NULL;
	remote_numa_page_t *rn_pg = NULL;
	int lookup_ret = remote_numa_mem_lookup_page(mgr, xfer->receiver_pg_cookie,
					      &dst, &rn_pg);
	if (!lookup_ret) {
		void *payload = ((u8 *)xfer) + sizeof(*xfer);
		memcpy(dst + xfer->seq_num, payload, xfer->payload_len);
	} else {
		atomic_inc(&rn_sync_skip);
	}

	void *ack_buf;
	remote_numa_mem_pg_xfer_ack_t *ack;
	void **v = (void **)&ack;

	donor_if->alloc_tx_buffer(sizeof(*ack), &ack_buf, v);
	if (!ack_buf || !ack)
		return -ENOMEM;

	ack->hdr.version        = remote_numa_protocol_0_1;
	ack->hdr.type           = remote_numa_mem_sync_ack;
	ack->hdr.main_cookie    = xfer->hdr.main_cookie;
	ack->hdr.donor_cookie   = xfer->hdr.donor_cookie;
	ack->bottom_seq_num     = xfer->seq_num;
	ack->top_seq_num        = xfer->seq_num + xfer->payload_len;
	ack->sender_pg_cookie   = xfer->receiver_pg_cookie;
	ack->receiver_pg_cookie = xfer->sender_pg_cookie;
	ack->hack               = xfer->hack;

	remote_numa_node_t *main_node = __remote_numa_get_node_locking(
		donor_if->trprt_ctx->node_table,
		REMOTE_NUMA_HASH_TABLE_ORDER,
		main_node_id);
	if (!main_node)
		return -ENOENT;

	int ret = donor_if->tx_msg(donor_if->trprt_ctx,
	                        main_node->priv_return_info,
	                        ack_buf) == 0
	       ? 0
	       : -EIO;

	return ret;
}

int remote_numa_rx_mem_pg_sat_ack(
	struct remote_numa_main_trprt_if *main_if,
	remote_numa_mem_satisfaction_t *ack)
{
	main_xfer_state_t *xfer = xfer_get(ack->main_pg_cookie);
	/*
	 * Best-effort ack handling:
	 * - If we can't find the xfer, or the hack tag doesn't match, just
	 *   drop the ACK. This avoids spurious bad_cookie errors when the
	 *   main side has already timed out or retired the xfer.
	 */
	if (!xfer) {
		atomic_inc(&rn_ack_drop_nf);
		return 0;
	}
	if (ack->hack != xfer->hack) {
		atomic_inc(&rn_ack_drop_hack);
		xfer_put(xfer);
		return 0;
	}

	/* mark done and publish donor cookie */
	spin_lock(&xfer->xfer_lock);
	bitmap_fill(xfer->received_bitmap, PAGE_SIZE);
	xfer->cached_pg->known_page->donor_pg_cookie = ack->donor_pg_cookie;
	smp_wmb();
	spin_unlock(&xfer->xfer_lock);
	wake_up(&xfer->waitq);
	xfer_put(xfer);
	return 0;
}

int remote_numa_rx_mem_pg_sync_ack(
	struct remote_numa_main_trprt_if *main_if,
	remote_numa_mem_pg_xfer_ack_t *ack)
{
	main_xfer_state_t *xfer = xfer_get(ack->receiver_pg_cookie);

	if (!xfer)
		return -ENOENT;

	bool done;
	spin_lock(&xfer->xfer_lock);
	bitmap_set(xfer->received_bitmap, ack->bottom_seq_num,
		   (ack->top_seq_num - ack->bottom_seq_num));
	done = xfer_compute_max_contig(xfer) >= PAGE_SIZE;
	spin_unlock(&xfer->xfer_lock);

	if (done)
	{
		wake_up(&xfer->waitq);
	}
	xfer_put(xfer);
	return 0;
}

int remote_numa_tx_mem_pg_sync_xfer_async(
	struct remote_numa_main_trprt_if *main_if,
	u64 donor_pg_cookie,
	struct page *pg,
	struct remote_numa_cached_page *victim)
{
	if (!main_if || !pg || !victim)
		return -EIO;
	remote_numa_node_t *donor = __remote_numa_get_node_locking(
		main_if->trprt_ctx->node_table,
		REMOTE_NUMA_HASH_TABLE_ORDER,
				victim->known_page->donor_id);
	if (!donor)
		return -ENODEV;

	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_ATOMIC);
	if (!xfer)
		return -ENOMEM;
	refcount_set(&xfer->refcnt, 1);
	spin_lock_init(&xfer->xfer_lock);

	/* The xfer is private (not yet in xfer_table); init fields unlocked. */
	xfer->cached_pg = victim;
	xfer->lookup_cookie = (uintptr_t)victim;
	victim->xfer_cookie = (uintptr_t)victim;
	xfer->hdr_main_cookie = victim->known_page->donor_id;
	xfer->hdr_donor_cookie = victim->known_page->donor_cookie;
	xfer->target = pg;
	xfer->transfer_type = remote_numa_mem_sync;
	xfer->last_update = ktime_get();
	xfer->main_trprt = main_if;
	xfer->is_main_node = true;
	xfer->return_info = donor->priv_return_info;
	init_waitqueue_head(&xfer->waitq);
	bitmap_zero(xfer->sent_bitmap, PAGE_SIZE);
	bitmap_zero(xfer->received_bitmap, PAGE_SIZE);
	xfer->retry_deadline = ktime_add_ns(ktime_get(), ((u64)retry_interval_ms * NSEC_PER_MSEC));
	xfer->retry_count = 0;

	u16 seg_len = main_if->get_max_payload_len() - sizeof(remote_numa_mem_pg_xfer_t);

	smp_wmb();

	xfer->hack = atomic_inc_return(&hack);
	spin_lock(&xfer_table_lock);
	hash_add_rcu(xfer_table, &xfer->node, xfer_hash((uintptr_t)victim));
	spin_unlock(&xfer_table_lock);

	remote_numa_send_all_segments(xfer, seg_len);

	/* Return immediately - transfer will complete async */
	return 0;
}

/* Check if transfer is complete. Returns 0 if done, -EAGAIN if in progress, <0 on error */
int remote_numa_check_transfer_complete(struct remote_numa_cached_page *cached_pg)
{
	main_xfer_state_t *xfer = xfer_get(cached_pg->xfer_cookie);
	if (!xfer) {
		/* Missing xfer state while caller still tracks a page: treat as error */
		return -ENOENT;
	}

	/* Check if transfer is complete */
	bool done;
	spin_lock(&xfer->xfer_lock);
	done = xfer_compute_max_contig(xfer) >= PAGE_SIZE;
	spin_unlock(&xfer->xfer_lock);

	if (done) {
		/* Transfer complete - clean up */
		bool removed = false;
		spin_lock(&xfer_table_lock);
		if (!hlist_unhashed(&xfer->node)) {
			hash_del_rcu(&xfer->node);
			removed = true;
		}
		spin_unlock(&xfer_table_lock);
		if (removed && xfer->cached_pg->xfer_cookie == xfer->lookup_cookie)
			xfer->cached_pg->xfer_cookie = 0;
		/* Drop the table's reference (if we removed it). */
		if (removed)
			xfer_put(xfer);
		/* Drop our lookup reference. */
		xfer_put(xfer);
		return 0;
	}

	/* Still in progress */
	xfer_put(xfer);
	return -EAGAIN;
}

remote_numa_trprt_ctx_t *remote_numa_make_trprt_ctx(struct remote_numa_mem_mgr *mem)
{
	remote_numa_trprt_ctx_t *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	hash_init(xfer_table);
	hash_init(ctx->node_table);

	spin_lock_init(&ctx->hash_write_lock);

	ctx->mem = mem;
	ctx->trprt_ctx = NULL;

	remote_numa_start_retry_worker();

	return ctx;
}

void remote_numa_transport_ctx_destroy(remote_numa_trprt_ctx_t *ctx)
{
	if (!ctx)
		return;
	u32 bkt;
	remote_numa_node_t *node;
	synchronize_rcu();
	rcu_read_lock();
	hash_for_each_rcu(ctx->node_table, bkt, node, hnode) {
		hash_del_rcu(&node->hnode);
		kfree(node->priv_return_info);
		kfree(node);
	}

	// XXX free the xfer_table nodes if needed.

	remote_numa_stop_retry_worker();

	rcu_read_unlock();
	synchronize_rcu();
	kfree(ctx);
}

int remote_numa_main_rx(
	remote_numa_main_trprt_if_t *main_if, void *rx_data, void *payload)
{
	int ret = 0;
	u8 hdr_type = 0;

	remote_numa_msg_hdr_t *hdr = payload;
	if (hdr->version != REMOTE_NUMA_SUPPORTED_PROTO)
	{
		ret = -EPROTO;
		goto done;
	}
	hdr_type = hdr->type;

	switch(hdr->type)
	{
	case remote_numa_eth_advert:
		ret = remote_numa_rx_advert(main_if, payload);
		goto done;
	case remote_numa_mem_resp:
		ret = remote_numa_rx_mem_resp(main_if, payload);
		goto done;
	case remote_numa_mem_sync_ack:
		ret = remote_numa_rx_mem_pg_sync_ack(main_if, payload);
		goto done;
	case remote_numa_mem_free_ack:
		ret = remote_numa_rx_mem_pg_free_ack(main_if, payload);
		goto done;
	case remote_numa_mem_sat_ack:
		ret = remote_numa_rx_mem_pg_sat_ack(main_if, payload);
		goto done;
	case remote_numa_mem_refetch_sat:
		ret = remote_numa_rx_mem_pg_refetch_sat(main_if, payload);
		goto done;
	default:
		ret = -EBADMSG;
		printk(KERN_WARNING "error processing pkt... unknown type\n");
		goto done;
	}
done:
	main_if->free_rx_buff(rx_data);	
	/* hdr/payload are no longer valid after free_rx_buff */
	if (ret) {
		printk(KERN_WARNING "error processing main pkt type=%u ret=%d\n",
		       hdr_type, ret);
	}
	return ret;
}

int remote_numa_donor_rx(
	remote_numa_donor_trprt_if_t *donor_if, void *rx_data, void *payload)
{
	int ret = 0;
	u32 src_node_id = 0;
	u8 hdr_type = 0;

	if (donor_if->remote_numa_rx_node_id)
		src_node_id = donor_if->remote_numa_rx_node_id(rx_data, payload);

	remote_numa_msg_hdr_t *hdr = payload;
	if (hdr->version != REMOTE_NUMA_SUPPORTED_PROTO)
	{
		ret = -EPROTO;
		goto done;
	}
	hdr_type = hdr->type;

	switch(hdr->type)
	{
	case remote_numa_mem_query:
		ret = remote_numa_rx_mem_query(donor_if, payload);
		goto done;
	case remote_numa_mem_alloc:
		ret = remote_numa_rx_mem_alloc_from(donor_if, payload, src_node_id);
		goto done;
	case remote_numa_mem_sync:
		ret = remote_numa_rx_mem_pg_sync_xfer_from(donor_if, payload, src_node_id);
		goto done;
	case remote_numa_mem_free:
		ret = remote_numa_rx_mem_pg_free_from(donor_if, payload, src_node_id);
		goto done;
	case remote_numa_mem_refetch:
		ret = remote_numa_rx_mem_pg_refetch(donor_if, payload, src_node_id);
		goto done;
	case remote_numa_mem_refetch_ack:
		ret = remote_numa_rx_mem_pg_refetch_ack(donor_if, payload);
		goto done;
	default:
		ret = -EBADMSG;
		printk(KERN_WARNING "error processing pkt... unknown type\n");
		goto done;
	}
done:
	donor_if->free_rx_buff(rx_data);

	if (ret) {
		printk(KERN_WARNING "error processing pkt type=%u ret=%d\n",
		       hdr_type, ret);
	}

	return ret;
}

int remote_numa_rx_advert(
	remote_numa_main_trprt_if_t *main_if,
	remote_numa_advert_t *advert)
{
	u32 node_id = main_if->remote_numa_node_id(advert);
	remote_numa_node_t *node = __remote_numa_get_or_add_node(
		main_if->trprt_ctx,
		REMOTE_NUMA_HASH_TABLE_ORDER,
		&main_if->trprt_ctx->hash_write_lock,
		node_id,
		(void *(*)(void *))main_if->priv_return_info_from_advert,
		advert);

	if (!node)
		return -ENOMEM;
	void *tx_buffer_start;
	remote_numa_mem_query_t *query;
	void **v_query = (void **)&query;
	main_if->alloc_tx_buffer(sizeof(remote_numa_mem_query_t),
		&tx_buffer_start, v_query);
	if (tx_buffer_start == NULL || query == NULL)
	{
		// TODO make file id for each return code to use.
		return -ENOMEM;
	}
	query->hdr.version = remote_numa_protocol_0_1;
	query->hdr.type = remote_numa_mem_query;
	query->hdr.main_cookie = node_id;
	query->hdr.donor_cookie = 0;
	/* XXX need a way to handle this return code */
	main_if->priv_return_info(main_if->trprt_ctx->trprt_ctx,
		&query->return_info);
	/* 
 	 * N.B., we are not accessing anything here that would be written
 	 * after init, so we do not lock the node.
 	 */	
	return main_if->tx_msg(main_if->trprt_ctx,
		node->priv_return_info, tx_buffer_start) == 0 ?
		0 : -EIO;
}

int remote_numa_rx_mem_query(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_query_t *query)
{
	u32 node_id = query->hdr.donor_cookie ?
		query->hdr.donor_cookie : donor_if->remote_numa_node_id(query);
	remote_numa_node_t *node = __remote_numa_get_or_add_node(
		donor_if->trprt_ctx,
		REMOTE_NUMA_HASH_TABLE_ORDER,
		&donor_if->trprt_ctx->hash_write_lock,
		node_id,
		(void *(*)(void *))donor_if->priv_return_info_from_mem_query,
		query);

	if (!node)
		return -ENOMEM;

	void *tx_buffer_start;
	remote_numa_mem_resp_t *resp;
	void **v_resp = (void **)&resp;
	donor_if->alloc_tx_buffer(sizeof(remote_numa_mem_resp_t),
		&tx_buffer_start, v_resp);
	if (tx_buffer_start == NULL || resp == NULL)
	{
		// TODO make file id for each return code to use.
		return -ENOMEM;
	}
	resp->hdr.version = remote_numa_protocol_0_1;
	resp->hdr.type = remote_numa_mem_resp;
	resp->hdr.main_cookie = query->hdr.main_cookie;
	resp->hdr.donor_cookie = node_id;

	// TODO XXX this should be under rcu lock

	resp->page_size_rank =
		donor_if->trprt_ctx->mem->page_size_rank;
	resp->free_pages = donor_if->trprt_ctx->mem->free_pages;

	/* 
 	 * N.B., we are not accessing anything here that would be written
 	 * after init, so we do not lock the node.
 	 */	
	return donor_if->tx_msg(donor_if->trprt_ctx,
		node->priv_return_info, tx_buffer_start) == 0 ?
		0 : -EIO;
}

int remote_numa_rx_mem_resp(
	remote_numa_main_trprt_if_t *main_if,
	remote_numa_mem_resp_t *resp)
{
	remote_numa_node_t *node = __remote_numa_get_node_locking(
		main_if->trprt_ctx->node_table,
		REMOTE_NUMA_HASH_TABLE_ORDER, resp->hdr.main_cookie);
	spin_lock(&node->node_lock);
	node->free_pages = resp->free_pages;
	node->page_size_rank = resp->page_size_rank;
	node->valid_mem_resp = true;
	node->donor_cookie = resp->hdr.donor_cookie;
	spin_unlock(&node->node_lock);

	printk(KERN_INFO "remote_numa: mem_resp from node=%u free_pages=%u page_size_rank=%u (valid_mem_resp now set)\n",
	       node->node_id, resp->free_pages, resp->page_size_rank);
	return 0;
}

int remote_numa_rx_mem_alloc(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_alloc_t *alloc)
{
	return remote_numa_rx_mem_alloc_from(donor_if, alloc, alloc->hdr.donor_cookie);
}

static int remote_numa_rx_mem_alloc_from(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_alloc_t *alloc,
	u32 main_node_id)
{
    u64 cookie;
    void *page;
    struct remote_numa_mem_mgr *mgr = donor_if->trprt_ctx->mem;

    if (!mgr) {
        printk(KERN_DEBUG "No mem manager in donor.\n");
		return -EIO;
    }

    if (remote_numa_mem_alloc_page(mgr, &cookie, &page) != 0) {
        atomic_inc(&rn_dmf_fail);
		return -ENOMEM;
    }

    void *tx_buf;
    remote_numa_mem_satisfaction_t *ack;
    void **v = (void **)&ack;

    donor_if->alloc_tx_buffer(sizeof(*ack), &tx_buf, v);
    if (!tx_buf || !ack) {
        printk(KERN_DEBUG "Bad tx alloc manager in donor.\n");
        remote_numa_mem_free_page(mgr, cookie); // Return page to pool
		return -ENOMEM;
    }

    ack->hdr.version = remote_numa_protocol_0_1;
    ack->hdr.type = remote_numa_mem_sat_ack;
    ack->hdr.main_cookie = alloc->hdr.main_cookie;
    ack->hdr.donor_cookie = donor_if->trprt_ctx->mem->cookie_gen.counter;
    ack->main_pg_cookie = alloc->main_pg_cookie;
    ack->donor_pg_cookie = cookie;
    ack->hack = alloc->hack;

	remote_numa_node_t *main_node = __remote_numa_get_node_locking(
		donor_if->trprt_ctx->node_table,
		REMOTE_NUMA_HASH_TABLE_ORDER,
		main_node_id);
	if (!main_node)
		return -ENOENT;

    // Send the ack
    return donor_if->tx_msg(
               donor_if->trprt_ctx,
               main_node->priv_return_info,
               tx_buf) == 0
	       ? 0
	       : -EIO;
}

int remote_numa_rx_mem_pg_free(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_free_t *pg_free)
{
	return remote_numa_rx_mem_pg_free_from(donor_if, pg_free, pg_free->hdr.donor_cookie);
}

static int remote_numa_rx_mem_pg_free_from(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_free_t *pg_free,
	u32 main_node_id)
{
	/*
	 * Best-effort free: if the cookie is unknown or the page is already
	 * free, treat it as success rather than a protocol error. This makes
	 * mem_free idempotent and avoids noisy "bad cookie" errors when the
	 * main side races or retries.
	 */
	int free_ret = remote_numa_mem_free_page(donor_if->trprt_ctx->mem,
						 pg_free->donor_pg_cookie);
	if (free_ret != 0) {
		/* Optional: debug-only signal of unexpected cookie. */
		pr_debug("remote_numa: mem_free for unknown/idle cookie=%llu ret=%d\n",
			 pg_free->donor_pg_cookie, free_ret);
	}

	// Send ACK
	void *tx_buf;
	remote_numa_mem_free_ack_t *ack;
	void **v = (void **)&ack;

	donor_if->alloc_tx_buffer(sizeof(*ack), &tx_buf, v);
	if (!tx_buf || !ack)
		return -ENOMEM;

	ack->hdr.version        = remote_numa_protocol_0_1;
	ack->hdr.type           = remote_numa_mem_free_ack;
	ack->hdr.main_cookie    = pg_free->hdr.main_cookie;
	ack->hdr.donor_cookie   = pg_free->hdr.donor_cookie;
	ack->main_pg_cookie     = pg_free->main_pg_cookie;

	remote_numa_node_t *main_node = __remote_numa_get_node_locking(
		donor_if->trprt_ctx->node_table,
		REMOTE_NUMA_HASH_TABLE_ORDER,
		main_node_id);
	if (!main_node)
		return -ENOENT;

	return donor_if->tx_msg(donor_if->trprt_ctx,
	                        main_node->priv_return_info,
	                        tx_buf) == 0
	       ? 0
	       : -EIO;
}


int remote_numa_rx_mem_pg_free_ack(
	remote_numa_main_trprt_if_t *main_if,
	remote_numa_mem_free_ack_t *ack)
{
	/*
	 * The main-side free path is fire-and-forget; no xfer is tracked for
	 * mem_free. The ack's main_pg_cookie identifies the entry as it was at
	 * free time, which may since have been reused for a different page's
	 * in-flight xfer, so it must never be used to complete an unrelated
	 * transfer. Ignore the ack.
	 */
	return 0;
}

int remote_numa_tx_mem_pg_free(
	struct remote_numa_main_trprt_if *main_if,
	u32 donor_id,
	u64 donor_pg_cookie,
	uintptr_t main_pg_cookie)
{
	remote_numa_node_t *donor = NULL;
	rcu_read_lock();
	hash_for_each_possible_rcu(main_if->trprt_ctx->node_table, donor, hnode, donor_id) {
		if (donor->node_id == donor_id)
			break;
		donor = NULL;
	}
	rcu_read_unlock();

	if (!donor)
		return -ENODEV;

	/* Fire-and-forget FREE: no xfer_table entry, no wait on ACK. */
	void *tx_buf;
	remote_numa_mem_free_t *msg;
	void **v = (void **)&msg;

	main_if->alloc_tx_buffer(sizeof(*msg), &tx_buf, v);
	if (!tx_buf || !msg)
		return -ENOMEM;

	msg->hdr.version        = remote_numa_protocol_0_1;
	msg->hdr.type           = remote_numa_mem_free;
	msg->hdr.main_cookie    = donor_id;
	msg->hdr.donor_cookie   = donor->donor_cookie;
	msg->donor_pg_cookie    = donor_pg_cookie;
	msg->main_pg_cookie     = main_pg_cookie;

	if (main_if->tx_msg(main_if->trprt_ctx, donor->priv_return_info, tx_buf))
		return -EIO;

	return 0;
}

int remote_numa_rx_mem_pg_refetch_sat(
    remote_numa_main_trprt_if_t *main_if,
    remote_numa_mem_pg_xfer_t *sat)
{
	main_xfer_state_t *xfer = xfer_get(sat->receiver_pg_cookie);
    if (!xfer)
    {
	return 0;
    }
    if (sat->payload_len + sat->seq_num > PAGE_SIZE)
	{
		xfer_put(xfer);
		return -EOVERFLOW;
	}

    /* write segment into local page */
    void *dst = page_address(xfer->target);
    memcpy(dst + sat->seq_num, ((u8 *)sat) + sizeof(*sat), sat->payload_len);

    /* Track whether the main node received a zeroed page (corruption indicator). */
    if (sat->seq_num == 0) {
        atomic_inc(&rn_mrw_write);
        if (((u8 *)dst)[0] == 0)
            atomic_inc(&rn_mrw_first_zero);
    }

    /* mark locally received so waiters can complete */
	spin_lock(&xfer->xfer_lock);
	bitmap_set(xfer->received_bitmap, sat->seq_num, sat->payload_len);
	spin_unlock(&xfer->xfer_lock);

    /* build ACK with bottom/top = [seq, seq+len), like sync */
    void *ack_buf;
    remote_numa_mem_pg_xfer_ack_t *ack;
    void **v = (void **)&ack;
    main_if->alloc_tx_buffer(sizeof(*ack), &ack_buf, v);
	if (!ack_buf || !ack) {
		xfer_put(xfer);
		return -ENOMEM;
	}

    ack->hdr.version        = REMOTE_NUMA_SUPPORTED_PROTO;
    ack->hdr.type           = remote_numa_mem_refetch_ack;
    ack->hdr.main_cookie    = sat->hdr.main_cookie;
    ack->hdr.donor_cookie   = sat->hdr.donor_cookie;
    ack->bottom_seq_num     = sat->seq_num;
    ack->top_seq_num        = sat->seq_num + sat->payload_len;
    ack->sender_pg_cookie   = sat->receiver_pg_cookie; /* main */
    ack->receiver_pg_cookie = sat->sender_pg_cookie;   /* donor */
    ack->hack               = sat->hack;

    remote_numa_node_t *donor = __remote_numa_get_node_locking(
        main_if->trprt_ctx->node_table,
        REMOTE_NUMA_HASH_TABLE_ORDER,
        sat->hdr.main_cookie);

    if (!donor) {
        printk(KERN_ERR "refetch_sat: failed to find donor node %u\n", sat->hdr.main_cookie);
		xfer_put(xfer);
		return -ENOENT;
    }

    /* send ACK back to donor */
    int ret = main_if->tx_msg(main_if->trprt_ctx, donor->priv_return_info, ack_buf);
    if (ret) {
        printk(KERN_ERR "refetch_sat: failed to send ACK, ret=%u\n", ret);
    }

    /* done? wake any waiters */
    bool done;
    spin_lock(&xfer->xfer_lock);
    done = xfer_compute_max_contig(xfer) >= PAGE_SIZE;
    spin_unlock(&xfer->xfer_lock);

    if (done) {
	        wake_up(&xfer->waitq);
	        xfer_put(xfer);
	} else {
	        xfer_put(xfer);
	}

	return 0;
}

int
remote_numa_rx_mem_pg_refetch_ack(struct remote_numa_donor_trprt_if *donor_if,
                                  remote_numa_mem_pg_xfer_ack_t *ack)
{
	main_xfer_state_t *xfer = xfer_get(ack->receiver_pg_cookie);
    /*
     * Best-effort ack handling: if the xfer is already gone (timed out or
     * retired), drop the ACK silently. This avoids spurious ENOENT errors
     * when the main side has already cleaned up the xfer.
     */
    if (!xfer)
		return 0;

    /* Stale in-flight? Keep behavior consistent with sync_ack: log and continue. */
    if (ack->hack != xfer->hack) {
        printk(KERN_INFO "Ignoring refetch_ack due to hack staleness (expected %d, got %d)\n",
               xfer->hack, ack->hack);
		xfer_put(xfer);
		return 0;  // Silently ignore stale acks
    }

    /* Range-based ACK: handle out-of-order arrivals without over-marking. */
    bool done;
    spin_lock(&xfer->xfer_lock);
	bitmap_set(xfer->received_bitmap,
		   ack->bottom_seq_num,
		   (ack->top_seq_num - ack->bottom_seq_num));
    done = xfer_compute_max_contig(xfer) >= PAGE_SIZE;
    spin_unlock(&xfer->xfer_lock);

    /* If the page is fully covered, wake the waiter. */
    if (done) {
	        wake_up(&xfer->waitq);
	        xfer_put(xfer);
	} else {
	        xfer_put(xfer);
	}

	return 0;
}

void tmp_init(void)
{
	spin_lock_init(&xfer_table_lock);
	atomic_set(&hack, 0);
}
EXPORT_SYMBOL_GPL(tmp_init);

EXPORT_SYMBOL_GPL(remote_numa_transport_alloc_page_async);
EXPORT_SYMBOL_GPL(remote_numa_transport_refetch_page_async);
EXPORT_SYMBOL_GPL(remote_numa_transport_is_transfer_complete);
EXPORT_SYMBOL_GPL(remote_numa_rx_mem_pg_sat_ack);
EXPORT_SYMBOL_GPL(remote_numa_rx_mem_pg_sync_ack);
EXPORT_SYMBOL_GPL(remote_numa_rx_mem_alloc);
EXPORT_SYMBOL_GPL(remote_numa_transport_ctx_destroy);
EXPORT_SYMBOL_GPL(remote_numa_make_trprt_ctx);
EXPORT_SYMBOL_GPL(remote_numa_donor_rx);
EXPORT_SYMBOL_GPL(remote_numa_main_rx);
MODULE_LICENSE("GPL");
