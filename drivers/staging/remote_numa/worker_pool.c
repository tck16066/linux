// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * transport.h - Remote Numa transport interface.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#include <linux/kthread.h>
#include <linux/llist.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#include "worker_pool.h"

struct worker_job {
	struct llist_node llnode;
	void *frame;
};

static struct llist_head job_list;
static struct task_struct **workers;
static wait_queue_head_t wq;
static int worker_count;
static rx_func_t handler_fn;
static bool rejecting_work;

remote_numa_receive_ret_t remote_numa_submit_frame(void *frame)
{
	if (READ_ONCE(rejecting_work))
		return REMOTE_NUMA_TRPRT_RET(
			remote_numa_receive_err_unknown, 10);

	struct worker_job *job = kmalloc(sizeof(*job), GFP_ATOMIC);
	if (!job)
		return REMOTE_NUMA_TRPRT_RET(
			remote_numa_receive_bad_alloc, 10);

	job->frame = frame;
	llist_add(&job->llnode, &job_list);
	wake_up_interruptible_nr(&wq, 1);
	return 0;
}

static int worker_fn(void *)
{
	struct llist_node *pending;
	struct worker_job *job;

	while (!kthread_should_stop()) {
		wait_event_interruptible(wq,
			!llist_empty(&job_list) ||
			kthread_should_stop());

		if (kthread_should_stop())
			break;

		pending = llist_del_all(&job_list);
		while (pending) {
			job = llist_entry(pending,
				struct worker_job, llnode);
			pending = pending->next;

			handler_fn(job->frame);
			kfree(job);
		}
	}

	return 0;
}

int remote_numa_worker_pool_init(rx_func_t handler, int num_workers)
{
	WRITE_ONCE(rejecting_work, false);
	handler_fn = handler;
	worker_count = num_workers;

	init_llist_head(&job_list);
	init_waitqueue_head(&wq);

	workers = kmalloc_array(worker_count,
		sizeof(*workers), GFP_KERNEL);
	if (!workers)
		return -ENOMEM;

	for (int i = 0; i < worker_count; i++) {
		workers[i] = kthread_run(worker_fn, NULL,
			"remote_numa_worker_%d", i);
		if (IS_ERR(workers[i]))
			return PTR_ERR(workers[i]);
	}

	return 0;
}

void remote_numa_worker_pool_stop(void)
{
	WRITE_ONCE(rejecting_work, true);
	for (int i = 0; i < worker_count; i++) {
		if (workers[i])
			kthread_stop(workers[i]);
	}
	kfree(workers);
	workers = NULL;
}

void remote_numa_worker_pool_set_reject(bool reject)
{
	WRITE_ONCE(rejecting_work, reject);
}

