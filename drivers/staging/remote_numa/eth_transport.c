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
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/net_custom_hook.h>
#include <linux/rcupdate.h>
#include <linux/siphash.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "eth_transport.h"
#include "protocol.h"
#include "transport.h"

#define REMOTE_NUMA_IF_NAME "eth0"
#define REMOTE_NUMA_MAX_DATA_LEN 1500



#define REMOTE_NUMA_SKB_SIZE(_size, _additional)				\
	(sizeof(struct ethhdr) +						\
	 (((_size + (_additional)) <					\
	   (ETH_ZLEN - sizeof(struct ethhdr))) ?		\
	  (ETH_ZLEN - sizeof(struct ethhdr)) :		\
	  (_size + (_additional))) +					\
	 NET_IP_ALIGN)

#define REMOTE_NUMA_SKB_SIZE_FOR_TYPE(_T_, __additional)				\
	REMOTE_NUMA_SKB_SIZE(sizeof(_T_), __additional)

#define REMOTE_NUMA_ALLOC_TRPRT_CTX(ptr, type, err_label)			\
	do {								\
		(ptr)->trprt_ctx = remote_numa_make_trprt_ctx();	\
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


static siphash_key_t net_secret;

static const u8 REMOTE_NUMA_SERVICE_MAC[ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

struct remote_numa_eth_trprt_ctx
{
	const char *if_name;
	u16 max_data_len;
};

typedef struct
{
	remote_numa_advert_t generic_advert;
	u8 src_mac_addr[ETH_ALEN];
}  __attribute__((__packed__)) remote_numa_eth_advert_t;

typedef struct
{
	u8 return_mac_addr[ETH_ALEN];
} remote_numa_eth_priv_ret_info_t;

static void* setup_skb(struct sk_buff *txskb, size_t data_len)
{
	skb_reserve(txskb, NET_IP_ALIGN + sizeof(struct ethhdr));
	data_len = data_len < ETH_ZLEN ? ETH_ZLEN : data_len;
	void *data  = skb_put(txskb, data_len);
	skb_push(txskb, sizeof(struct ethhdr));
	skb_reset_mac_header(txskb);

	return data;
}

static void alloc_tx_buffer(size_t payload_len, void **obj, void**payload_start)
{
	struct sk_buff *txskb = alloc_skb(
	    REMOTE_NUMA_SKB_SIZE(payload_len, 0),
	    GFP_ATOMIC);

	if (!txskb)
	{
		*obj = NULL;
		*payload_start = NULL;
		return;
	}
	*payload_start = setup_skb(txskb,
		REMOTE_NUMA_SKB_SIZE(payload_len, 0));
	*obj = txskb;
}

static void* eth_priv_return_info(remote_numa_advert_t *advert)
{
	remote_numa_eth_advert_t *eth_ad = (remote_numa_eth_advert_t*)advert;
	remote_numa_eth_priv_ret_info_t *info =
		kmalloc(sizeof(*info), GFP_KERNEL);

	if (info)
	{
		ether_addr_copy(info->return_mac_addr, eth_ad->src_mac_addr);
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

	if (iface->trprt_ctx)
	{
		kfree(iface->trprt_ctx);
	}
	remote_numa_transport_ctx_destroy(iface->trprt_ctx);
	
}

void remote_numa_clean_main_trprt_if(remote_numa_main_trprt_if_t *iface)
{
	get_random_bytes(&net_secret, sizeof(net_secret));

	// TODO make a cleaner function in the if for the ctx, then move this
	// calling function to transport.c
	rcu_assign_pointer(custom_net_hook, NULL);

	if (!iface)
		return;

	if (iface->trprt_ctx)
	{
		kfree(iface->trprt_ctx);
	}
	remote_numa_transport_ctx_destroy(iface->trprt_ctx);
}

static u32 advert_to_node_id(remote_numa_advert_t *ad)
{
	/*
 	 * Cast is ok because we know that this advert type is only sent by this
 	 * transport type (or we would once we got an auth model working!).
 	 */
	remote_numa_eth_advert_t* eth = (remote_numa_eth_advert_t*)ad;
	return hsiphash(eth->src_mac_addr,
		ETH_ALEN, (const hsiphash_key_t *) &net_secret);
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

static remote_numa_send_ret_t remote_numa_eth_tx_advert(remote_numa_trprt_ctx_t *trprt_ctx)
{
	/* The error path should not unlock if we did not
 	 * acquire the lock in this function.
 	 */
	bool took_rcu = false;
	remote_numa_send_ret_t ret = 0;
	struct sk_buff *txskb = alloc_skb(
	    REMOTE_NUMA_SKB_SIZE_FOR_TYPE(remote_numa_eth_advert_t, 0),
	    GFP_ATOMIC);
	if (!txskb)
	{
		ret = REMOTE_NUMA_TRPRT_RET(remote_numa_send_bad_alloc, 1);
		goto done;
	}
	
	remote_numa_eth_advert_t *payload_start =
	    setup_skb(txskb,
	        REMOTE_NUMA_SKB_SIZE_FOR_TYPE(remote_numa_eth_advert_t, 0));
	payload_start->generic_advert.max_frame_len =
	    ((struct remote_numa_eth_trprt_ctx *)trprt_ctx->trprt_ctx)->max_data_len;
	
	remote_numa_msg_hdr_t *hdr = &payload_start->generic_advert.hdr;
	hdr->version = remote_numa_eth_advert;
	hdr->type = remote_numa_protocol_0_1;
	hdr->sender_cookie = 0;

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
	ether_addr_copy(payload_start->src_mac_addr, dev->dev_addr);

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
	printk("rx a mem query");
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

remote_numa_donor_trprt_if_t *remote_numa_eth_donor_init(skb_handler_t handler)
{
	get_random_bytes(&net_secret, sizeof(net_secret));

	remote_numa_donor_trprt_if_t *ptr = kzalloc(sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
	{
		printk(KERN_ERR "REMOTE_NUMA Failure to allocate.");
		goto err;
	}

	ptr->tx_advert = remote_numa_eth_tx_advert;
	ptr->rx_mem_query = remote_numa_eth_rx_mem_query;
	ptr->tx_mem_resp = remote_numa_eth_tx_mem_resp;

	REMOTE_NUMA_ALLOC_TRPRT_CTX(ptr, struct remote_numa_eth_trprt_ctx, err);
	struct remote_numa_eth_trprt_ctx *eth_ctx = ptr->trprt_ctx->trprt_ctx;
	eth_ctx->if_name = REMOTE_NUMA_IF_NAME;
	eth_ctx->max_data_len =
		REMOTE_NUMA_MAX_DATA_LEN - sizeof(remote_numa_msg_hdr_t);

	rcu_assign_pointer(custom_net_hook, handler);

	return ptr;
err:
	if (ptr)
		remote_numa_transport_ctx_destroy(ptr->trprt_ctx);
	return NULL;
}

remote_numa_main_trprt_if_t *remote_numa_eth_main_init(skb_handler_t handler)
{
	remote_numa_main_trprt_if_t *ptr = kzalloc(sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
	{
		printk(KERN_ERR "REMOTE_NUMA Failure to allocate.");
		goto err;
	}

	ptr->alloc_tx_buffer = alloc_tx_buffer;
	ptr->remote_numa_node_id = advert_to_node_id;
	ptr->priv_return_info = eth_priv_return_info;
	ptr->free_priv_return_info = eth_free_priv_return_info;
	ptr->rx_mem_resp = remote_numa_eth_rx_mem_resp;
	ptr->tx_msg = tx_msg;

	REMOTE_NUMA_ALLOC_TRPRT_CTX(ptr, struct remote_numa_eth_trprt_ctx ,err);
	struct remote_numa_eth_trprt_ctx *eth_ctx = ptr->trprt_ctx->trprt_ctx;
	eth_ctx->if_name = REMOTE_NUMA_IF_NAME;
	eth_ctx->max_data_len =
		REMOTE_NUMA_MAX_DATA_LEN - sizeof(remote_numa_msg_hdr_t);

	rcu_assign_pointer(custom_net_hook, handler);

	return ptr;
err:
	if (ptr)
		remote_numa_transport_ctx_destroy(ptr->trprt_ctx);
	return NULL;
}

