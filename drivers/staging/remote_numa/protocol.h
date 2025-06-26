// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * protocol.h - Protocol for remote numa caching.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#ifndef _REMOTE_NUMA_PROTOCOL_H
#define _REMOTE_NUMA_PROTOCOL_H

#include <linux/if_ether.h>
#include <linux/types.h>

enum remote_numa_msg_type
{
	remote_numa_eth_advert = 1,
	/* End advertisement types  */

	remote_numa_mem_query = 20,
	remote_numa_mem_resp,
};

enum remote_numa_protocol_version
{
	remote_numa_protocol_0_1 = 1,
};

typedef struct {
	u8   version : 6;
	u16  type : 10;
	u16  reserved;
	u32  sender_cookie;
} __attribute__((__packed__)) remote_numa_msg_hdr_t;

/*
 * Used to advertise remote numa node of agnostic transport type.
 */
typedef struct
{
	remote_numa_msg_hdr_t hdr;
	/* Refers to data only, not transport overhead. */
	u16                   max_frame_len;
} __attribute__((__packed__)) remote_numa_advert_t;

typedef struct
{
	remote_numa_msg_hdr_t hdr;
	
} __attribute__((__packed__)) remote_numa_mem_query_t;

typedef struct
{
	remote_numa_msg_hdr_t hdr;
	u8                    total_pages_rank;
	u8                    page_size_rank;
	u16                   reserved;
	u64                   free_pages;
} __attribute__((__packed__)) remote_numa_mem_resp_t;

#endif
