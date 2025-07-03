// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * memory.h - Remote Numa memory manager.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#ifndef _REMOTE_NUMA_MEMORY_H
#define _REMOTE_NUMA_MEMORY_H

#include "memory.h"

typedef struct
{
	void *large_buffer;
	size_t buffer_len;
} remote_numa_mem_buff_t;

typedef struct
{
	remote_numa_mem_buff_t mem;
} remote_numa_mem_mgr_t;

remote_numa_mem_mgr_t *remote_numa_create_mem_mgr(size_t len);

void remote_numa_clean_mem_mgr(remote_numa_mem_mgr_t *mgr);

#endif

