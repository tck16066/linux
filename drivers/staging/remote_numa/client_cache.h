// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * client.h - Remote Numa client APIs.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#ifndef _REMOTE_NUMA_CLIENT_CACHE_H
#define _REMOTE_NUMA_CLIENT_CACHE_H

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include "protocol.h"
#include "transport.h"

#define REMOTE_NUMA_CLIENT_CACHE_HASH_BITS 13

struct remote_numa_main_trprt_if;

typedef struct remote_numa_known_page {
	struct page *page;
	struct mm_struct *mm; /* now belongs here */
	unsigned long addr;
	u64 donor_pg_cookie;
	u32 donor_id;
	u32 donor_cookie;
	struct hlist_node node; /* for hash table linkage */
} remote_numa_known_page_t;

typedef struct remote_numa_cached_page {
	struct remote_numa_known_page *known_page;
	struct list_head lru_list;
	uintptr_t main_pg_cookie;
	struct hlist_node node;
	atomic_t transfer_in_progress; /* non-zero if alloc/refault/evict in flight */
} remote_numa_cached_page_t;

typedef struct remote_numa_client_cache {
	struct list_head lru_head;
	struct list_head free_list;
	struct list_head evicting_list; /* Pages being evicted asynchronously */
	spinlock_t lock;
	wait_queue_head_t waitq;
	u32 max_cached_pages;
	u32 current_cached_pages;
	struct remote_numa_main_trprt_if *trprt;
	DECLARE_HASHTABLE(page_lookup, REMOTE_NUMA_CLIENT_CACHE_HASH_BITS);
	DECLARE_HASHTABLE(known_pages, REMOTE_NUMA_CLIENT_CACHE_HASH_BITS);
	struct delayed_work eviction_completion_work;
} remote_numa_client_cache_t;

int remote_numa_client_cache_init(remote_numa_client_cache_t *cache,
				  struct remote_numa_main_trprt_if *trprt,
				  u32 max_cached_pages);

void remote_numa_client_cache_destroy(remote_numa_client_cache_t *cache);

struct page *remote_numa_client_cache_alloc(remote_numa_client_cache_t *cache,
	struct vm_fault *vmf);

/*
 * Refaults a previously evicted remote page.
 * Uses a (mm, addr) → donor_pg_cookie mapping to retrieve content from donor.
 */
int remote_numa_client_cache_refault(remote_numa_client_cache_t *cache,
				     struct page *faulting_page,
				     struct vm_fault *vmf);


int remote_numa_client_cache_free_page(remote_numa_client_cache_t *cache,
				       struct page *page);

/*
 * Check if an async allocation or refault has completed.
 * Returns 0 if ready, -EAGAIN if still in progress, -ENOENT if not found.
 */
int remote_numa_client_cache_check_ready(remote_numa_client_cache_t *cache,
					  struct page *page);

#endif

