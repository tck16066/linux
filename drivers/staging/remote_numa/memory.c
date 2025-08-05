// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * memory.c - Remote Numa memory management (donor-side).
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/errno.h>

#include "memory.h"
#include "client_cache.h"

#define REMOTE_NUMA_POOL_SIZE 1024
#define REMOTE_NUMA_COOKIE_HASH_BITS 14

typedef struct {
	void *data;
	remote_numa_page_t *pages;
} mem_pool_t;

static u32 cookie_hash(u64 cookie) {
	return hash_64(cookie, REMOTE_NUMA_COOKIE_HASH_BITS);
}

static mem_pool_t *make_pool(void)
{
	mem_pool_t *pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return NULL;

	pool->data = vzalloc(PAGE_SIZE * REMOTE_NUMA_POOL_SIZE);
	if (!pool->data)
		goto fail;

	pool->pages = kcalloc(REMOTE_NUMA_POOL_SIZE, sizeof(remote_numa_page_t), GFP_KERNEL);
	if (!pool->pages)
		goto fail;

	return pool;

fail:
	if (pool) {
		vfree(pool->data);
		kfree(pool);
	}
	return NULL;
}

static void clean_pool(mem_pool_t *pool)
{
	if (!pool) return;
	vfree(pool->data);
	kfree(pool->pages);
	kfree(pool);
}

struct remote_numa_mem_mgr *remote_numa_create_mem_mgr(size_t num_pages)
{
	struct remote_numa_mem_mgr *mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);
	if (!mgr)
		return NULL;

	atomic64_set(&mgr->cookie_gen, 123);

	mgr->num_pools = DIV_ROUND_UP(num_pages, REMOTE_NUMA_POOL_SIZE);
	mgr->num_pages = mgr->num_pools * REMOTE_NUMA_POOL_SIZE;
	mgr->free_pages = mgr->num_pages;
	mgr->page_size_rank = PAGE_SHIFT;
	mgr->cookie_hash_bits = REMOTE_NUMA_COOKIE_HASH_BITS;
	INIT_LIST_HEAD(&mgr->free_list);
	spin_lock_init(&mgr->lock);
	mgr->cookie_table = kmalloc_array(1 << REMOTE_NUMA_COOKIE_HASH_BITS,
	                                  sizeof(struct hlist_head),
	                                  GFP_KERNEL);
	if (!mgr->cookie_table)
		goto fail;

	for (int i = 0; i < (1 << REMOTE_NUMA_COOKIE_HASH_BITS); i++)
		INIT_HLIST_HEAD(&mgr->cookie_table[i]);

	mem_pool_t **pools = kmalloc_array(mgr->num_pools,
	                                   sizeof(mem_pool_t *),
	                                   GFP_KERNEL);
	if (!pools)
		goto fail;

	for (size_t p = 0; p < mgr->num_pools; ++p) {
		mem_pool_t *pool = make_pool();
		if (!pool)
			goto fail;

		for (int i = 0; i < REMOTE_NUMA_POOL_SIZE; ++i) {
			remote_numa_page_t *pg = &pool->pages[i];
			pg->addr = pool->data + i * PAGE_SIZE;
			pg->donor_pg_cookie = atomic64_inc_return(&mgr->cookie_gen);
			pg->in_use = false;
			INIT_LIST_HEAD(&pg->list);

			list_add_tail(&pg->list, &mgr->free_list);

			u32 h = cookie_hash(pg->donor_pg_cookie);
			hlist_add_head(&pg->hnode, &mgr->cookie_table[h]);
		}

		pools[p] = pool;
	}

	mgr->priv = pools;
	return mgr;

fail:
	remote_numa_clean_mem_mgr(mgr);
	return NULL;
}

void remote_numa_clean_mem_mgr(struct remote_numa_mem_mgr *mgr)
{
	if (!mgr)
		return;

	if (mgr->priv) {
		for (size_t i = 0; i < mgr->num_pools; ++i)
			clean_pool(((mem_pool_t **)mgr->priv)[i]);
		kfree(mgr->priv);
	}

	kfree(mgr->cookie_table);
	kfree(mgr);
}

int remote_numa_mem_alloc_page(struct remote_numa_mem_mgr *mgr,
                               u64 *cookie_out,
                               void **page_out)
{
	unsigned long flags;
	spin_lock_irqsave(&mgr->lock, flags);
	if (list_empty(&mgr->free_list)) {
		spin_unlock_irqrestore(&mgr->lock, flags);
		return -ENOMEM;
	}

	remote_numa_page_t *pg = list_first_entry(&mgr->free_list, remote_numa_page_t, list);
	list_del(&pg->list);
	pg->in_use = true;
	mgr->free_pages--;

	*cookie_out = pg->donor_pg_cookie;
	*page_out = pg->addr;

	spin_unlock_irqrestore(&mgr->lock, flags);
	return 0;
}

int remote_numa_mem_lookup_page(struct remote_numa_mem_mgr *mgr,
                                u64 cookie,
                                void **page_out,
				remote_numa_page_t **rn_pg_out)
{
	u32 h = cookie_hash(cookie);
	struct hlist_head *head = &mgr->cookie_table[h];
	remote_numa_page_t *pg;
	
	unsigned long flags;
	spin_lock_irqsave(&mgr->lock, flags);

	hlist_for_each_entry(pg, head, hnode) {
		if (pg->donor_pg_cookie == cookie) {
		spin_unlock_irqrestore(&mgr->lock, flags);
			*page_out = pg->addr;
			*rn_pg_out = pg;
			return 0;
		}
	}
	spin_unlock_irqrestore(&mgr->lock, flags);
	return -ENOENT;
}

int remote_numa_mem_sync_to_donor(struct remote_numa_mem_mgr *mgr,
                                  u64 cookie,
                                  void *src_buf,
                                  size_t len,
                                  size_t offset)
{
	void *dst;
	remote_numa_page_t *cached_pg;
	int ret = remote_numa_mem_lookup_page(mgr, cookie, &dst, &cached_pg);
	if (ret)
		return ret;

	if (offset + len > PAGE_SIZE)
		return -EINVAL;

	memcpy(dst + offset, src_buf, len);
	return 0;
}

int remote_numa_mem_sync_from_donor(struct remote_numa_mem_mgr *mgr,
                                    u64 cookie,
                                    void **src_buf_out,
                                    size_t *len_out)
{
	void *src;
	remote_numa_page_t *cached_pg;
	int ret = remote_numa_mem_lookup_page(mgr, cookie, &src, &cached_pg);
	if (ret)
		return ret;

	*src_buf_out = src;
	*len_out = PAGE_SIZE;
	return 0;
}

int remote_numa_mem_free_page(struct remote_numa_mem_mgr *mgr,
                              u64 cookie)
{
printk("remote_numa_mem_free_page  mgr  %px   cookie %llu\n",
	mgr, cookie);
	u32 h = cookie_hash(cookie);
	struct hlist_head *head = &mgr->cookie_table[h];
	remote_numa_page_t *pg;

	unsigned long flags;
	spin_lock_irqsave(&mgr->lock, flags);
	hlist_for_each_entry(pg, head, hnode)
	{
		if (pg->donor_pg_cookie == cookie) {
			if (!pg->in_use) {
				spin_unlock_irqrestore(&mgr->lock, flags);
				return -EINVAL;
			}
			pg->in_use = false;
			list_add_tail(&pg->list, &mgr->free_list);
			mgr->free_pages++;
			spin_unlock_irqrestore(&mgr->lock, flags);
			return 0;
		}
	}
	spin_unlock_irqrestore(&mgr->lock, flags);
	return -ENOENT;
}

EXPORT_SYMBOL_GPL(remote_numa_create_mem_mgr);
EXPORT_SYMBOL_GPL(remote_numa_clean_mem_mgr);
