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

DEFINE_HASHTABLE(xfer_table, REMOTE_NUMA_XFER_HASH_BITS);

typedef struct main_xfer_state {
	struct page *target;
	wait_queue_head_t waitq;
	ktime_t last_update;
	unsigned long received_bitmap[BITS_TO_LONGS(PAGE_SIZE)];
	u16 acked_max_seq_num;
	bool is_sync;
	remote_numa_cached_page_t cached_pg;
	struct hlist_node node;
} main_xfer_state_t;

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
	return find_first_zero_bit(x->received_bitmap, PAGE_SIZE);
}

static int remote_numa_xfer_wait_complete(main_xfer_state_t *xfer, unsigned long timeout_jiffies)
{
	return wait_event_timeout(
		xfer->waitq,
		xfer_compute_max_contig(xfer) >= PAGE_SIZE,
		timeout_jiffies) > 0 ? 0 : -ETIMEDOUT;
}

static inline u32 xfer_hash(u64 cookie)
{
	return hash_64(cookie, REMOTE_NUMA_XFER_HASH_BITS);
}

static main_xfer_state_t *xfer_lookup(u64 cookie)
{
	main_xfer_state_t *xfer;

	hash_for_each_possible(xfer_table, xfer, node, xfer_hash(cookie)) {
		if (((uintptr_t)cookie) == (uintptr_t)&xfer->cached_pg)
			return xfer;
	}

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


/* Pure lookup: caller must hold rcu_read_lock() */
static remote_numa_node_t *__remote_numa_get_node(struct hlist_head *table,
		       unsigned int table_bits,
		       u32 node_id)
{
	remote_numa_node_t *node;
	struct hlist_head *head = &table[hash_min(node_id, table_bits)];

	hlist_for_each_entry_rcu(node, head, hnode, head) {
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

remote_numa_receive_ret_t remote_numa_rx_mem_pg_alloc_xfer(
	struct remote_numa_main_trprt_if *main_if,
	remote_numa_mem_pg_xfer_t *xfer)
{
	main_xfer_state_t *state = xfer_lookup(xfer->receiver_pg_cookie);
	if (!state || !state->target)
		return remote_numa_receive_bad_cookie;

	u32 offset = xfer->seq_num;
	u32 len = xfer->payload_len;

	if (offset + len > PAGE_SIZE)
		return remote_numa_receive_out_of_bounds;

	// Copy payload into page
	void *dst = page_address(state->target) + offset;
	void *src = ((u8 *)xfer) + sizeof(*xfer);
	memcpy(dst, src, len);

	// Mark bytes as received
	bitmap_set(state->received_bitmap, offset, len);

	// Optionally update last activity timestamp
	state->last_update = ktime_get();

	// Wake up waiter if the page is complete
	if (xfer_compute_max_contig(state) >= PAGE_SIZE)
		wake_up(&state->waitq);

	// Prepare and send ACK
	void *ack_buf;
	remote_numa_mem_pg_xfer_ack_t *ack;
	void **v = (void **)&ack;
	main_if->alloc_tx_buffer(sizeof(*ack), &ack_buf, v);
	if (!ack_buf || !ack)
		return remote_numa_receive_bad_alloc;

	ack->hdr.version           = remote_numa_protocol_0_1;
	ack->hdr.type              = remote_numa_mem_sat_ack;
	ack->hdr.main_cookie       = state->cached_pg.donor_id;
	ack->hdr.donor_cookie      = xfer->hdr.donor_cookie;
	ack->max_seq_num           = xfer_compute_max_contig(state);
	ack->sender_pg_cookie      = (uintptr_t)&state->cached_pg;
	ack->receiver_pg_cookie    = xfer->sender_pg_cookie;

	remote_numa_node_t *donor = __remote_numa_get_node_locking(
		main_if->trprt_ctx->node_table,
		REMOTE_NUMA_HASH_TABLE_ORDER,
		xfer->hdr.donor_cookie);

	if (!main_if->tx_msg(main_if->trprt_ctx,
		donor->priv_return_info, ack_buf))
		return remote_numa_receive_err_unknown;

	return remote_numa_receive_success;
}

remote_numa_send_ret_t remote_numa_transport_alloc_page_rcu(
	struct remote_numa_main_trprt_if *trprt,
	remote_numa_node_t *donor,
	struct remote_numa_cached_page *cached_target)
{
	u64 main_pg_cookie = (uintptr_t)cached_target;
	struct page *target = cached_target->page;
	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer)
		return remote_numa_send_bad_alloc;

	xfer->cached_pg.donor_pg_cookie = 0;
	xfer->cached_pg.donor_id = donor->node_id;
	xfer->target = target;
	xfer->is_sync = false;
	xfer->acked_max_seq_num = 0;
	xfer->last_update = ktime_get();
	xfer->cached_pg.main_pg_cookie = main_pg_cookie;
	init_waitqueue_head(&xfer->waitq);
	bitmap_zero(xfer->received_bitmap, PAGE_SIZE);
	hash_add(xfer_table, &xfer->node, main_pg_cookie);

	// Build and send request
	void *tx_buf;
	remote_numa_mem_alloc_t *alloc;
	void **v = (void **)&alloc;
	trprt->alloc_tx_buffer(sizeof(*alloc), &tx_buf, v);
	if (!tx_buf || !alloc)
		goto fail;

	alloc->main_pg_cookie = main_pg_cookie;
	alloc->hdr.version = remote_numa_protocol_0_1;
	alloc->hdr.type = remote_numa_mem_alloc;
	alloc->hdr.main_cookie = cached_target->donor_id;
	alloc->hdr.donor_cookie = donor->donor_cookie;

	if (trprt->tx_msg(trprt->trprt_ctx, donor->priv_return_info, tx_buf))
		goto fail;

	// Wait for full page arrival
	if (remote_numa_xfer_wait_complete(xfer, 5 * HZ)) {
		hash_del(&xfer->node);
		kfree(xfer);
		return remote_numa_send_timeout;
	}

	hash_del(&xfer->node);
	kfree(xfer);
	return remote_numa_send_success;

fail:
	if (xfer)
		hash_del(&xfer->node);
	kfree(xfer);
	return remote_numa_send_bad_xmit;
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
		if (donor->node_id == donor_node_id) {
			break;
		}
		donor = NULL;
	}
	rcu_read_unlock();

	if (!donor)
		return remote_numa_send_no_if;

	// Register transfer
	main_xfer_state_t *xfer = kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer)
		return remote_numa_send_bad_alloc;

	xfer->cached_pg.donor_pg_cookie = donor_pg_cookie;
	xfer->cached_pg.donor_id = donor_node_id;
	xfer->target = cached_target->page;
	xfer->is_sync = false;
	xfer->acked_max_seq_num = 0;
	xfer->last_update = ktime_get();
	xfer->cached_pg.main_pg_cookie = (uintptr_t)cached_target;
	init_waitqueue_head(&xfer->waitq);
	bitmap_zero(xfer->received_bitmap, PAGE_SIZE);
	hash_add(xfer_table, &xfer->node, (uintptr_t)cached_target);

	// Send refetch request
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

	// Wait for full page to arrive
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
	struct remote_numa_donor_trprt_if *donor_if,
	remote_numa_mem_pg_xfer_t *xfer)
{
	return -1;
}

/*
 * When sent by main node, received by donor node in ack
 * of a segment of a page transfer. Is a remote_numa_mem_sat_ack
 */
remote_numa_receive_ret_t remote_numa_rx_mem_pg_sat_ack(
struct remote_numa_donor_trprt_if *donor_if,
remote_numa_mem_pg_xfer_ack_t *ack)
{
	return 1;
}

/*
 * Seny by donor node in ack of a mem sync segment. Is a remote_numa_mem_sync_ack.
 */
remote_numa_receive_ret_t remote_numa_rx_mem_pg_sync_ack(
struct remote_numa_main_trprt_if *main_if,
remote_numa_mem_pg_xfer_ack_t *ack)
{
	return 1;
}

/*
 * Transmitted by main node when needing to sync a page over.
 * Is a remote_numa_mem_sync.
 */
remote_numa_send_ret_t remote_numa_tx_mem_pg_sync_xfer(
	struct remote_numa_main_trprt_if *main_if,
	u64 donor_pg_cookie,
	struct page *pg,
	struct remote_numa_cached_page *victim)
{
	if (!main_if || !pg || !victim)
		return remote_numa_send_err_unknown;

	void *page_data = page_address(pg);

	remote_numa_node_t *donor = __remote_numa_get_node_locking(
		main_if->trprt_ctx->node_table,
		REMOTE_NUMA_HASH_TABLE_ORDER,
		victim->donor_id);

	if (!donor)
		return remote_numa_send_no_if;

	u16 max_payload_len = main_if->get_max_payload_len();
	if (max_payload_len <= sizeof(remote_numa_mem_pg_xfer_t) + 1)
		return remote_numa_send_err_unknown;

	for (u32 offset = 0; offset < PAGE_SIZE;) {
		u32 len = min(max_payload_len - sizeof(remote_numa_mem_pg_xfer_t),
		              PAGE_SIZE - offset);

		void *tx_buf;
		remote_numa_mem_pg_xfer_t *xfer;
		void **v = (void **)&xfer;

		main_if->alloc_tx_buffer(sizeof(*xfer) + len, &tx_buf, v);
		if (!tx_buf || !xfer)
			return remote_numa_send_bad_alloc;

		xfer->hdr.version       = remote_numa_protocol_0_1;
		xfer->hdr.type          = remote_numa_mem_sync;
		xfer->hdr.main_cookie   = 0;
		xfer->hdr.donor_cookie  = donor->node_id;
		xfer->flags             = (offset + len >= PAGE_SIZE)
		                          ? remote_numa_xfer_end_of_pg : 0;
		xfer->seq_num           = offset;
		xfer->payload_len       = len;
		xfer->sender_pg_cookie  = donor_pg_cookie;

		void *payload = ((u8 *)xfer) + sizeof(*xfer);
		memcpy(payload, page_data + offset, len);

		if (main_if->tx_msg(main_if->trprt_ctx, donor->priv_return_info, tx_buf) != 0)
			return remote_numa_send_bad_xmit;

		offset += len;
	}

	return remote_numa_send_success;
}

remote_numa_trprt_ctx_t *remote_numa_make_trprt_ctx(remote_numa_mem_mgr_t *mem)
{
	remote_numa_trprt_ctx_t *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	hash_init(ctx->node_table);

	spin_lock_init(&ctx->hash_write_lock);

	ctx->mem = mem;
	ctx->trprt_ctx = NULL;
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
	case remote_numa_mem_satisfaction:
		ret = remote_numa_rx_mem_pg_alloc_xfer(main_if, payload);
		goto done;
	case remote_numa_mem_sync_ack:
		ret = remote_numa_rx_mem_pg_sync_ack(main_if, payload);
		goto done;
	case remote_numa_mem_free_ack:
		ret = remote_numa_rx_mem_pg_free_ack(main_if, payload);
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
	case remote_numa_mem_sat_ack:
		ret = remote_numa_rx_mem_pg_sat_ack(donor_if, payload);
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
	return 1;
}

remote_numa_receive_ret_t remote_numa_rx_mem_pg_free(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_free_t *pg_free)
{
	return 1;
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
			break;
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
	init_waitqueue_head(&xfer->waitq);
	xfer->last_update = ktime_get();
	bitmap_zero(xfer->received_bitmap, PAGE_SIZE);

	hash_add(xfer_table, &xfer->node, main_pg_cookie);

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
	msg->hdr.donor_cookie   = 0; // TODO we shouldn't need this. Re-evalute these.
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

