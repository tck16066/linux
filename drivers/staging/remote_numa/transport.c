// SPDX-License-Identifier: GPL-2.0-or-later
/*
* transport.c - Remote Numa transport generic functions.
*
* Copyright (C) 2025 Trevor Kemp
*/

#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>

#include "transport.h"

#define REMOTE_NUMA_SUPPORTED_PROTO  remote_numa_protocol_0_1

remote_numa_trprt_ctx_t *remote_numa_make_trprt_ctx(void)
{
	remote_numa_trprt_ctx_t *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	hash_init(ctx->node_table);

	spin_lock_init(&ctx->hash_write_lock);

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
	kfree(ctx);
}

remote_numa_receive_ret_t remote_numa_main_rx(
	remote_numa_main_trprt_if_t *main_if, void *rx_data)
{
	remote_numa_msg_hdr_t *hdr = rx_data;
	if (hdr->version != REMOTE_NUMA_SUPPORTED_PROTO)
	{
		return REMOTE_NUMA_TRPRT_RET(remote_numa_receive_bad_proto, 1);
	}

	switch(hdr->type)
	{
	case remote_numa_eth_advert:
		return remote_numa_rx_advert(main_if, rx_data);
	default:
		return REMOTE_NUMA_TRPRT_RET(remote_numa_receive_mangled_pkt, 1);
	}
}

remote_numa_receive_ret_t remote_numa_donor_rx(
	remote_numa_donor_trprt_if_t *donor_if, void *rx_data)
{
	remote_numa_msg_hdr_t *hdr = rx_data;
	if (hdr->version != REMOTE_NUMA_SUPPORTED_PROTO)
	{
		return REMOTE_NUMA_TRPRT_RET(remote_numa_receive_bad_proto, 1);
	}

	switch(hdr->type)
	{
	case remote_numa_mem_query:
		return remote_numa_rx_mem_query(donor_if, rx_data);
	default:
		return REMOTE_NUMA_TRPRT_RET(remote_numa_receive_mangled_pkt, 1);
	}
}

remote_numa_receive_ret_t remote_numa_rx_advert(
	remote_numa_main_trprt_if_t *main_if,
	remote_numa_advert_t *advert)
{
	remote_numa_node_t *iter_node;
	bool present_already = false;
	rcu_read_lock();
	u32 node_id = main_if->remote_numa_node_id(advert);
	hash_for_each_possible_rcu(main_if->trprt_ctx->node_table,
				    iter_node, hnode, node_id)
	{
		if (iter_node->node_id == node_id && iter_node->advert_type == advert->hdr.type)
		{
			present_already = true;
			break;
		}
	}
	rcu_read_unlock();

	remote_numa_node_t *node;
	if (present_already)
	{
		node = iter_node;
		goto success;
	}

	node = kmalloc(sizeof(*node), GFP_ATOMIC);

	if (!node)
		return REMOTE_NUMA_TRPRT_RET(remote_numa_receive_bad_alloc, 1);
	node->advert_type = advert->hdr.type;
	node->node_id = node_id;

	unsigned long flags;
	spin_lock_irqsave(&main_if->trprt_ctx->hash_write_lock, flags);

	// Need to check again if anyone has added this node.
	present_already = false;
	hash_for_each_possible_rcu(main_if->trprt_ctx->node_table,
				    iter_node, hnode, node_id)
	{
		if (iter_node->node_id == node_id && iter_node->advert_type == advert->hdr.type)
		{
			present_already = true;
			break;
		}
	}

	if (!present_already)
	{
		node->priv_return_info = main_if->priv_return_info(advert);
		if (!node->priv_return_info)
		{
			kfree(node);
			spin_unlock_irqrestore(
				&main_if->trprt_ctx->hash_write_lock, flags);
			return REMOTE_NUMA_TRPRT_RET(
				remote_numa_receive_bad_alloc, 2);
		}
		hash_add_rcu(main_if->trprt_ctx->node_table,
				&node->hnode, node->node_id);
	}
	else
	{
		kfree(node);
		node = iter_node;
	}

	spin_unlock_irqrestore(&main_if->trprt_ctx->hash_write_lock, flags);

success:
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
	query->hdr.sender_cookie = node->node_id;
	
	return main_if->tx_msg(main_if->trprt_ctx,
		node->priv_return_info, tx_buffer_start) == 0 ?
		0 : REMOTE_NUMA_TRPRT_RET(remote_numa_receive_err_unknown, 1);
}

remote_numa_receive_ret_t remote_numa_rx_mem_query(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_query_t *query)
{
	// TODO replace this, remove from the specializatoin.
	return donor_if->rx_mem_query(donor_if->trprt_ctx->trprt_ctx, query);
}

