// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * donor_node.c - The node the user interacts with.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/net_custom_hook.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/skbuff.h>

#include "eth_transport.h"
#include "protocol.h"

#define ADVERT_MIN_SLEEP_MS 3000
#define ADVERT_RANDOM_SLEEP_MS_MAX 500

static struct task_struct *kthread = NULL;
remote_numa_donor_trprt_if_t *ctx = NULL;

static custom_net_hook_ret_t
donor_skb_handler(struct sk_buff *skb)
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
	
	remote_numa_receive_ret_t tmp_ret = remote_numa_donor_rx(ctx, payload);
	kfree_skb(skb);

	if (tmp_ret)
	{
		return custom_net_hook_consumed_with_error;
	}

	return custom_net_hook_consumed;

}

static int
advert_thread(void *data)
{
	while (!kthread_should_stop())
	{
		ctx->tx_advert(ctx->trprt_ctx);
		u32 sleep_ms = ADVERT_MIN_SLEEP_MS +
		    get_random_u32() % ADVERT_RANDOM_SLEEP_MS_MAX;
		msleep(sleep_ms);
	}
	return 0;
}

static int __init
remote_numa_donor_node_init(void)
{
	ctx = remote_numa_eth_donor_init(donor_skb_handler);
	if (ctx)
	{
		kthread = kthread_run(advert_thread, NULL, "remote_numa_advert");
		if (IS_ERR(kthread)) {
			printk(KERN_ERR "REMOTE_NUMA donor advert kthread failed\n");
			remote_numa_transport_ctx_destroy(ctx->trprt_ctx);
			return PTR_ERR(kthread);
		}
	}
	else
	{
		printk(KERN_ERR "REMOTE_NUMA donor bad init.");
		return -ENOMEM;
	}
	return 0;
}

static void __exit
remote_numa_donor_node_exit(void)
{
	printk(KERN_INFO "REMOTE_NUMA donor cleanup\n");

	if (kthread)
		kthread_stop(kthread);
	
	remote_numa_clean_donor_trprt_if(ctx);
}

module_init(remote_numa_donor_node_init);
module_exit(remote_numa_donor_node_exit);
MODULE_LICENSE("GPL");
