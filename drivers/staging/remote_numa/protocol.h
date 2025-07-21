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
	remote_numa_mem_alloc,
	remote_numa_mem_refetch,
	remote_numa_mem_sat_ack,
	remote_numa_mem_sync,
	remote_numa_mem_sync_ack,
	remote_numa_mem_free,
	remote_numa_mem_free_ack,
};

enum remote_numa_protocol_version
{
	remote_numa_protocol_0_1 = 1,
};

typedef struct {
	u16  version : 6;
	u16  type : 10;
	u16  reserved;
	u32  main_cookie;
	u32  donor_cookie;
} __attribute__((__packed__)) remote_numa_msg_hdr_t;

typedef struct
{
	u8 abstract_info[16];
} __attribute__((__packed__)) remote_numa_main_return_info_t;

/*
 * Used to advertise remote numa node of agnostic transport type.
 */
typedef struct
{
	remote_numa_msg_hdr_t hdr;
	remote_numa_main_return_info_t return_info;
	/* Refers to data only, not transport overhead. */
	u16                   max_frame_len;
} __attribute__((__packed__)) remote_numa_advert_t;

typedef struct
{
	remote_numa_msg_hdr_t hdr;
	remote_numa_main_return_info_t return_info;
} __attribute__((__packed__)) remote_numa_mem_query_t;

typedef struct
{
	remote_numa_msg_hdr_t hdr;
	u8                    page_size_rank;
	u32                   reserved : 24;
	u32                   free_pages;
} __attribute__((__packed__)) remote_numa_mem_resp_t;

typedef struct
{
	remote_numa_msg_hdr_t hdr;
	u64 main_pg_cookie;
	int hack;
} __attribute__((__packed__)) remote_numa_mem_alloc_t;

typedef enum
{
	remote_numa_xfer_end_of_pg = 2,
} remote_numa_mem_pg_xfer_flags_t;

typedef struct
{
	remote_numa_msg_hdr_t hdr;
	u8  flags;
	u8 reserved;
	u16 reserved2;
	u16 payload_len;
	u16 seq_num;
	u64 sender_pg_cookie;
	u64 receiver_pg_cookie;
	int hack;
} __attribute__((__packed__)) remote_numa_mem_pg_xfer_t;

typedef struct {
	remote_numa_msg_hdr_t hdr;
	u64 donor_pg_cookie;
} __attribute__((__packed__)) remote_numa_mem_refetch_t;

typedef struct {
	remote_numa_msg_hdr_t hdr;
	u64 main_pg_cookie;
	u64 donor_pg_cookie;
	int hack;
} __attribute__((__packed__)) remote_numa_mem_satisfaction_t;

typedef struct
{
	remote_numa_msg_hdr_t hdr;
	u16 max_seq_num;
	u16 reserved;
	u64 sender_pg_cookie;
	u64 receiver_pg_cookie;
	int hack;
} __attribute__((__packed__)) remote_numa_mem_pg_xfer_ack_t;

typedef struct
{
	remote_numa_msg_hdr_t hdr;
	u32 donor_pg_cookie;
	u32 main_pg_cookie;
} __attribute__((__packed__)) remote_numa_mem_free_t;

typedef struct
{
	remote_numa_msg_hdr_t hdr;
	u32 main_pg_cookie;
} __attribute__((__packed__)) remote_numa_mem_free_ack_t;
#endif
