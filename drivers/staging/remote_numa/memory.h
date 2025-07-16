// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * memory.h - Remote Numa memory manager.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#ifndef _REMOTE_NUMA_MEMORY_H
#define _REMOTE_NUMA_MEMORY_H

#include <linux/types.h>
#include <linux/list.h>

typedef struct {
	void *addr;
	u64 donor_pg_cookie;
	bool in_use;
	struct list_head list;
	struct hlist_node hnode;
} remote_numa_page_t;

typedef struct {
	void *priv; // Internal pool array
	size_t num_pages;
	size_t free_pages;
	size_t page_size_rank;
	size_t num_pools;

	// Free list of remote_numa_page_t*
	struct list_head free_list;
	spinlock_t lock;

	// Cookie -> page mapping
	struct hlist_head *cookie_table;
	u32 cookie_hash_bits;

	atomic64_t cookie_gen;
} remote_numa_mem_mgr_t;

remote_numa_mem_mgr_t *remote_numa_create_mem_mgr(size_t num_pages);
void remote_numa_clean_mem_mgr(remote_numa_mem_mgr_t *mgr);

int remote_numa_mem_alloc_page(remote_numa_mem_mgr_t *mgr,
                               u64 *cookie_out,
                               void **page_out);

int remote_numa_mem_lookup_page(remote_numa_mem_mgr_t *mgr,
                                u64 cookie,
                                void **page_out);

int remote_numa_mem_sync_to_donor(remote_numa_mem_mgr_t *mgr,
                                  u64 cookie,
                                  void *src_buf,
                                  size_t len,
                                  size_t offset);

int remote_numa_mem_sync_from_donor(remote_numa_mem_mgr_t *mgr,
                                    u64 cookie,
                                    void **src_buf_out,
                                    size_t *len_out);

int remote_numa_mem_free_page(remote_numa_mem_mgr_t *mgr,
                              u64 cookie);

#endif

