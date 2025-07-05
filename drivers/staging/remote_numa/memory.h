// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * memory.h - Remote Numa memory manager.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#ifndef _REMOTE_NUMA_MEMORY_H
#define _REMOTE_NUMA_MEMORY_H

typedef struct
{
	void *priv;
	size_t num_pages;
	size_t free_pages;
	size_t page_size_rank;
	size_t num_pools;
} remote_numa_mem_mgr_t;

remote_numa_mem_mgr_t *remote_numa_create_mem_mgr(size_t num_pages);

void remote_numa_clean_mem_mgr(remote_numa_mem_mgr_t *mgr);

#endif

