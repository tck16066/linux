// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * main_node.c - The node the user interacts with.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/net_custom_hook.h>
#include <linux/skbuff.h>
#include <linux/types.h>

#include "eth_transport.h"
#include "protocol.h"

remote_numa_main_trprt_if_t *ctx = NULL;

static custom_net_hook_ret_t
main_node_skb_handler(struct sk_buff *skb)
{
	/*
 	 * TODO we should really be doing any work on a worker thread
 	 * and not in the unterrupt context.
 	 */  
	if (skb->protocol != REMOTE_NUMA_ETHERTYPE)
		return custom_net_hook_not_consumed;

	u8 *payload;
	if (skb_mac_header_was_set(skb))
		payload = skb_mac_header(skb) + ETH_HLEN;
	else
		payload = skb->data + ETH_HLEN;

	remote_numa_receive_ret_t tmp_ret = remote_numa_main_rx(ctx, payload);
	kfree_skb(skb);

	if (tmp_ret)
	{
		return custom_net_hook_consumed_with_error;
	}

	return custom_net_hook_consumed;
}

static int __init
remote_numa_main_node_init(void)
{
	ctx = remote_numa_eth_main_init(main_node_skb_handler);
	if (!ctx)
	{
		printk(KERN_ERR "REMOTE_NUMA main bad init.");
		return -ENOMEM;
	}
	return 0;
}

static void __exit
remote_numa_main_node_exit(void)
{
	custom_net_hook = NULL;
	printk(KERN_INFO "Unregistered custom handler\n");
}

module_init(remote_numa_main_node_init);
module_exit(remote_numa_main_node_exit);
MODULE_LICENSE("GPL");
