// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * client.h - Remote Numa client APIs.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/rmap.h>
#include "client_cache.h"

static int maybe_evict(remote_numa_client_cache_t *cache)
{
	if (cache->current_cached_pages >= cache->max_cached_pages) {
		remote_numa_cached_page_t *victim;

		spin_lock(&cache->lock);
		if (list_empty(&cache->lru_head)) {
			spin_unlock(&cache->lock);
			return -ENOMEM;
		}
		victim = list_last_entry(&cache->lru_head,
					 remote_numa_cached_page_t,
					 lru_list);
		list_del(&victim->lru_list);
		cache->current_cached_pages--;
		spin_unlock(&cache->lock);

		if (page_mapped(victim->page))
			try_to_unmap(victim->page,
				     TTU_UNMAP | TTU_IGNORE_MLOCK);

		remote_numa_transport_sync_page(cache->trprt,
						victim->donor_node_id,
						victim->donor_pg_cookie,
						victim->page,
						victim);

		// Mark as reusable
		spin_lock(&cache->lock);
		list_add(&victim->lru_list, &cache->free_list);
		spin_unlock(&cache->lock);
	}
	return 0;
}

static remote_numa_cached_page_t *reuse_or_alloc_page(remote_numa_client_cache_t *cache)
{
	remote_numa_cached_page_t *entry = NULL;

	spin_lock(&cache->lock);
	if (!list_empty(&cache->free_list)) {
		entry = list_first_entry(&cache->free_list,
				 remote_numa_cached_page_t, lru_list);
		list_del(&entry->lru_list);
	}
	spin_unlock(&cache->lock);

	if (!entry) {
		struct page *page = alloc_page(GFP_ATOMIC);
		if (!page)
			return NULL;

		entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
		if (!entry) {
			__free_page(page);
			return NULL;
		}
		entry->page = page;
	}
	return entry;
}

static void cache_insert(remote_numa_client_cache_t *cache,
			 remote_numa_cached_page_t *entry)
{
	spin_lock(&cache->lock);
	list_add(&entry->lru_list, &cache->lru_head);
	cache->current_cached_pages++;
	spin_unlock(&cache->lock);
}

int remote_numa_client_cache_init(remote_numa_client_cache_t *cache,
				  remote_numa_main_trprt_if_t *trprt,
				  u32 max_cached_pages)
{
	INIT_LIST_HEAD(&cache->lru_head);
	INIT_LIST_HEAD(&cache->free_list);
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
	list_for_each_safe(pos, tmp, &cache->free_list) {
		entry = list_entry(pos, remote_numa_cached_page_t, lru_list);
		list_del(pos);
		__free_page(entry->page);
		kfree(entry);
	}
	cache->current_cached_pages = 0;
}

struct page *remote_numa_client_cache_alloc(remote_numa_client_cache_t *cache,
					    u32 donor_node_id)
{
	if (maybe_evict(cache))
		return NULL;

	remote_numa_cached_page_t *entry = reuse_or_alloc_page(cache);
	if (!entry)
		return NULL;

	entry->donor_node_id = donor_node_id;
	entry->main_pg_cookie = (u64)(uintptr_t)entry->page;

	if (remote_numa_transport_alloc_page(cache->trprt, donor_node_id,
					     entry->page, entry)) {
		spin_lock(&cache->lock);
		list_add(&entry->lru_list, &cache->free_list);
		spin_unlock(&cache->lock);
		return NULL;
	}

	cache_insert(cache, entry);
	return entry->page;
}

int remote_numa_client_cache_refault(remote_numa_client_cache_t *cache,
				     struct page *faulting_page)
{
	if (maybe_evict(cache))
		return -ENOMEM;

	u64 main_cookie = (u64)(uintptr_t)faulting_page;
	u32 donor_node_id = 0;
	u32 donor_pg_cookie = 0;

	remote_numa_cached_page_t *entry = reuse_or_alloc_page(cache);
	if (!entry)
		return -ENOMEM;

	entry->page = faulting_page;
	entry->main_pg_cookie = main_cookie;
	entry->donor_node_id = donor_node_id;
	entry->donor_pg_cookie = donor_pg_cookie;

	if (remote_numa_transport_refetch_page(cache->trprt, donor_node_id,
					       donor_pg_cookie, faulting_page,
					       entry)) {
		spin_lock(&cache->lock);
		list_add(&entry->lru_list, &cache->free_list);
		spin_unlock(&cache->lock);
		return -EIO;
	}

	cache_insert(cache, entry);
	return 0;
}

