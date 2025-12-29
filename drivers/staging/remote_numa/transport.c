// SPDX-License-Identifier: GPL-2.0-or-later
/*
* transport.c - Remote Numa transport generic functions.
*
* Copyright (C) 2025 Trevor Kemp
*/

#include <linux/mm.h>
#include <linux/random.h>
#include <linux/rculist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>

#include "memory.h"
#include "transport.h"

#define REMOTE_NUMA_SUPPORTED_PROTO  remote_numa_protocol_0_1
#define REMOTE_NUMA_XFER_HASH_BITS 12
#define REMOTE_NUMA_REXMIT_CHECK_MS 10

#define REMOTE_NUMA_TRANSFER_TIMEOUT_MS 1000
#define REMOTE_NUMA_DEFAUKT_RETRY_INTERVAL_MS 5
#define REMOTE_NUMA_DEFGAULT_RETRY_INTERVAL_NS (REMOTE_NUMA_DEFAUKT_RETRY_INTERVAL_MS * NSEC_PER_MSEC)
#define REMOTE_NUMA_MAX_RETRY_COUNT 20

DEFINE_HASHTABLE(xfer_table, REMOTE_NUMA_XFER_HASH_BITS);

#include <linux/atomic.h>
static spinlock_t hacky_spinlock;
static int hack;


typedef struct main_xfer_state {
	int hack;
	struct page *target;
	wait_queue_head_t waitq;
	ktime_t last_update;
	ktime_t retry_deadline;
	u16 retry_count;
	remote_numa_cached_page_t *cached_pg;
	bool is_main_node;
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

static remote_numa_send_ret_t tx_msg(
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

static remote_numa_receive_ret_t remote_numa_rx_mem_pg_refetch(
	struct remote_numa_donor_trprt_if *donor_if,
	remote_numa_mem_refetch_t *refetch);

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

static int remote_numa_xfer_wait_complete(main_xfer_state_t *xfer, unsigned long timeout_jiffies)
{
	int ret = wait_event_timeout(
		xfer->waitq,
		xfer_compute_max_contig(xfer) >= PAGE_SIZE,
		timeout_jiffies) > 0 ? 0 : -ETIMEDOUT;
	return ret;
}

static inline u32 xfer_hash(u64 cookie)
{
	return hash_64(cookie, REMOTE_NUMA_XFER_HASH_BITS);
}

static void xfer_free(main_xfer_state_t *xfer)
{
	struct remote_numa_cached_page *cached = xfer->cached_pg;
	
	// Free cached_pg if it was allocated for mem_free tracking
	if (xfer->transfer_type == remote_numa_mem_free && cached)
		kfree(cached);
	
	kfree(xfer);
}

static main_xfer_state_t *xfer_lookup(u64 cookie)
{
	spin_lock(&hacky_spinlock);
	main_xfer_state_t *xfer;
	hash_for_each_possible(xfer_table, xfer, node, xfer_hash(cookie)) {
		if (!xfer->cached_pg) {
			continue;
		}
		if (((uintptr_t)cookie) == (uintptr_t)xfer->cached_pg->main_pg_cookie)
		{
			spin_unlock(&hacky_spinlock);
			return xfer;
		}
	}

	spin_unlock(&hacky_spinlock);
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
	spin_lock(&hacky_spinlock);

	msg->hdr.version = REMOTE_NUMA_SUPPORTED_PROTO;
	msg->hdr.type = xfer->transfer_type;
	msg->hdr.main_cookie = xfer->cached_pg->donor_id;
	msg->hdr.donor_cookie = xfer->cached_pg->donor_cookie;
	msg->flags = (offset + payload_len >= PAGE_SIZE) ? remote_numa_xfer_end_of_pg : 0;
	msg->seq_num = offset;
	msg->payload_len = payload_len;
	msg->sender_pg_cookie = xfer->cached_pg->main_pg_cookie;
	msg->receiver_pg_cookie = xfer->cached_pg->donor_pg_cookie;
	msg->hack = xfer->hack;
	spin_unlock(&hacky_spinlock);

	void *payload = ((u8 *)msg) + sizeof(*msg);
	memcpy(payload, data + offset, payload_len);

	set_bit(offset / seg_len, xfer->sent_bitmap);

	int ret = tx_msg(xfer, tx_buf);

	return ret;
}

static void remote_numa_retry_xfers(struct work_struct *work)
{
	if (hash_empty(xfer_table)) {
		schedule_delayed_work(&remote_numa_retry_work,
			msecs_to_jiffies(REMOTE_NUMA_DEFAUKT_RETRY_INTERVAL_MS));
		return;
	}

	u64 now_ns = ktime_get_ns();
	main_xfer_state_t *xfer;
	struct hlist_node *tmp;
	int bkt;
	LIST_HEAD(work_list);
	main_xfer_state_t **cleanup_array = NULL;
	int cleanup_count = 0;
	int cleanup_capacity = 16;

	/* Use an array to store pointers to completed transfers to minimize lock hold time */
	cleanup_array = kmalloc(cleanup_capacity * sizeof(*cleanup_array), GFP_KERNEL);
	if (!cleanup_array) {
		schedule_delayed_work(&remote_numa_retry_work,
			msecs_to_jiffies(REMOTE_NUMA_DEFAUKT_RETRY_INTERVAL_MS));
		return;
	}

	spin_lock(&hacky_spinlock);
	hash_for_each_safe(xfer_table, bkt, tmp, xfer, node) {
		if (xfer->is_main_node)
			continue;

		if (xfer_compute_max_contig(xfer) >= PAGE_SIZE) {
			hash_del(&xfer->node);
			if (cleanup_count < cleanup_capacity)
				cleanup_array[cleanup_count++] = xfer;
			else /* Out of space in cleanup array, need to free now. */
				xfer_free(xfer);
			continue;
		}

		if (ktime_to_ns(xfer->retry_deadline) >= now_ns)
			continue;

		if (xfer->retry_count >= REMOTE_NUMA_MAX_RETRY_COUNT) {
			printk(KERN_WARNING "remote_numa: donor xfer timeout\n");
			hash_del(&xfer->node);
			if (cleanup_count < cleanup_capacity)
				cleanup_array[cleanup_count++] = xfer;
			else
				xfer_free(xfer);
			continue;
		}

		struct retry_work_item *item = kmalloc(sizeof(*item), GFP_ATOMIC);
		if (item) {
			item->xfer = xfer;
			item->seg_len = xfer_state_max_payload(xfer) - sizeof(remote_numa_mem_pg_xfer_t);
			list_add(&item->list, &work_list);
		}

		xfer->retry_count++;
		u64 base_interval = REMOTE_NUMA_DEFGAULT_RETRY_INTERVAL_NS << xfer->retry_count;
		u64 jitter_ns = get_random_u32() % (base_interval / 10);
		xfer->retry_deadline = ktime_add_ns(ktime_get(), base_interval + jitter_ns);
	}
	spin_unlock(&hacky_spinlock);

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
		kfree(item);
	}

	for (int i = 0; i < cleanup_count; i++)
		xfer_free(cleanup_array[i]);
	kfree(cleanup_array);

	schedule_delayed_work(&remote_numa_retry_work,
		msecs_to_jiffies(REMOTE_NUMA_DEFAUKT_RETRY_INTERVAL_MS));
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
	main_xfer_state_t *xfer = xfer_lookup((uintptr_t)cached_target);
	if (!xfer)
		return true; /* No active transfer */
	return xfer_compute_max_contig(xfer) >= PAGE_SIZE;
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

static remote_numa_receive_ret_t remote_numa_rx_mem_pg_refetch(
	struct remote_numa_donor_trprt_if *donor_if,
	remote_numa_mem_refetch_t *refetch)
{
	struct remote_numa_mem_mgr *mgr = donor_if->trprt_ctx->mem;
	if (!mgr)
		return remote_numa_receive_err_unknown;

	void *page_ptr;
	remote_numa_page_t *rn_pg;
	struct remote_numa_cached_page *cached_pg;
	if (remote_numa_mem_lookup_page(mgr, refetch->donor_pg_cookie,
	                                 &page_ptr, &rn_pg) != 0)
	{
		return remote_numa_receive_bad_cookie;
	}
	cached_pg = &rn_pg->cached_pg;

	struct page *pg = virt_to_page(page_ptr);
	if (!pg)
	{
		return remote_numa_receive_err_unknown;
	}

	remote_numa_node_t *main_node = __remote_numa_get_node_locking(
		donor_if->trprt_ctx->node_table,
		REMOTE_NUMA_HASH_TABLE_ORDER,
		refetch->hdr.donor_cookie);
	if (!main_node)
	{
		return remote_numa_receive_err_unknown;
	}

	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer)
		return remote_numa_receive_bad_alloc;

	// TODO, this is super confusing, need to rename some fields.
	// These donor/main pg cookies need to be backwards since
	// they're referred to by donor/main name in the send* function.
	
	cached_pg->main_pg_cookie = rn_pg->donor_pg_cookie;
	cached_pg->donor_pg_cookie = refetch->main_pg_cookie;
	cached_pg->donor_id = refetch->hdr.main_cookie; //gross. broke an abstraction.
	cached_pg->donor_cookie = refetch->hdr.donor_cookie;

	spin_lock(&hacky_spinlock);
	xfer->hack		= ++hack;
	xfer->cached_pg		= cached_pg;
	xfer->target		= pg;
	xfer->transfer_type	= remote_numa_mem_refetch_sat;
	xfer->is_main_node	= false;
	xfer->donor_trprt	= donor_if;
	xfer->return_info	= main_node->priv_return_info;
	xfer->last_update	= ktime_get();
	init_waitqueue_head(&xfer->waitq);
	bitmap_zero(xfer->sent_bitmap, PAGE_SIZE);
	bitmap_zero(xfer->received_bitmap, PAGE_SIZE);
	u64 jitter_ns = get_random_u32() % (REMOTE_NUMA_DEFGAULT_RETRY_INTERVAL_NS / 10);
	xfer->retry_deadline	= ktime_add_ns(ktime_get(),
					   REMOTE_NUMA_DEFGAULT_RETRY_INTERVAL_NS + jitter_ns);
	xfer->retry_count	= 0;
	smp_wmb();
	hash_add(xfer_table, &xfer->node,
		 xfer_hash(refetch->donor_pg_cookie));
	spin_unlock(&hacky_spinlock);

	u16 seg_len = donor_if->get_max_payload_len() -
		      sizeof(remote_numa_mem_pg_xfer_t);
	remote_numa_send_all_segments(xfer, seg_len);

	return remote_numa_receive_success;
}

remote_numa_send_ret_t remote_numa_transport_alloc_page_async(
	struct remote_numa_main_trprt_if *trprt,
	remote_numa_node_t *donor,
	struct remote_numa_cached_page *cached_target)
{
	spin_lock(&hacky_spinlock);
	u64 main_pg_cookie = (uintptr_t)cached_target;
	struct page *target = cached_target->page;
	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_ATOMIC);
	if (!xfer) {
		spin_unlock(&hacky_spinlock);
		printk(KERN_DEBUG "Could not alloc xfer state.\n");
		return remote_numa_send_bad_alloc;
	}

	xfer->hack = ++hack;
	xfer->cached_pg = cached_target;
	xfer->cached_pg->donor_pg_cookie = 12345;
	xfer->cached_pg->main_pg_cookie = main_pg_cookie;
	xfer->cached_pg->donor_id = donor->node_id;
	xfer->target = target;
	xfer->transfer_type = remote_numa_mem_alloc;
	xfer->last_update = ktime_get();
	xfer->main_trprt = trprt;
	xfer->is_main_node = true;
	xfer->return_info = donor->priv_return_info;
	init_waitqueue_head(&xfer->waitq);
	bitmap_zero(xfer->received_bitmap, PAGE_SIZE);
	bitmap_zero(xfer->sent_bitmap, PAGE_SIZE);
	u64 jitter_ns = get_random_u32() % (REMOTE_NUMA_DEFGAULT_RETRY_INTERVAL_NS / 10);
	xfer->retry_deadline = ktime_add_ns(ktime_get(), REMOTE_NUMA_DEFGAULT_RETRY_INTERVAL_NS + jitter_ns);
	xfer->retry_count = 0;

	hash_add(xfer_table, &xfer->node, xfer_hash(main_pg_cookie));

	void *tx_buf;
	remote_numa_mem_alloc_t *req;
	void **v = (void **)&req;

	smp_wmb();
	trprt->alloc_tx_buffer(sizeof(*req), &tx_buf, v);
	if (!tx_buf || !req) {
		printk(KERN_WARNING "tx_mem_pg_free: hash_del xfer (alloc fail), cookie=%llu, hash=%u, xfer=%p\n",
		       (unsigned long long)main_pg_cookie, xfer_hash(main_pg_cookie), xfer);
		hash_del(&xfer->node);
		xfer_free(xfer);
		spin_unlock(&hacky_spinlock);
		return remote_numa_send_bad_alloc;
	}

	req->hdr.version = remote_numa_protocol_0_1;
	req->hdr.type = remote_numa_mem_alloc;
	req->hdr.main_cookie = donor->node_id;
	req->hdr.donor_cookie = donor->donor_cookie;
	req->main_pg_cookie = main_pg_cookie;
	req->hack = hack;

	spin_unlock(&hacky_spinlock);
	if (trprt->tx_msg(trprt->trprt_ctx, donor->priv_return_info, tx_buf)) {
		printk(KERN_WARNING "tx_mem_pg_free: hash_del xfer (xmit fail), cookie=%llu, hash=%u, xfer=%p\n",
		       (unsigned long long)main_pg_cookie, xfer_hash(main_pg_cookie), xfer);
		hash_del(&xfer->node);
		xfer_free(xfer);
		return remote_numa_send_bad_xmit;
	}

	/* Return immediately - caller must check completion later */
	return remote_numa_send_success;
}

remote_numa_send_ret_t remote_numa_transport_alloc_page_rcu(
	struct remote_numa_main_trprt_if *trprt,
	remote_numa_node_t *donor,
	struct remote_numa_cached_page *cached_target)
{
	remote_numa_send_ret_t ret = remote_numa_transport_alloc_page_async(trprt, donor, cached_target);
	if (ret != remote_numa_send_success)
		return ret;

	main_xfer_state_t *xfer = xfer_lookup((uintptr_t)cached_target);
	if (!xfer)
		return remote_numa_send_err_unknown;

	if (remote_numa_xfer_wait_complete(xfer, (msecs_to_jiffies(REMOTE_NUMA_TRANSFER_TIMEOUT_MS)))) {
		 hash_del(&xfer->node);
		 xfer_free(xfer);
		 printk(KERN_DEBUG "Timeout waiting for remote page allocation ack.\n");
		 return remote_numa_send_timeout;
	}

	hash_del(&xfer->node);
	xfer_free(xfer);
	return remote_numa_send_success;
}

/* Blocking version */
remote_numa_send_ret_t remote_numa_transport_refetch_page(
	struct remote_numa_main_trprt_if *trprt,
	u32 donor_node_id,
	u64 donor_pg_cookie,
	struct remote_numa_cached_page *cached_target)
{
	remote_numa_send_ret_t ret = remote_numa_transport_refetch_page_async(
		trprt, donor_node_id, donor_pg_cookie, cached_target);
	if (ret != remote_numa_send_success)
		return ret;

	main_xfer_state_t *xfer = xfer_lookup((uintptr_t)cached_target);
	if (!xfer)
		return remote_numa_send_err_unknown;

	if (remote_numa_xfer_wait_complete(xfer, msecs_to_jiffies(REMOTE_NUMA_TRANSFER_TIMEOUT_MS))) {
		spin_lock(&hacky_spinlock);
		hash_del(&xfer->node);
		spin_unlock(&hacky_spinlock);
		xfer_free(xfer);
		printk(KERN_WARNING "send timeout waiting for remote page refetch.\n");
		return remote_numa_send_timeout;
	}
	spin_lock(&hacky_spinlock);
	hash_del(&xfer->node);
	spin_unlock(&hacky_spinlock);
	xfer_free(xfer);
	return remote_numa_send_success;
}

/* Non-blocking version */
remote_numa_send_ret_t remote_numa_transport_refetch_page_async(
	struct remote_numa_main_trprt_if *trprt,
	u32 donor_node_id,
	u64 donor_pg_cookie,
	struct remote_numa_cached_page *cached_target)
{
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
		return remote_numa_send_no_if;

	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_ATOMIC);
	if (!xfer)
		return remote_numa_send_bad_alloc;

	xfer->cached_pg = cached_target;
	xfer->cached_pg->donor_pg_cookie = donor_pg_cookie;
	xfer->cached_pg->donor_id = donor_node_id;
	xfer->target = cached_target->page;
	xfer->transfer_type = remote_numa_mem_refetch;
	xfer->last_update = ktime_get();
	xfer->cached_pg->main_pg_cookie = (uintptr_t)cached_target;
	xfer->main_trprt = trprt;
	xfer->is_main_node = true;
	xfer->return_info = donor->priv_return_info;
	init_waitqueue_head(&xfer->waitq);
	bitmap_zero(xfer->sent_bitmap, PAGE_SIZE);
	bitmap_zero(xfer->received_bitmap, PAGE_SIZE);
	xfer->retry_deadline = ktime_add_ns(ktime_get(), REMOTE_NUMA_DEFGAULT_RETRY_INTERVAL_NS);
	xfer->retry_count = 0;

	smp_wmb();
	hash_add_rcu(xfer_table, &xfer->node, xfer_hash((uintptr_t)cached_target));

	void *tx_buf;
	remote_numa_mem_refetch_t *refetch;
	void **v = (void **)&refetch;
	trprt->alloc_tx_buffer(sizeof(*refetch), &tx_buf, v);
	if (!tx_buf || !refetch) {
		spin_lock(&hacky_spinlock);
		hash_del(&xfer->node);
		spin_unlock(&hacky_spinlock);
		xfer_free(xfer);
		return remote_numa_send_bad_alloc;
	}

	refetch->hdr.version       = remote_numa_protocol_0_1;
	refetch->hdr.type          = remote_numa_mem_refetch;
	refetch->hdr.main_cookie   = donor->node_id;
	refetch->hdr.donor_cookie  = donor->donor_cookie;
	refetch->donor_pg_cookie   = donor_pg_cookie;
	refetch->main_pg_cookie    = cached_target->main_pg_cookie;

	if (trprt->tx_msg(trprt->trprt_ctx, donor->priv_return_info, tx_buf)) {
		spin_lock(&hacky_spinlock);
		hash_del(&xfer->node);
		spin_unlock(&hacky_spinlock);
		xfer_free(xfer);
		return remote_numa_send_bad_xmit;
	}

	/* Return immediately - caller polls for completion */
	return remote_numa_send_success;
}

remote_numa_receive_ret_t remote_numa_rx_mem_pg_sync_xfer(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_pg_xfer_t *xfer)
{
	struct remote_numa_mem_mgr *mgr = donor_if->trprt_ctx->mem;
	if (!mgr)
		return remote_numa_receive_err_unknown;

	if (xfer->payload_len + xfer->seq_num > PAGE_SIZE)
		return remote_numa_receive_out_of_bounds;

	void *dst;
	remote_numa_page_t *rn_pg;
	if (remote_numa_mem_lookup_page(mgr, xfer->receiver_pg_cookie,
		&dst, &rn_pg) != 0)
	{
		return remote_numa_receive_bad_cookie;
	}

	void *payload = ((u8 *)xfer) + sizeof(*xfer);
	memcpy(dst + xfer->seq_num, payload, xfer->payload_len);

	void *ack_buf;
	remote_numa_mem_pg_xfer_ack_t *ack;
	void **v = (void **)&ack;

	donor_if->alloc_tx_buffer(sizeof(*ack), &ack_buf, v);
	if (!ack_buf || !ack)
		return remote_numa_receive_bad_alloc;

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
		xfer->hdr.donor_cookie);

	int ret = donor_if->tx_msg(donor_if->trprt_ctx,
	                        main_node->priv_return_info,
	                        ack_buf) == 0
	       ? remote_numa_receive_success
	       : remote_numa_receive_err_unknown;

	return ret;
}

remote_numa_receive_ret_t remote_numa_rx_mem_pg_sat_ack(
	struct remote_numa_main_trprt_if *main_if,
	remote_numa_mem_satisfaction_t *ack)
{
	main_xfer_state_t *xfer = xfer_lookup(ack->main_pg_cookie);
	if (!xfer || ack->hack != hack)
	{
		return remote_numa_receive_bad_cookie;
	}

	// small hack here to signify "done."
	spin_lock(&hacky_spinlock);
	bitmap_fill(xfer->received_bitmap, PAGE_SIZE);

	xfer->cached_pg->donor_pg_cookie = ack->donor_pg_cookie;
	smp_wmb();
	wake_up(&xfer->waitq);
	spin_unlock(&hacky_spinlock);
	return remote_numa_receive_success;
}

remote_numa_receive_ret_t remote_numa_rx_mem_pg_sync_ack(
	struct remote_numa_main_trprt_if *main_if,
	remote_numa_mem_pg_xfer_ack_t *ack)
{
	main_xfer_state_t *xfer = xfer_lookup(ack->receiver_pg_cookie);

	if (!xfer)
		return remote_numa_receive_bad_cookie;

	bitmap_set(xfer->received_bitmap, ack->bottom_seq_num,
		ack->top_seq_num - ack->bottom_seq_num);

	if (xfer_compute_max_contig(xfer) >= PAGE_SIZE)
	{
		wake_up(&xfer->waitq);
	}
	return remote_numa_receive_success;
}

remote_numa_send_ret_t remote_numa_tx_mem_pg_sync_xfer_async(
	struct remote_numa_main_trprt_if *main_if,
	u64 donor_pg_cookie,
	struct page *pg,
	struct remote_numa_cached_page *victim)
{
	if (!main_if || !pg || !victim)
		return remote_numa_send_err_unknown;
	remote_numa_node_t *donor = __remote_numa_get_node_locking(
		main_if->trprt_ctx->node_table,
		REMOTE_NUMA_HASH_TABLE_ORDER,
		victim->donor_id);
	if (!donor)
		return remote_numa_send_no_if;

	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_ATOMIC);
	if (!xfer)
		return remote_numa_send_bad_alloc;

	spin_lock(&hacky_spinlock);
	xfer->hack = ++hack;
	xfer->cached_pg = victim;
	xfer->target = pg;
	xfer->transfer_type = remote_numa_mem_sync;
	xfer->last_update = ktime_get();
	xfer->main_trprt = main_if;
	xfer->is_main_node = true;
	xfer->return_info = donor->priv_return_info;
	init_waitqueue_head(&xfer->waitq);
	bitmap_zero(xfer->sent_bitmap, PAGE_SIZE);
	bitmap_zero(xfer->received_bitmap, PAGE_SIZE);
	xfer->retry_deadline = ktime_add_ns(ktime_get(), REMOTE_NUMA_DEFGAULT_RETRY_INTERVAL_NS);
	xfer->retry_count = 0;

	smp_wmb();
	hash_add(xfer_table, &xfer->node, xfer_hash((uintptr_t)victim));

	u16 seg_len = main_if->get_max_payload_len() - sizeof(remote_numa_mem_pg_xfer_t);
	smp_wmb();
	spin_unlock(&hacky_spinlock);

	remote_numa_send_all_segments(xfer, seg_len);

	/* Return immediately - transfer will complete async */
	return remote_numa_send_success;
}

remote_numa_send_ret_t remote_numa_tx_mem_pg_sync_xfer(
	struct remote_numa_main_trprt_if *main_if,
	u64 donor_pg_cookie,
	struct page *pg,
	struct remote_numa_cached_page *victim)
{
	remote_numa_send_ret_t ret = remote_numa_tx_mem_pg_sync_xfer_async(main_if,
			donor_pg_cookie, pg, victim);
	if (ret != remote_numa_send_success)
		return ret;

	main_xfer_state_t *xfer = xfer_lookup((uintptr_t)victim);
	if (!xfer)
		return remote_numa_send_err_unknown;

	if (remote_numa_xfer_wait_complete(xfer, msecs_to_jiffies(REMOTE_NUMA_TRANSFER_TIMEOUT_MS))) {
		 u64 cookie = xfer->cached_pg ? xfer->cached_pg->main_pg_cookie : (uintptr_t)victim;
		 printk(KERN_WARNING "tx_mem_pg_free: hash_del xfer (timeout), cookie=%llu, hash=%u, xfer=%p\n",
			 (unsigned long long)cookie, xfer_hash(cookie), xfer);
		 hash_del(&xfer->node);
		 xfer_free(xfer);
		 return remote_numa_send_timeout;
	}

	spin_lock(&hacky_spinlock);
	hash_del(&xfer->node);
	spin_unlock(&hacky_spinlock);
	xfer_free(xfer);
	return remote_numa_send_success;
}

/* Check if transfer is complete. Returns 0 if done, -EAGAIN if in progress, <0 on error */
int remote_numa_check_transfer_complete(struct remote_numa_cached_page *cached_pg)
{
	main_xfer_state_t *xfer = xfer_lookup((uintptr_t)cached_pg);
	if (!xfer) {
		/* No transfer found - either never started or already completed */
		return 0;
	}

	/* Check if transfer is complete */
	if (xfer_compute_max_contig(xfer) >= PAGE_SIZE) {
		/* Transfer complete - clean up */
		spin_lock(&hacky_spinlock);
		hash_del(&xfer->node);
		spin_unlock(&hacky_spinlock);
		xfer_free(xfer);
		return 0;
	}

	/* Still in progress */
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

remote_numa_receive_ret_t remote_numa_main_rx(
	remote_numa_main_trprt_if_t *main_if, void *rx_data, void *payload)
{
	remote_numa_receive_ret_t ret = 0;

	remote_numa_msg_hdr_t *hdr = payload;
	if (hdr->version != REMOTE_NUMA_SUPPORTED_PROTO)
	{
		ret = REMOTE_NUMA_TRPRT_RET(remote_numa_receive_bad_proto, 1);
		goto done;
	}

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
		ret = REMOTE_NUMA_TRPRT_RET(remote_numa_receive_mangled_pkt, 1);
		printk(KERN_WARNING "error processing pkt... unknown type\n");
		goto done;
	}
done:
	main_if->free_rx_buff(rx_data);	
	return ret;
}

remote_numa_receive_ret_t remote_numa_donor_rx(
	remote_numa_donor_trprt_if_t *donor_if, void *rx_data, void *payload)
{
	remote_numa_receive_ret_t ret = 0;

	remote_numa_msg_hdr_t *hdr = payload;
	if (hdr->version != REMOTE_NUMA_SUPPORTED_PROTO)
	{
		ret = REMOTE_NUMA_TRPRT_RET(remote_numa_receive_bad_proto, 1);
		goto done;
	}

	switch(hdr->type)
	{
	case remote_numa_mem_query:
		ret = remote_numa_rx_mem_query(donor_if, payload);
		goto done;
	case remote_numa_mem_alloc:
		ret = remote_numa_rx_mem_alloc(donor_if, payload);
		goto done;
	case remote_numa_mem_sync:
		ret = remote_numa_rx_mem_pg_sync_xfer(donor_if, payload);
		goto done;
	case remote_numa_mem_free:
		ret = remote_numa_rx_mem_pg_free(donor_if, payload);
		goto done;
	case remote_numa_mem_refetch:
		ret = remote_numa_rx_mem_pg_refetch(donor_if, payload);
		goto done;
	case remote_numa_mem_refetch_ack:
		ret = remote_numa_rx_mem_pg_refetch_ack(donor_if, payload);
		goto done;
	default:
		ret = REMOTE_NUMA_TRPRT_RET(remote_numa_receive_mangled_pkt, 1);
		printk(KERN_WARNING "error processing pkt... unknown type\n");
		goto done;
	}
done:
	donor_if->free_rx_buff(rx_data);

	if (ret)
		printk(KERN_WARNING "error processing pkt\n");

	return ret;
}

remote_numa_receive_ret_t remote_numa_rx_advert(
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
		return REMOTE_NUMA_TRPRT_RET(remote_numa_receive_bad_alloc, 5);
	void *tx_buffer_start;
	remote_numa_mem_query_t *query;
	void **v_query = (void **)&query;
	main_if->alloc_tx_buffer(sizeof(remote_numa_mem_query_t),
		&tx_buffer_start, v_query);
	if (tx_buffer_start == NULL || query == NULL)
	{
		// TODO make file id for each return code to use.
		return REMOTE_NUMA_TRPRT_RET(remote_numa_send_bad_alloc, 1);
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
		0 : REMOTE_NUMA_TRPRT_RET(remote_numa_receive_err_unknown, 1);
}

remote_numa_receive_ret_t remote_numa_rx_mem_query(
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
		return REMOTE_NUMA_TRPRT_RET(remote_numa_receive_bad_alloc, 6);

	void *tx_buffer_start;
	remote_numa_mem_resp_t *resp;
	void **v_resp = (void **)&resp;
	donor_if->alloc_tx_buffer(sizeof(remote_numa_mem_resp_t),
		&tx_buffer_start, v_resp);
	if (tx_buffer_start == NULL || resp == NULL)
	{
		// TODO make file id for each return code to use.
		return REMOTE_NUMA_TRPRT_RET(remote_numa_send_bad_alloc, 2);
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
		0 : REMOTE_NUMA_TRPRT_RET(remote_numa_receive_err_unknown, 2);
}

remote_numa_receive_ret_t remote_numa_rx_mem_resp(
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

	return 0;
}

remote_numa_receive_ret_t remote_numa_rx_mem_alloc(
    remote_numa_donor_trprt_if_t *donor_if,
    remote_numa_mem_alloc_t *alloc)
{
    u64 cookie;
    void *page;
    struct remote_numa_mem_mgr *mgr = donor_if->trprt_ctx->mem;

    if (!mgr) {
        printk(KERN_DEBUG "No mem manager in donor.\n");
        return remote_numa_receive_err_unknown;
    }

    if (remote_numa_mem_alloc_page(mgr, &cookie, &page) != 0) {
        printk(KERN_DEBUG "Bad page alloc manager in donor.\n");
        return remote_numa_receive_bad_alloc;
    }

    void *tx_buf;
    remote_numa_mem_satisfaction_t *ack;
    void **v = (void **)&ack;

    donor_if->alloc_tx_buffer(sizeof(*ack), &tx_buf, v);
    if (!tx_buf || !ack) {
        printk(KERN_DEBUG "Bad tx alloc manager in donor.\n");
        remote_numa_mem_free_page(mgr, cookie); // Return page to pool
        return remote_numa_receive_bad_alloc;
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
		alloc->hdr.donor_cookie);

    // Send the ack
    return donor_if->tx_msg(
               donor_if->trprt_ctx,
               main_node->priv_return_info,
               tx_buf) == 0
           ? remote_numa_receive_success
           : remote_numa_receive_err_unknown;
}

remote_numa_receive_ret_t remote_numa_rx_mem_pg_free(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_free_t *pg_free)
{
	if (remote_numa_mem_free_page(donor_if->trprt_ctx->mem,
	                              pg_free->donor_pg_cookie) != 0)
		return remote_numa_receive_bad_cookie;

	// Send ACK
	void *tx_buf;
	remote_numa_mem_free_ack_t *ack;
	void **v = (void **)&ack;

	donor_if->alloc_tx_buffer(sizeof(*ack), &tx_buf, v);
	if (!tx_buf || !ack)
		return remote_numa_receive_bad_alloc;

	ack->hdr.version        = remote_numa_protocol_0_1;
	ack->hdr.type           = remote_numa_mem_free_ack;
	ack->hdr.main_cookie    = pg_free->hdr.main_cookie;
	ack->hdr.donor_cookie   = pg_free->hdr.donor_cookie;
	ack->main_pg_cookie     = pg_free->main_pg_cookie;

	remote_numa_node_t *main_node = __remote_numa_get_node_locking(
		donor_if->trprt_ctx->node_table,
		REMOTE_NUMA_HASH_TABLE_ORDER,
		pg_free->hdr.donor_cookie);

	return donor_if->tx_msg(donor_if->trprt_ctx,
	                        main_node->priv_return_info,
	                        tx_buf) == 0
	       ? remote_numa_receive_success
	       : remote_numa_receive_err_unknown;
}


remote_numa_receive_ret_t remote_numa_rx_mem_pg_free_ack(
	remote_numa_main_trprt_if_t *main_if,
	remote_numa_mem_free_ack_t *ack)
{
    // Look up xfer state by main node's cookie for this page
    main_xfer_state_t *xfer = xfer_lookup(ack->main_pg_cookie);
    if (!xfer) {
        printk(KERN_WARNING "rx_mem_pg_free_ack: xfer_lookup failed for main_pg_cookie=%llu\n", (unsigned long long)ack->main_pg_cookie);
        return remote_numa_receive_bad_cookie;
    }

	// Mark as complete so the wait condition is satisfied
	bitmap_fill(xfer->received_bitmap, PAGE_SIZE);
	// Wake any waiters (e.g. cleanup thread) now that the page is freed remotely
	wake_up(&xfer->waitq);

    // Optional: mark max acked region or log the completion

    return remote_numa_receive_success;
}

remote_numa_send_ret_t remote_numa_tx_mem_pg_free(
	struct remote_numa_main_trprt_if *main_if,
	u32 donor_id,
	u64 donor_pg_cookie,
	uintptr_t main_pg_cookie)
{
	remote_numa_node_t *donor = NULL;
	rcu_read_lock();
	hash_for_each_possible_rcu(main_if->trprt_ctx->node_table, donor, hnode, donor_id) {
		if (donor->node_id == donor_id)
		{
			break;
		}
		donor = NULL;
	}
	rcu_read_unlock();

	if (!donor)
		return remote_numa_send_no_if;

	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer)
		return remote_numa_send_bad_alloc;

	/* Create a minimal cached_pg for hash table lookup consistency */
	struct remote_numa_cached_page *cached_pg = kzalloc(sizeof(*cached_pg), GFP_KERNEL);
	if (!cached_pg) {
		xfer_free(xfer);
		return remote_numa_send_bad_alloc;
	}
	cached_pg->main_pg_cookie = main_pg_cookie;
	cached_pg->donor_pg_cookie = donor_pg_cookie;
	cached_pg->donor_id = donor_id;

	xfer->cached_pg = cached_pg;
	xfer->target = NULL;  // no local page, just waiting for ack
	xfer->transfer_type = remote_numa_mem_free;
	xfer->main_trprt = main_if;
	xfer->is_main_node = true;
	xfer->return_info = donor->priv_return_info;
	init_waitqueue_head(&xfer->waitq);
	xfer->last_update = ktime_get();
	xfer->return_info = donor->priv_return_info;
	bitmap_zero(xfer->received_bitmap, PAGE_SIZE);

	smp_wmb();
	hash_add(xfer_table, &xfer->node, xfer_hash(main_pg_cookie));

	// Send FREE message
	void *tx_buf;
	remote_numa_mem_free_t *msg;
	void **v = (void **)&msg;

	main_if->alloc_tx_buffer(sizeof(*msg), &tx_buf, v);
	if (!tx_buf || !msg) {
		hash_del(&xfer->node);
		xfer_free(xfer);
		return remote_numa_send_bad_alloc;
	}

	msg->hdr.version        = remote_numa_protocol_0_1;
	msg->hdr.type           = remote_numa_mem_free;
	msg->hdr.main_cookie    = donor_id;
	msg->hdr.donor_cookie   = donor->donor_cookie;
	msg->donor_pg_cookie    = donor_pg_cookie;
	msg->main_pg_cookie     = main_pg_cookie;

	if (main_if->tx_msg(main_if->trprt_ctx, donor->priv_return_info, tx_buf)) {
		printk(KERN_WARNING "tx_mem_pg_free: hash_del xfer (xmit fail), cookie=%llu, hash=%u, xfer=%p\n",
		       (unsigned long long)main_pg_cookie, xfer_hash(main_pg_cookie), xfer);
		hash_del(&xfer->node);
		xfer_free(xfer);
		return remote_numa_send_bad_xmit;
	}
	// Wait for ACK
	if (remote_numa_xfer_wait_complete(xfer, msecs_to_jiffies(REMOTE_NUMA_TRANSFER_TIMEOUT_MS))) {
		printk(KERN_WARNING "tx_mem_pg_free: hash_del xfer (timeout), cookie=%llu, hash=%u, xfer=%p\n",
		       (unsigned long long)main_pg_cookie, xfer_hash(main_pg_cookie), xfer);
		hash_del(&xfer->node);
		xfer_free(xfer);
		return remote_numa_send_timeout;
	}

	// Done
	spin_lock(&hacky_spinlock);
	hash_del(&xfer->node);
	spin_unlock(&hacky_spinlock);
	xfer_free(xfer);
	return remote_numa_send_success;
}

remote_numa_receive_ret_t remote_numa_rx_mem_pg_refetch_sat(
    remote_numa_main_trprt_if_t *main_if,
    remote_numa_mem_pg_xfer_t *sat)
{
    main_xfer_state_t *xfer = xfer_lookup(sat->receiver_pg_cookie);
    if (!xfer)
    {
	return remote_numa_receive_bad_cookie;
    }
    if (sat->payload_len + sat->seq_num > PAGE_SIZE)
        return remote_numa_receive_out_of_bounds;

    /* write segment into local page */
    void *dst = page_address(xfer->target);
    memcpy(dst + sat->seq_num, ((u8 *)sat) + sizeof(*sat), sat->payload_len);

    /* mark locally received so waiters can complete */
    bitmap_set(xfer->received_bitmap, sat->seq_num, sat->payload_len);

    /* build ACK with bottom/top = [seq, seq+len), like sync */
    void *ack_buf;
    remote_numa_mem_pg_xfer_ack_t *ack;
    void **v = (void **)&ack;
    main_if->alloc_tx_buffer(sizeof(*ack), &ack_buf, v);
    if (!ack_buf || !ack) return remote_numa_receive_bad_alloc;

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
        return remote_numa_receive_bad_cookie;
    }

    /* send ACK back to donor */
    int ret = main_if->tx_msg(main_if->trprt_ctx, donor->priv_return_info, ack_buf);
    if (ret) {
        printk(KERN_ERR "refetch_sat: failed to send ACK, ret=%u\n", ret);
    }

    /* done? wake any waiters */
    if (xfer_compute_max_contig(xfer) >= PAGE_SIZE)
        wake_up(&xfer->waitq);

    return remote_numa_receive_success;
}

remote_numa_receive_ret_t
remote_numa_rx_mem_pg_refetch_ack(struct remote_numa_donor_trprt_if *donor_if,
                                  remote_numa_mem_pg_xfer_ack_t *ack)
{
    main_xfer_state_t *xfer = xfer_lookup(ack->receiver_pg_cookie);
    if (!xfer)
        return remote_numa_receive_bad_cookie;

    /* Stale in-flight? Keep behavior consistent with sync_ack: log and continue. */
    if (ack->hack != xfer->hack) {
        printk(KERN_INFO "Ignoring refetch_ack due to hack staleness (expected %d, got %d)\n",
               xfer->hack, ack->hack);
        return remote_numa_receive_success;  // Silently ignore stale acks
    }

    /* Range-based ACK: handle out-of-order arrivals without over-marking. */
    bitmap_set(xfer->received_bitmap,
               ack->bottom_seq_num,
               ack->top_seq_num - ack->bottom_seq_num);

    /* If the page is fully covered, wake the waiter. */
    if (xfer_compute_max_contig(xfer) >= PAGE_SIZE)
        wake_up(&xfer->waitq);

    return remote_numa_receive_success;
}

void tmp_init(void)
{
	spin_lock_init(&hacky_spinlock);
	hack = 0;
}
EXPORT_SYMBOL_GPL(tmp_init);


EXPORT_SYMBOL_GPL(remote_numa_transport_alloc_page_rcu);
EXPORT_SYMBOL_GPL(remote_numa_transport_alloc_page_async);
EXPORT_SYMBOL_GPL(remote_numa_transport_refetch_page);
EXPORT_SYMBOL_GPL(remote_numa_transport_refetch_page_async);
EXPORT_SYMBOL_GPL(remote_numa_tx_mem_pg_sync_xfer_async);
EXPORT_SYMBOL_GPL(remote_numa_transport_is_transfer_complete);
EXPORT_SYMBOL_GPL(remote_numa_rx_mem_pg_sat_ack);
EXPORT_SYMBOL_GPL(remote_numa_rx_mem_pg_sync_ack);
EXPORT_SYMBOL_GPL(remote_numa_rx_mem_alloc);
EXPORT_SYMBOL_GPL(remote_numa_transport_ctx_destroy);
EXPORT_SYMBOL_GPL(remote_numa_make_trprt_ctx);
EXPORT_SYMBOL_GPL(remote_numa_donor_rx);
EXPORT_SYMBOL_GPL(remote_numa_main_rx);
MODULE_LICENSE("GPL");
