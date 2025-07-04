// SPDX-License-Identifier: GPL-2.0-or-later
/*
* memory.c - Remote Numa memory management.
*
* Copyright (C) 2025 Trevor Kemp
*/

#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "memory.h"

#define REMOTE_NUMA_POOL_SIZE 1024


typedef struct
{
	size_t total_pages;
	size_t free_pages;
	void* data;
} mem_pool_t;

static mem_pool_t *make_pool(void)
{
	mem_pool_t *ret = NULL;
	ret = kmalloc(sizeof(*ret), GFP_KERNEL);
	if (!ret)
		goto done;

	ret->data = vzalloc(PAGE_SIZE * REMOTE_NUMA_POOL_SIZE);
	if (!ret->data)
	{
		kfree(ret);
		ret = NULL;
		goto done;
	}
	ret->total_pages = REMOTE_NUMA_POOL_SIZE;
	ret->free_pages = REMOTE_NUMA_POOL_SIZE;

done:
	return ret;
}

static void clean_pool(mem_pool_t *pool)
{
	if (!pool)
		return;

	vfree(pool->data);
	kfree(pool);
}

remote_numa_mem_mgr_t *remote_numa_create_mem_mgr(size_t num_pages_rank)
{
	remote_numa_mem_mgr_t *mgr = kmalloc(sizeof(*mgr), GFP_KERNEL);
	if (!mgr)
		goto err;

	mgr->num_pools = (1 << num_pages_rank) / REMOTE_NUMA_POOL_SIZE;
	mgr->num_pools = mgr->num_pools == 0 ? 1 : mgr->num_pools;
	mgr->priv = kzalloc(sizeof(mem_pool_t*) * mgr->num_pools, GFP_KERNEL);
	if (!mgr->priv)
		goto err;

	for (int i = 0; i < mgr->num_pools; i++)
	{
		mem_pool_t **xp = (((mem_pool_t**)mgr->priv) + i);
		*xp = make_pool();
		if (!*xp)
			goto err;
	}

	mgr->num_pages_rank = num_pages_rank;
	mgr->free_pages = 1 << num_pages_rank;
	mgr->page_size_rank = PAGE_SHIFT;

	return mgr;
err:
	remote_numa_clean_mem_mgr(mgr);
	return NULL;
}

void remote_numa_clean_mem_mgr(remote_numa_mem_mgr_t *mgr)
{
	if (mgr)
	{
		if (mgr->priv)
		{
			for (int i = 0; i < mgr->num_pools; i++)
			{
				mem_pool_t **x = ((mem_pool_t**)mgr->priv) + i;
				clean_pool(*x);
			}
			kfree(mgr->priv);
		}
		kfree(mgr);
	}
}

