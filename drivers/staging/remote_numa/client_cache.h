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

struct remote_numa_main_trprt_if;

typedef struct remote_numa_cached_page {
	struct page *page;
	u64 donor_pg_cookie;
	u32 donor_id;
	struct list_head lru_list;
	struct mm_struct *mm;
	uintptr_t addr;
} remote_numa_cached_page_t;

typedef struct remote_numa_client_cache {
	struct list_head lru_head;
	struct list_head free_list;
	spinlock_t lock;
	wait_queue_head_t waitq;
	u32 max_cached_pages;
	u32 current_cached_pages;
	struct remote_numa_main_trprt_if *trprt;
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

#endif

