// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * donor_node.c - The node the user interacts with.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/llist.h>
#include <linux/net_custom_hook.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/skbuff.h>

#include "eth_transport.h"
#include "protocol.h"
#include "worker_pool.h"

#define ADVERT_MIN_SLEEP_MS 3000
#define ADVERT_RANDOM_SLEEP_MS_MAX 500
#define WORKER_POOL_SIZE 4

#define NUM_PGS_RANK 18

static struct task_struct *kthread = NULL;
static remote_numa_donor_trprt_if_t *ctx = NULL;
static remote_numa_mem_mgr_t *mem;

static remote_numa_receive_ret_t rx_wrapper(void *frame)
{
	void *payload = NULL;
	ctx->prepare_rx_buff(frame, &payload);
	return remote_numa_donor_rx(ctx, frame, payload);
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
	mem = remote_numa_create_mem_mgr(NUM_PGS_RANK);
	if (!mem)
		return -ENOMEM;
	remote_numa_worker_pool_init(rx_wrapper, WORKER_POOL_SIZE);

	ctx = remote_numa_eth_donor_init(mem);
	if (ctx)
	{
		kthread = kthread_run(advert_thread, NULL, "remote_numa_advert");
		if (IS_ERR(kthread)) {
			printk(KERN_ERR "REMOTE_NUMA donor advert kthread failed\n");
			remote_numa_transport_ctx_destroy(ctx->trprt_ctx);
			remote_numa_clean_mem_mgr(mem);
			return PTR_ERR(kthread);
		}
	}
	else
	{
		printk(KERN_ERR "REMOTE_NUMA donor bad init.");
		remote_numa_clean_mem_mgr(mem);
		return -ENOMEM;
	}
	return 0;
}

static void __exit
remote_numa_donor_node_exit(void)
{
	remote_numa_worker_pool_set_reject(true);

	if (kthread)
		kthread_stop(kthread);

	remote_numa_worker_pool_stop();
	remote_numa_clean_donor_trprt_if(ctx);
	remote_numa_clean_mem_mgr(mem);
}

module_init(remote_numa_donor_node_init);
module_exit(remote_numa_donor_node_exit);
MODULE_LICENSE("GPL");
