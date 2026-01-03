// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * eth_transport.c - Remote Numa transport functions for ethernet.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#include <linux/etherdevice.h> 
#include <linux/hashtable.h>
#include <linux/if.h> 
#include <linux/if_ether.h>
#include <linux/jhash.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/net_custom_hook.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "eth_transport.h"
#include "protocol.h"
#include "transport.h"
#include "worker_pool.h"

#define REMOTE_NUMA_IF_NAME "eth0"
#define REMOTE_NUMA_MAX_DATA_LEN 1400

#define REMOTE_NUMA_ALLOC_TRPRT_CTX(ptr, type, err_label, __mem)			\
	do {								\
		(ptr)->trprt_ctx = remote_numa_make_trprt_ctx(__mem);	\
		if (!(ptr)->trprt_ctx) {				\
			printk(KERN_ERR					\
			       "REMOTE_NUMA: alloc generic ctx failed\n"); \
			goto err_label;					\
		}							\
		(ptr)->trprt_ctx->trprt_ctx =				\
			kzalloc(sizeof(type), GFP_KERNEL);		\
		if (!(ptr)->trprt_ctx->trprt_ctx) {			\
			printk(KERN_ERR					\
			       "REMOTE_NUMA: alloc transport ctx failed\n"); \
			goto err_label;					\
		}							\
	} while (0)


static u32 mac_to_node_id(const u8 *mac)
{
	u32 id;

	if (!mac)
		return 0;
	/* Deterministic mapping across nodes; collisions are possible but rare. */
	id = jhash(mac, ETH_ALEN, 0);
	if (id == 0)
		id = 1;
	return id;
}

static const u8 REMOTE_NUMA_SERVICE_MAC[ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

struct remote_numa_eth_trprt_ctx
{
	const char *if_name;
};

typedef struct
{
	u8 return_mac_addr[ETH_ALEN];
} remote_numa_eth_priv_ret_info_t;

static u16 max_payload_len(void)
{
	return REMOTE_NUMA_MAX_DATA_LEN;
}

static void free_rx_buff(void *buff)
{
	struct sk_buff *b = buff;
	kfree_skb(b);
}

static custom_net_hook_ret_t eth_skb_handler(struct sk_buff *skb)
{
	const struct ethhdr *eh = skb_mac_header_was_set(skb) ? eth_hdr(skb) : NULL;

	if (eh->h_proto != REMOTE_NUMA_ETHERTYPE)
		return custom_net_hook_not_consumed;

	if (remote_numa_submit_frame(skb)) {
		free_rx_buff(skb);
		return custom_net_hook_consumed_with_error;
	}

	return custom_net_hook_consumed;
}

static void *setup_skb(struct sk_buff *skb, size_t total_payload_len)
{
	skb_reserve(skb, NET_IP_ALIGN + sizeof(struct ethhdr));

	// Make sure we satisfy Ethernet minimum (46) after MH
	size_t padded_len = total_payload_len < 46 ? 46 : total_payload_len;
	skb_put(skb, padded_len);

	skb_push(skb, sizeof(struct ethhdr));
	skb_reset_mac_header(skb);

	return skb->data + sizeof(struct ethhdr);
}

static void alloc_tx_buffer(size_t payload_len, void **obj, void**payload_start)
{
	size_t alloc_len = (sizeof(struct ethhdr) + ((payload_len <
		(ETH_ZLEN - sizeof(struct ethhdr))) ? (ETH_ZLEN - sizeof(struct ethhdr)) :
		 (payload_len))) + NET_IP_ALIGN;
	struct sk_buff *txskb = alloc_skb(alloc_len, GFP_KERNEL);

	if (!txskb)
	{
		*obj = NULL;
		*payload_start = NULL;
		return;
	}
	*payload_start = setup_skb(txskb, payload_len);
	*obj = txskb;
}

static remote_numa_send_ret_t eth_priv_return_info(
	void *trprt_ctx,
	remote_numa_main_return_info_t *info)
{
	struct remote_numa_eth_trprt_ctx *eth_ctx = trprt_ctx;
	struct net_device *dev = dev_get_by_name_rcu(&init_net, eth_ctx->if_name);
	if (!dev)
	{
		return REMOTE_NUMA_TRPRT_RET(remote_numa_send_no_if, 3);
	}
	ether_addr_copy(info->abstract_info, dev->dev_addr);

	return 0;
}

static void* eth_priv_return_info_from_advert(remote_numa_advert_t *advert)
{
	remote_numa_eth_priv_ret_info_t *info =
		kmalloc(sizeof(*info), GFP_KERNEL);
	if (info)
	{
		ether_addr_copy(info->return_mac_addr,
			advert->return_info.abstract_info);
	}
	return info;
}

static void* eth_priv_return_info_from_mem_query(remote_numa_mem_query_t *mem)
{
	remote_numa_eth_priv_ret_info_t *info =
		kmalloc(sizeof(*info), GFP_KERNEL);
	if (info)
	{
		ether_addr_copy(info->return_mac_addr, mem->return_info.abstract_info);
	}
	return info;
}

static void eth_free_priv_return_info(void *info)
{
	kfree(info);
}

void remote_numa_clean_donor_trprt_if(remote_numa_donor_trprt_if_t *iface)
{
	// TODO make a cleaner function in the if for the ctx, then move this
	// calling function to transport.c
	rcu_assign_pointer(custom_net_hook, NULL);

	if (!iface)
		return;

	if (iface->trprt_ctx->trprt_ctx)
		kfree(iface->trprt_ctx->trprt_ctx);

	if (iface->trprt_ctx)
		remote_numa_transport_ctx_destroy(iface->trprt_ctx);
}

void remote_numa_clean_main_trprt_if(remote_numa_main_trprt_if_t *iface)
{
	// TODO make a cleaner function in the if for the ctx, then move this
	// calling function to transport.c
	rcu_assign_pointer(custom_net_hook, NULL);

	if (!iface)
		return;

	if (iface->trprt_ctx->trprt_ctx)
		kfree(iface->trprt_ctx->trprt_ctx);

	if (iface->trprt_ctx)
		remote_numa_transport_ctx_destroy(iface->trprt_ctx);
}

static u32 advert_to_node_id(remote_numa_advert_t *ad)
{
	return mac_to_node_id(ad->return_info.abstract_info);
}

static u32 mem_query_to_node_id(remote_numa_mem_query_t *mem)
{
	return mac_to_node_id(mem->return_info.abstract_info);
}

static u32 rx_skb_to_node_id(void *rx_data, void *payload)
{
	struct sk_buff *skb = rx_data;
	struct ethhdr *eth;

	if (!skb)
		return 0;
	eth = eth_hdr(skb);
	if (!eth)
		return 0;
	return mac_to_node_id(eth->h_source);
}

static remote_numa_send_ret_t send_msg(struct remote_numa_eth_trprt_ctx *ctx,
				       struct sk_buff *skb,
				       const u8* dst_mac)
{
	/* TODO this function should be called by a helper that accepts a
 	*  list of skb and acquires the lock one time.
 	*/

	remote_numa_send_ret_t ret = 0;

	struct ethhdr *eth = eth_hdr(skb);
	ether_addr_copy(eth->h_dest, dst_mac);
	eth->h_proto = REMOTE_NUMA_ETHERTYPE;
	skb->pkt_type = PACKET_OUTGOING;

	bool in_rcu = rcu_read_lock_held();
	if (!in_rcu)
		rcu_read_lock();

	struct net_device *dev = dev_get_by_name_rcu(&init_net, ctx->if_name);
	if (!dev)
	{
		ret = REMOTE_NUMA_TRPRT_RET(remote_numa_send_no_if, 1);
		goto done;
	}

	skb->dev = dev;
	ether_addr_copy(eth->h_source, dev->dev_addr);

	if (0 != dev_queue_xmit(skb))
	{
		/*
 		 * XXX: In this case, we have dropped a frame. This won't always be
 		 * terrible since the other end can just re-request. But we do need
 		 * to carefully handle the case of segmented data drops.
 		 */ 
		skb = NULL;
		ret = REMOTE_NUMA_TRPRT_RET(remote_numa_send_bad_xmit, 1);
		goto done;
	}
	skb = NULL;

done:
	if(!in_rcu)
		rcu_read_unlock();
	kfree(skb);
	return ret;
}

static remote_numa_send_ret_t tx_msg(remote_numa_trprt_ctx_t *trprt_ctx,
	void *return_info, void *msg)
{
	return send_msg(trprt_ctx->trprt_ctx, msg, return_info);
}

static void prepare_rx_buff(void *rx_buff, void **payload)
{

	struct sk_buff *skb = rx_buff;
	skb_linearize(skb);

	u8 *pay = skb_mac_header_was_set(skb) ?
		skb_mac_header(skb) + ETH_HLEN :
		skb->data + ETH_HLEN;

	*payload = pay;
}

static remote_numa_send_ret_t remote_numa_eth_tx_advert(remote_numa_trprt_ctx_t *trprt_ctx)
{
	/* The error path should not unlock if we did not
 	 * acquire the lock in this function.
 	 */
	bool took_rcu = false;
	remote_numa_send_ret_t ret = 0;
	struct sk_buff *txskb = NULL;
	remote_numa_advert_t *payload_start = NULL;
	void **v_txskb = (void**)&txskb;
	void **v_payload_start = (void**)&payload_start;

	alloc_tx_buffer(sizeof(*payload_start), v_txskb, v_payload_start);
	
	if (!txskb)
	{
		ret = REMOTE_NUMA_TRPRT_RET(remote_numa_send_bad_alloc, 1);
		goto done;
	}
	
	payload_start->max_frame_len = max_payload_len();
	
	remote_numa_msg_hdr_t *hdr = &payload_start->hdr;
	hdr->version = remote_numa_protocol_0_1;
	hdr->type = remote_numa_eth_advert;
	hdr->main_cookie = 0;
	hdr->donor_cookie = 0;

	if (!rcu_read_lock_held())
	{
		rcu_read_lock();
		took_rcu = true;
	}
	struct remote_numa_eth_trprt_ctx *ctx =
		trprt_ctx->trprt_ctx;
	struct net_device *dev =
		dev_get_by_name_rcu(&init_net, ctx->if_name);
	if (!dev)
	{
		ret = REMOTE_NUMA_TRPRT_RET(remote_numa_send_no_if, 2);
		goto done;
	}
	ether_addr_copy(payload_start->return_info.abstract_info, dev->dev_addr);

	ret = send_msg(
		trprt_ctx->trprt_ctx, txskb, REMOTE_NUMA_SERVICE_MAC);

done:
	if(took_rcu)
		rcu_read_unlock();
	return ret;
}

static remote_numa_receive_ret_t remote_numa_eth_rx_mem_query
    (remote_numa_trprt_ctx_t *trprt_ctx, remote_numa_mem_query_t *query)
{
	return REMOTE_NUMA_TRPRT_RET(remote_numa_receive_err_unknown, 0);
}

static remote_numa_send_ret_t remote_numa_eth_tx_mem_resp
    (remote_numa_trprt_ctx_t *trprt_ctx, remote_numa_mem_resp_t *resp)
{
	return REMOTE_NUMA_TRPRT_RET(remote_numa_send_err_unknown, 0);
}

static remote_numa_receive_ret_t remote_numa_eth_rx_mem_resp
    (remote_numa_trprt_ctx_t *trprt_ctx, remote_numa_mem_resp_t *resp)
{
	return REMOTE_NUMA_TRPRT_RET(remote_numa_receive_err_unknown, 0);
}

remote_numa_donor_trprt_if_t *remote_numa_eth_donor_init(struct remote_numa_mem_mgr *mem)
{
	remote_numa_donor_trprt_if_t *ptr = kzalloc(sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
	{
		printk(KERN_ERR "REMOTE_NUMA Failure to allocate.");
		goto err;
	}

	ptr->priv_return_info_from_mem_query = eth_priv_return_info_from_mem_query;
	ptr->remote_numa_node_id = mem_query_to_node_id;
	ptr->remote_numa_rx_node_id = rx_skb_to_node_id;
	ptr->get_max_payload_len = max_payload_len;
	ptr->alloc_tx_buffer = alloc_tx_buffer;
	ptr->prepare_rx_buff = prepare_rx_buff;
	ptr->free_rx_buff = free_rx_buff;
	ptr->tx_advert = remote_numa_eth_tx_advert;
	ptr->rx_mem_query = remote_numa_eth_rx_mem_query;
	ptr->tx_mem_resp = remote_numa_eth_tx_mem_resp;
	ptr->tx_msg = tx_msg;

	REMOTE_NUMA_ALLOC_TRPRT_CTX(ptr,
		struct remote_numa_eth_trprt_ctx, err, mem);
	struct remote_numa_eth_trprt_ctx *eth_ctx = ptr->trprt_ctx->trprt_ctx;
	eth_ctx->if_name = REMOTE_NUMA_IF_NAME;

	rcu_assign_pointer(custom_net_hook, eth_skb_handler);

	return ptr;
err:
	if (ptr)
		remote_numa_transport_ctx_destroy(ptr->trprt_ctx);
	return NULL;
}

remote_numa_main_trprt_if_t *remote_numa_eth_main_init(void)
{
	remote_numa_main_trprt_if_t *ptr = kzalloc(sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
	{
		printk(KERN_ERR "REMOTE_NUMA Failure to allocate.");
		goto err;
	}

	ptr->get_max_payload_len = max_payload_len;
	ptr->prepare_rx_buff = prepare_rx_buff;
	ptr->free_rx_buff = free_rx_buff;
	ptr->alloc_tx_buffer = alloc_tx_buffer;
	ptr->remote_numa_node_id = advert_to_node_id;
	ptr->priv_return_info = eth_priv_return_info;
	ptr->priv_return_info_from_advert = eth_priv_return_info_from_advert;
	ptr->free_priv_return_info = eth_free_priv_return_info;
	ptr->rx_mem_resp = remote_numa_eth_rx_mem_resp;
	ptr->tx_msg = tx_msg;

	REMOTE_NUMA_ALLOC_TRPRT_CTX(ptr,
		struct remote_numa_eth_trprt_ctx, err, NULL);
	struct remote_numa_eth_trprt_ctx *eth_ctx = ptr->trprt_ctx->trprt_ctx;
	eth_ctx->if_name = REMOTE_NUMA_IF_NAME;
	rcu_assign_pointer(custom_net_hook, eth_skb_handler);

	return ptr;
err:
	if (ptr)
		remote_numa_transport_ctx_destroy(ptr->trprt_ctx);
	return NULL;
}

