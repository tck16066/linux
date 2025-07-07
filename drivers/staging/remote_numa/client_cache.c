// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * client.c - Remote Numa client APIs.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include "client_cache.h"

int remote_numa_client_cache_init(remote_numa_client_cache_t *cache,
				  remote_numa_main_trprt_if_t *trprt,
				  u32 max_cached_pages)
{
	INIT_LIST_HEAD(&cache->lru_head);
	spin_lock_init(&cache->lock);
	init_waitqueue_head(&cache->waitq);
	cache->max_cached_pages = max_cached_pages;
	cache->current_cached_pages = 0;
	cache->trprt = trprt;
	return 0;
}

void remote_numa_client_cache_destroy(remote_numa_client_cache_t *cache)
{
	struct list_head *pos, *tmp;
	remote_numa_cached_page_t *entry;

	list_for_each_safe(pos, tmp, &cache->lru_head) {
		entry = list_entry(pos, remote_numa_cached_page_t, lru_list);
		list_del(pos);
		__free_page(entry->page);
		kfree(entry);
	}
	cache->current_cached_pages = 0;
}

static void cache_insert(remote_numa_client_cache_t *cache,
			 remote_numa_cached_page_t *entry)
{
	spin_lock(&cache->lock);
	list_add(&entry->lru_list, &cache->lru_head);
	cache->current_cached_pages++;
	spin_unlock(&cache->lock);
}

struct page *remote_numa_client_cache_alloc(remote_numa_client_cache_t *cache,
					    u32 donor_node_id)
{
	struct page *page = alloc_page(GFP_ATOMIC);
	if (!page)
		return NULL;

	remote_numa_cached_page_t *entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		__free_page(page);
		return NULL;
	}

	entry->page = page;
	entry->donor_node_id = donor_node_id;
	entry->main_pg_cookie = (u32)(uintptr_t)page_to_virt(page);

	if (remote_numa_transport_alloc_page(cache->trprt, donor_node_id,
					     page, entry)) {
		__free_page(page);
		kfree(entry);
		return NULL;
	}

	cache_insert(cache, entry);
	return page;
}

int remote_numa_client_cache_refault(remote_numa_client_cache_t *cache,
				     struct page *faulting_page)
{
	u32 main_cookie = (u32)(uintptr_t)page_to_virt(faulting_page);
	u32 donor_node_id = 0;
	u32 donor_pg_cookie = 0;

	remote_numa_cached_page_t *entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return -ENOMEM;

	entry->page = faulting_page;
	entry->main_pg_cookie = main_cookie;
	entry->donor_node_id = donor_node_id;
	entry->donor_pg_cookie = donor_pg_cookie;

	if (remote_numa_transport_refetch_page(cache->trprt, donor_node_id,
					       donor_pg_cookie, faulting_page,
					       entry)) {
		kfree(entry);
		return -EIO;
	}

	cache_insert(cache, entry);
	return 0;
}

