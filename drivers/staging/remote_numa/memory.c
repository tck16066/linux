// SPDX-License-Identifier: GPL-2.0-or-later
/*
* memory.c - Remote Numa memory management.
*
* Copyright (C) 2025 Trevor Kemp
*/

#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "memory.h"

remote_numa_mem_mgr_t *remote_numa_create_mem_mgr(size_t len)
{
	remote_numa_mem_mgr_t *mgr = kmalloc(sizeof(*mgr), GFP_KERNEL);
	if (!mgr)
		return NULL;

	mgr->mem.buffer_len = len;
	mgr->mem.large_buffer = vmalloc(len);

	if (!mgr->mem.large_buffer)
	{
		kfree(mgr);
		return NULL;
	}

	return mgr;
}

void remote_numa_clean_mem_mgr(remote_numa_mem_mgr_t *mgr)
{
	if (!mgr)
		return;

	vfree(mgr->mem.large_buffer);
	kfree(mgr);
}

