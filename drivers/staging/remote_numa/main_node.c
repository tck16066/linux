// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * main_node.c - The node the user interacts with.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#include <linux/compiler.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/net_custom_hook.h>
#include <linux/skbuff.h>
#include <linux/types.h>

#include "client_cache.h"
#include "transport.h"
#include "eth_transport.h"
#include "protocol.h"
#include "worker_pool.h"

#define WORKER_POOL_SIZE 4
//#define REMOTE_NUMA_MAX_CACHED_PGS 128000
#define REMOTE_NUMA_MAX_CACHED_PGS 1200

remote_numa_main_trprt_if_t *ctx = NULL;
remote_numa_client_cache_t *client_cache = NULL;
EXPORT_SYMBOL(client_cache);

static int rx_wrapper(void *frame)
{
	void *payload = NULL;
	ctx->prepare_rx_buff(frame, &payload);
	return remote_numa_main_rx(ctx, frame, payload);
}

static int __init
remote_numa_main_node_init(void)
{
	tmp_init();
	remote_numa_worker_pool_init(rx_wrapper, WORKER_POOL_SIZE);
	ctx = remote_numa_eth_main_init();
	if (!ctx)
	{
		printk(KERN_ERR "REMOTE_NUMA main bad init.");
		return -ENOMEM;
	}
	client_cache = kzalloc(sizeof(*client_cache), GFP_KERNEL);
	if (!client_cache)
	{
		printk(KERN_ERR "Could not alloc client cache.");
		return -ENOMEM;
	}

	int cache_init = -1;
	if ((cache_init = remote_numa_client_cache_init(
		client_cache,
		ctx,
		REMOTE_NUMA_MAX_CACHED_PGS)))
	{
		printk(KERN_ERR "Could not init client cache.");
		return cache_init;
	}
	ctx->client_cache = client_cache;
	return 0;
}

static void __exit
remote_numa_main_node_exit(void)
{
	/* Once any pages have been allocated, this is essentially
 	 * never safe to call*/
	remote_numa_worker_pool_set_reject(true);
	remote_numa_worker_pool_stop();
	remote_numa_clean_main_trprt_if(ctx);
	remote_numa_client_cache_destroy(client_cache);
}

module_init(remote_numa_main_node_init);
module_exit(remote_numa_main_node_exit);
MODULE_LICENSE("GPL");
