// SPDX-License-Identifier: GPL-2.0-or-later
/*
* transport.c - Remote Numa transport generic functions.
*
* Copyright (C) 2025 Trevor Kemp
*/

#include <linux/mm.h>
#include <linux/rculist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>

#include "transport.h"

#define REMOTE_NUMA_SUPPORTED_PROTO  remote_numa_protocol_0_1
#define REMOTE_NUMA_MAX_SEGMENTS 512
#define REMOTE_NUMA_XFER_HASH_BITS 12
#define REMOTE_NUMA_REXMIT_CHECK_MS (HZ / 40)

#define REMOTE_NUMA_MAX_RETRY_INTERVAL_MS (HZ / 20)  // Retry every 50ms
#define REMOTE_NUMA_MAX_RETRY_COUNT 8

DEFINE_HASHTABLE(xfer_table, REMOTE_NUMA_XFER_HASH_BITS);

typedef struct main_xfer_state {
	struct page *target;
	wait_queue_head_t waitq;
	ktime_t last_update;
	ktime_t retry_deadline;
	u16 retry_count;
	u16 acked_max_seq_num;
	bool is_sync;
	remote_numa_cached_page_t *cached_pg;
	bool is_main_node;
	struct hlist_node node;
	union {
		struct remote_numa_main_trprt_if *main_trprt;
		struct remote_numa_donor_trprt_if *donor_trprt;
	};
	union {
		// Used when receiving a page (e.g., alloc/refetch)
		unsigned long received_bitmap[BITS_TO_LONGS(PAGE_SIZE)];

		// Used when sending a page (e.g., sync/satisfaction)
		unsigned long sent_bitmap[BITS_TO_LONGS(PAGE_SIZE)];
	};
	void *return_info;
} main_xfer_state_t;

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
printk("xfer  %d  xfer->main_trprt->trprt_ctx %px \n", xfer->is_main_node,xfer->main_trprt->trprt_ctx);
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
printk("xfer done check  %d\n", xfer_compute_max_contig(xfer));
	int ret = wait_event_timeout(
		xfer->waitq,
		xfer->acked_max_seq_num >= PAGE_SIZE,
		timeout_jiffies) > 0 ? 0 : -ETIMEDOUT;
	return ret;
}

static inline u32 xfer_hash(u64 cookie)
{
	return hash_64(cookie, REMOTE_NUMA_XFER_HASH_BITS);
}

static void dump_xfer_table(void)
{
    main_xfer_state_t *xfer;
    int bkt;

    pr_info("=== Dumping xfer_table ===\n");
    hash_for_each(xfer_table, bkt, xfer, node) {
        pr_info("  bucket %d: cached:%llu  main_pg_cookie=%llu, donor_pg_cookie=%llx\n",
                bkt,
                (uintptr_t)xfer->cached_pg,
		xfer->cached_pg->main_pg_cookie,
                xfer->cached_pg->donor_pg_cookie);
    }
}

static main_xfer_state_t *xfer_lookup(u64 cookie)
{
	main_xfer_state_t *xfer;

	hash_for_each_possible(xfer_table, xfer, node, xfer_hash(cookie)) {
		if (((uintptr_t)cookie) == (uintptr_t)xfer->cached_pg)
			return xfer;
	}
printk("found nothing:  \n");
dump_xfer_table();
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

	msg->hdr.version = REMOTE_NUMA_SUPPORTED_PROTO;
	msg->hdr.type = xfer->is_sync ? remote_numa_mem_sync : remote_numa_mem_sat_ack;
	msg->hdr.main_cookie = xfer->cached_pg->donor_id;
	msg->hdr.donor_cookie = xfer->cached_pg->donor_cookie;
	msg->flags = (offset + payload_len >= PAGE_SIZE) ? remote_numa_xfer_end_of_pg : 0;
	msg->seq_num = offset;
	msg->payload_len = payload_len;
	msg->sender_pg_cookie = xfer->cached_pg->main_pg_cookie;
	msg->receiver_pg_cookie = xfer->cached_pg->donor_pg_cookie;
	printk("last spot before send  main_pg_cookie %llu", msg->sender_pg_cookie);

	void *payload = ((u8 *)msg) + sizeof(*msg);
	memcpy(payload, data + offset, payload_len);

	set_bit(offset / seg_len, xfer->sent_bitmap);

	return tx_msg(xfer, tx_buf);
}

static void remote_numa_retry_xfers(struct work_struct *work)
{
	if (hash_empty(xfer_table))
	{
		schedule_delayed_work(&remote_numa_retry_work, HZ / 5);
    		return;
	}

	u64 now_ns = ktime_get_ns();
	main_xfer_state_t *xfer;
	int bkt;

	hash_for_each(xfer_table, bkt, xfer, node) {
		if (xfer->acked_max_seq_num >= PAGE_SIZE)
			continue;

		if (ktime_to_ns(xfer->retry_deadline) > now_ns)
			continue;

		if (xfer->retry_count >= REMOTE_NUMA_MAX_RETRY_COUNT) {
			printk(KERN_WARNING "remote_numa: transfer timeout\n");
			wake_up(&xfer->waitq);
			continue;
		}
		u16 max_payload = xfer_state_max_payload(xfer);
		u16 seg_len = max_payload - sizeof(remote_numa_mem_pg_xfer_t);

		for (u32 offset = 0; offset < PAGE_SIZE; offset += seg_len) {
			if (!test_bit(offset / seg_len, xfer->sent_bitmap))
				continue;
			if (offset < xfer->acked_max_seq_num)
				continue;
			remote_numa_send_segment(xfer, offset, seg_len);
		}
		xfer->retry_count++;
		xfer->retry_deadline = ktime_add_ns(ktime_get(),
			REMOTE_NUMA_REXMIT_CHECK_MS * 1000 * 1000);
	}

	schedule_delayed_work(&remote_numa_retry_work, HZ / 5);
}

static void remote_numa_start_retry_worker(void)
{
	INIT_DELAYED_WORK(&remote_numa_retry_work, remote_numa_retry_xfers);
	schedule_delayed_work(&remote_numa_retry_work, HZ / 5);
}

static void remote_numa_stop_retry_worker(void)
{
	cancel_delayed_work_sync(&remote_numa_retry_work);
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

remote_numa_send_ret_t remote_numa_transport_alloc_page_rcu(
	struct remote_numa_main_trprt_if *trprt,
	remote_numa_node_t *donor,
	struct remote_numa_cached_page *cached_target)
{
	u64 main_pg_cookie = (uintptr_t)cached_target;
	struct page *target = cached_target->page;
	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer) {
		printk(KERN_DEBUG "Could not alloc xfer state.\n");
		return remote_numa_send_bad_alloc;
	}

	xfer->cached_pg = cached_target;
	xfer->cached_pg->donor_pg_cookie = 0;
	xfer->cached_pg->main_pg_cookie = (uintptr_t)cached_target;
	xfer->cached_pg->donor_id = donor->node_id;
	xfer->target = target;
	xfer->is_sync = false;
	xfer->acked_max_seq_num = 0;
	xfer->last_update = ktime_get();
	xfer->cached_pg->main_pg_cookie = main_pg_cookie;
	xfer->main_trprt = trprt;
	xfer->is_main_node = true;
	xfer->return_info = donor->priv_return_info;
	init_waitqueue_head(&xfer->waitq);
	bitmap_zero(xfer->sent_bitmap, PAGE_SIZE);
	xfer->retry_deadline = ktime_add_ns(ktime_get(), REMOTE_NUMA_MAX_RETRY_INTERVAL_MS * 1000 * 1000);
	xfer->retry_count = 0;

	hash_add(xfer_table, &xfer->node, xfer_hash(main_pg_cookie));
	// Build and send remote_numa_mem_alloc_t
	void *tx_buf;
	remote_numa_mem_alloc_t *req;
	void **v = (void **)&req;

	trprt->alloc_tx_buffer(sizeof(*req), &tx_buf, v);
	if (!tx_buf || !req) {
		hash_del(&xfer->node);
		kfree(xfer);
		return remote_numa_send_bad_alloc;
	}

	req->hdr.version = remote_numa_protocol_0_1;
	req->hdr.type = remote_numa_mem_alloc;
	req->hdr.main_cookie = donor->node_id;
	req->hdr.donor_cookie = donor->donor_cookie;
	req->main_pg_cookie = main_pg_cookie;

	if (trprt->tx_msg(trprt->trprt_ctx, donor->priv_return_info, tx_buf)) {
		hash_del(&xfer->node);
		kfree(xfer);
		return remote_numa_send_bad_xmit;
	}

	if (remote_numa_xfer_wait_complete(xfer, 5 * HZ)) {
		hash_del(&xfer->node);
		kfree(xfer);
		printk(KERN_DEBUG "Timeout waiting for remote page allocation ack.\n");
		return remote_numa_send_timeout;
	}

	// On success, donor_pg_cookie should now be set in xfer->cached_pg
	cached_target->donor_pg_cookie = xfer->cached_pg->donor_pg_cookie;

	hash_del(&xfer->node);
	kfree(xfer);
	return remote_numa_send_success;
}

remote_numa_send_ret_t remote_numa_transport_refetch_page(
	struct remote_numa_main_trprt_if *trprt,
	u32 donor_node_id,
	u64 donor_pg_cookie,
	struct remote_numa_cached_page *cached_target)
{
	remote_numa_node_t *donor = NULL;

	rcu_read_lock();
	hash_for_each_possible_rcu(trprt->trprt_ctx->node_table, donor, hnode, donor_node_id) {
		if (donor->node_id == donor_node_id) break;
		donor = NULL;
	}
	rcu_read_unlock();
	if (!donor) return remote_numa_send_no_if;

	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer)
		return remote_numa_send_bad_alloc;

	xfer->cached_pg->donor_pg_cookie = donor_pg_cookie;
	xfer->cached_pg->donor_id = donor_node_id;
	xfer->target = cached_target->page;
	xfer->is_sync = false;
	xfer->acked_max_seq_num = 0;
	xfer->last_update = ktime_get();
	xfer->cached_pg->main_pg_cookie = (uintptr_t)cached_target;
	xfer->main_trprt = trprt;
	xfer->is_main_node = true;
	xfer->return_info = donor->priv_return_info;
	init_waitqueue_head(&xfer->waitq);
	bitmap_zero(xfer->sent_bitmap, PAGE_SIZE);
	xfer->retry_deadline = ktime_add_ns(ktime_get(), 500 * 1000 * 1000);
	xfer->retry_count = 0;

	hash_add(xfer_table, &xfer->node, (uintptr_t)cached_target);

	// Build and send refetch request
	void *tx_buf;
	remote_numa_mem_refetch_t *refetch;
	void **v = (void **)&refetch;
	trprt->alloc_tx_buffer(sizeof(*refetch), &tx_buf, v);
	if (!tx_buf || !refetch) {
		hash_del(&xfer->node);
		kfree(xfer);
		return remote_numa_send_bad_alloc;
	}

	refetch->hdr.version       = remote_numa_protocol_0_1;
	refetch->hdr.type          = remote_numa_mem_refetch;
	refetch->hdr.main_cookie   = donor->node_id;
	refetch->hdr.donor_cookie  = donor->donor_cookie;
	refetch->donor_pg_cookie   = donor_pg_cookie;

	if (trprt->tx_msg(trprt->trprt_ctx, donor->priv_return_info, tx_buf)) {
		hash_del(&xfer->node);
		kfree(xfer);
		return remote_numa_send_bad_xmit;
	}

	if (remote_numa_xfer_wait_complete(xfer, 5 * HZ)) {
		hash_del(&xfer->node);
		kfree(xfer);
		return remote_numa_send_timeout;
	}

	hash_del(&xfer->node);
	kfree(xfer);
	return remote_numa_send_success;
}

remote_numa_receive_ret_t remote_numa_rx_mem_pg_sync_xfer(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_pg_xfer_t *xfer)
{
printk("rx pg xfer\n");
	remote_numa_mem_mgr_t *mgr = donor_if->trprt_ctx->mem;
	if (!mgr)
		return remote_numa_receive_err_unknown;

printk("good mgr\n");
	if (xfer->payload_len + xfer->seq_num > PAGE_SIZE)
		return remote_numa_receive_out_of_bounds;

printk("good pg sz\n");
	void *dst;
	if (remote_numa_mem_lookup_page(mgr, xfer->receiver_pg_cookie, &dst) != 0)
		return remote_numa_receive_bad_cookie;

printk("continue...\n");
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
	ack->max_seq_num        = PAGE_SIZE;
	ack->sender_pg_cookie   = xfer->receiver_pg_cookie;
	ack->receiver_pg_cookie = xfer->sender_pg_cookie;

printk("main cookie will be %llu  other is %llu \n", ack->receiver_pg_cookie, ack->sender_pg_cookie);

	remote_numa_node_t *main_node = __remote_numa_get_node_locking(
		donor_if->trprt_ctx->node_table,
		REMOTE_NUMA_HASH_TABLE_ORDER,
		xfer->hdr.donor_cookie);

	int ret = donor_if->tx_msg(donor_if->trprt_ctx,
	                        main_node->priv_return_info,
	                        ack_buf) == 0
	       ? remote_numa_receive_success
	       : remote_numa_receive_err_unknown;
printk("result of donor ack tx %d\n", ret);

	return ret;
}

remote_numa_receive_ret_t remote_numa_rx_mem_pg_sat_ack(
	struct remote_numa_main_trprt_if *main_if,
	remote_numa_mem_satisfaction_t *ack)
{
	main_xfer_state_t *xfer = xfer_lookup(ack->main_pg_cookie);
	if (!xfer)
	{
		return remote_numa_receive_bad_cookie;
	}

	// small hack here to signify "done."
	xfer->acked_max_seq_num = PAGE_SIZE;

	xfer->cached_pg->donor_pg_cookie = ack->donor_pg_cookie;
	wake_up(&xfer->waitq);
	return remote_numa_receive_success;
}

remote_numa_receive_ret_t remote_numa_rx_mem_pg_sync_ack(
	struct remote_numa_main_trprt_if *main_if,
	remote_numa_mem_pg_xfer_ack_t *ack)
{
printk("remote_numa_rx_mem_pg_sync_ack   %llu\n", ack->receiver_pg_cookie);
	main_xfer_state_t *xfer = xfer_lookup(ack->receiver_pg_cookie);
if(!xfer) printk("bad lookup, man.\n");
	if (!xfer)
		return remote_numa_receive_bad_cookie;

	// Update only if we made progress
	if (ack->max_seq_num > xfer->acked_max_seq_num)
		xfer->acked_max_seq_num = ack->max_seq_num;

	// For future robustness: could also set bitmap of acked segments (if desired)


printk("awaken?  %d    %d  %d\n", xfer->acked_max_seq_num >= PAGE_SIZE, xfer->acked_max_seq_num, PAGE_SIZE);
	if (xfer->acked_max_seq_num >= PAGE_SIZE)
		wake_up(&xfer->waitq);

	return remote_numa_receive_success;
}

remote_numa_send_ret_t remote_numa_tx_mem_pg_sync_xfer(
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

	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer)
		return remote_numa_send_bad_alloc;

	xfer->cached_pg = victim;
printk("sync xfer main_pg_cookie %llu\n   %llu", (uintptr_t)victim, victim->main_pg_cookie);
	xfer->target = pg;
	xfer->is_sync = true;
	xfer->acked_max_seq_num = 0;
	xfer->last_update = ktime_get();
	xfer->main_trprt = main_if;
	xfer->is_main_node = true;
	xfer->return_info = donor->priv_return_info;
	init_waitqueue_head(&xfer->waitq);
	bitmap_zero(xfer->sent_bitmap, PAGE_SIZE);
	xfer->retry_deadline = ktime_add_ns(ktime_get(), 500 * 1000 * 1000);
	xfer->retry_count = 0;

	hash_add(xfer_table, &xfer->node, xfer_hash((uintptr_t)victim));

	u16 seg_len = main_if->get_max_payload_len() - sizeof(remote_numa_mem_pg_xfer_t);
	remote_numa_send_all_segments(xfer, seg_len);

	if (remote_numa_xfer_wait_complete(xfer, 5 * HZ)) {
		hash_del(&xfer->node);
		kfree(xfer);
printk("send timeout!!!\n");
		return remote_numa_send_timeout;
	}

	hash_del(&xfer->node);
	kfree(xfer);
	return remote_numa_send_success;
}

remote_numa_trprt_ctx_t *remote_numa_make_trprt_ctx(remote_numa_mem_mgr_t *mem)
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
	default:
		ret = REMOTE_NUMA_TRPRT_RET(remote_numa_receive_mangled_pkt, 1);
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
	default:
		ret = REMOTE_NUMA_TRPRT_RET(remote_numa_receive_mangled_pkt, 1);
		goto done;
	}
done:
	donor_if->free_rx_buff(rx_data);	
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
    remote_numa_mem_mgr_t *mgr = donor_if->trprt_ctx->mem;

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
printk("received pg free!   %px   %px\n", donor_if,  pg_free);
	if (remote_numa_mem_free_page(donor_if->trprt_ctx->mem,
	                              pg_free->donor_pg_cookie) != 0)
		return remote_numa_receive_bad_cookie;

	// Send ACK
	void *tx_buf;
	remote_numa_mem_free_ack_t *ack;
	void **v = (void **)&ack;

printk("alloc buffer\n");
	donor_if->alloc_tx_buffer(sizeof(*ack), &tx_buf, v);
	if (!tx_buf || !ack)
		return remote_numa_receive_bad_alloc;

printk("alloc good\n");
	ack->hdr.version        = remote_numa_protocol_0_1;
	ack->hdr.type           = remote_numa_mem_free_ack;
	ack->hdr.main_cookie    = pg_free->hdr.main_cookie;
	ack->hdr.donor_cookie   = pg_free->hdr.donor_cookie;
	ack->main_pg_cookie     = pg_free->main_pg_cookie;

printk("node lookup for   pg_free->hdr.donor_cookie  %px\n", pg_free->hdr.donor_cookie);
	remote_numa_node_t *main_node = __remote_numa_get_node_locking(
		donor_if->trprt_ctx->node_table,
		REMOTE_NUMA_HASH_TABLE_ORDER,
		pg_free->hdr.donor_cookie);

printk("lookup res:  %px\n", main_node);
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
	if (!xfer)
		return remote_numa_receive_bad_cookie;

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

	// Prepare xfer tracking for ACK
	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer)
		return remote_numa_send_bad_alloc;

	xfer->target = NULL;  // no local page, just waiting for ack
	xfer->is_sync = false;
	xfer->main_trprt = main_if;
	xfer->is_main_node = true;
	init_waitqueue_head(&xfer->waitq);
	xfer->last_update = ktime_get();
	xfer->return_info = donor->priv_return_info;
	bitmap_zero(xfer->received_bitmap, PAGE_SIZE);

	hash_add(xfer_table, &xfer->node, xfer_hash(main_pg_cookie));

	// Send FREE message
	void *tx_buf;
	remote_numa_mem_free_t *msg;
	void **v = (void **)&msg;

	main_if->alloc_tx_buffer(sizeof(*msg), &tx_buf, v);
	if (!tx_buf || !msg) {
		hash_del(&xfer->node);
		kfree(xfer);
		return remote_numa_send_bad_alloc;
	}

	msg->hdr.version        = remote_numa_protocol_0_1;
	msg->hdr.type           = remote_numa_mem_free;
	msg->hdr.main_cookie    = donor_id;
	msg->hdr.donor_cookie   = donor->donor_cookie;
	msg->donor_pg_cookie    = donor_pg_cookie;
	msg->main_pg_cookie     = main_pg_cookie;

	if (main_if->tx_msg(main_if->trprt_ctx, donor->priv_return_info, tx_buf)) {
		hash_del(&xfer->node);
		kfree(xfer);
		return remote_numa_send_bad_xmit;
	}
	// Wait for ACK
	if (remote_numa_xfer_wait_complete(xfer, 5 * HZ)) {
		hash_del(&xfer->node);
		kfree(xfer);
		return remote_numa_send_timeout;
	}

	// Done
	hash_del(&xfer->node);
	kfree(xfer);
	return remote_numa_send_success;
}

EXPORT_SYMBOL_GPL(remote_numa_transport_alloc_page_rcu);
EXPORT_SYMBOL_GPL(remote_numa_transport_refetch_page);
EXPORT_SYMBOL_GPL(remote_numa_rx_mem_pg_sat_ack);
EXPORT_SYMBOL_GPL(remote_numa_rx_mem_pg_sync_ack);
EXPORT_SYMBOL_GPL(remote_numa_rx_mem_alloc);
EXPORT_SYMBOL_GPL(remote_numa_transport_ctx_destroy);
EXPORT_SYMBOL_GPL(remote_numa_make_trprt_ctx);
EXPORT_SYMBOL_GPL(remote_numa_donor_rx);
EXPORT_SYMBOL_GPL(remote_numa_main_rx);
MODULE_LICENSE("GPL");
