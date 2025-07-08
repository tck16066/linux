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
#include "protocol.h"
#include "transport.h"

typedef struct {
	struct page *page;
	u64 donor_pg_cookie;
	struct list_head lru_list;
} remote_numa_cached_page_t;

typedef struct {
	struct list_head lru_head;
	struct list_head free_list;
	spinlock_t lock;
	wait_queue_head_t waitq;
	u32 max_cached_pages;
	u32 current_cached_pages;
	remote_numa_main_trprt_if_t *trprt;
} remote_numa_client_cache_t;

int remote_numa_client_cache_init(remote_numa_client_cache_t *cache,
				  remote_numa_main_trprt_if_t *trprt,
				  u32 max_cached_pages);

void remote_numa_client_cache_destroy(remote_numa_client_cache_t *cache);

struct page *remote_numa_client_cache_alloc(remote_numa_client_cache_t *cache);

/*
 * Refaults a previously evicted remote page.
 * Uses a (mm, addr) → donor_pg_cookie mapping to retrieve content from donor.
 */
int remote_numa_client_cache_refault(remote_numa_client_cache_t *cache,
				     struct page *faulting_page);

#endif

