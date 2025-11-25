// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * transport.h - Remote Numa transport interface.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#ifndef _REMOTE_NUMA_TRANSPORT_H
#define _REMOTE_NUMA_TRANSPORT_H

#include <linux/hashtable.h>
#include <linux/mm_types.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "client_cache.h"
#include "protocol.h"

/* Create a return val for base + transport */
#define REMOTE_NUMA_TRPRT_RET(code, trprt_code) ((u32)((trprt_code << 8) | (code & 0xFF)))

/* Decode return val for send */
#define REMOTE_NUMA_TRPRT_TX_RET_BASE(code) ((remote_numa_send_ret_t)(code & 0xFF))

/* Decode return val for receive */
#define REMOTE_NUMA_TRPRT_RX_RET_BASE(code) ((remote_numa_receive_ret_t)(code & 0xFF))

/* Decode return val */
#define REMOTE_NUMA_TRPT_RET_TRPRT_CODE(code) ((code & ~0xFF) >> 8)

#define REMOTE_NUMA_HASH_TABLE_ORDER 16

struct remote_numa_cached_page;
struct remote_numa_client_cache;
struct remote_numa_mem_mgr;

/*
 * Return status for sending messages. Transports encode their specific
 * errors in the upper 3 bytes.
 */
typedef enum
{
	remote_numa_send_success = 0,
	remote_numa_send_err_unknown = 1,
	remote_numa_send_bad_alloc = 2,
	remote_numa_send_no_if = 3,
	remote_numa_send_bad_xmit = 4,
	remote_numa_send_timeout = 5,
	/*After 255, they are transport-encoded.*/
	remote_numa_send_max = 255,
} remote_numa_send_ret_t;

/*
 * Return status for receiving messages. Transports encode their specific
 * errors in the upper 3 bytes.
 */
typedef enum
{
	remote_numa_receive_success = 0,
	remote_numa_receive_err_unknown = 1,
	remote_numa_receive_bad_alloc = 2,
	remote_numa_receive_bad_proto = 3,
	remote_numa_receive_mangled_pkt = 4,
	remote_numa_receive_bad_cookie = 5,
	remote_numa_receive_out_of_bounds = 6,
	/*After 255, they are transport-encoded.*/
	remote_numa_receive_max = 255,
} remote_numa_receive_ret_t;

typedef struct
{
	u32			     node_id;
	struct hlist_node            hnode;
	void *priv_return_info;
	u32 donor_cookie;
	u32 free_pages;
	u8 page_size_rank;
	bool valid_mem_resp;
	spinlock_t node_lock;
} remote_numa_node_t;

typedef struct
{
	struct remote_numa_mem_mgr *mem;
	DECLARE_HASHTABLE(node_table, REMOTE_NUMA_HASH_TABLE_ORDER);
	void *trprt_ctx;
	spinlock_t hash_write_lock;
} remote_numa_trprt_ctx_t;

typedef struct remote_numa_donor_trprt_if
{
	u32 (*remote_numa_node_id)(remote_numa_mem_query_t *);
	u16 (*get_max_payload_len)(void); // Does not count header.
	void (*free_rx_buff)(void *);
	void (*prepare_rx_buff)(void *rx_buff, void **payload);
	void (*alloc_tx_buffer)(size_t payload_len,
		void **obj, void**payload_start);
	void* (*priv_return_info_from_mem_query)(remote_numa_mem_query_t *);
	remote_numa_send_ret_t (*tx_msg)(remote_numa_trprt_ctx_t *trprt_ctx,
		void *return_info, void *msg);
	remote_numa_send_ret_t (*tx_advert)(remote_numa_trprt_ctx_t *trprt_ctx);
	remote_numa_receive_ret_t (*rx_mem_query)(remote_numa_trprt_ctx_t *trprt_ctx, remote_numa_mem_query_t *query);
	remote_numa_send_ret_t (*tx_mem_resp)(remote_numa_trprt_ctx_t *trprt_ctx, remote_numa_mem_resp_t *resp);

	/* Some callbacks need transport-specific data. */
	remote_numa_trprt_ctx_t *trprt_ctx;
} remote_numa_donor_trprt_if_t;

typedef struct remote_numa_main_trprt_if
{
	u32 (*remote_numa_node_id)(remote_numa_advert_t *);
	u16 (*get_max_payload_len)(void); // Does not count header.
	remote_numa_send_ret_t (*priv_return_info)(
		void *trprt_ctx, remote_numa_main_return_info_t *info);
	void* (*priv_return_info_from_advert)(remote_numa_advert_t *);
	void (*free_priv_return_info)(void*);
	void (*free_rx_buff)(void *);
	void (*prepare_rx_buff)(void *rx_buff, void **payload);
	void (*alloc_tx_buffer)(size_t payload_len,
		void **obj, void**payload_start);
	remote_numa_send_ret_t (*tx_msg)(remote_numa_trprt_ctx_t *trprt_ctx,
		void *return_info, void *msg);
	remote_numa_receive_ret_t (*rx_mem_resp)(remote_numa_trprt_ctx_t *trprt_ctx, remote_numa_mem_resp_t *resp);

	/* Some callbacks need transport-specific data. */
	remote_numa_trprt_ctx_t *trprt_ctx;
	struct remote_numa_client_cache *client_cache;
} remote_numa_main_trprt_if_t;

remote_numa_trprt_ctx_t *remote_numa_make_trprt_ctx(struct remote_numa_mem_mgr *mem);

void remote_numa_transport_ctx_destroy(remote_numa_trprt_ctx_t *ctx);

remote_numa_receive_ret_t remote_numa_main_rx(
	remote_numa_main_trprt_if_t *main_if, void *rx_data, void *payload);

remote_numa_receive_ret_t remote_numa_donor_rx(
	remote_numa_donor_trprt_if_t *donor_if, void *rx_data, void *payload);

remote_numa_receive_ret_t remote_numa_rx_advert(
	remote_numa_main_trprt_if_t *main_if,
	remote_numa_advert_t *advert);

remote_numa_receive_ret_t remote_numa_rx_mem_query(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_query_t *query);

remote_numa_receive_ret_t remote_numa_rx_mem_resp(
	remote_numa_main_trprt_if_t *main_if,
	remote_numa_mem_resp_t *resp);

remote_numa_receive_ret_t remote_numa_rx_mem_alloc(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_alloc_t *alloc);

remote_numa_send_ret_t remote_numa_transport_alloc_page_rcu(
	remote_numa_main_trprt_if_t *trprt,
	remote_numa_node_t *donor,
	struct remote_numa_cached_page *cached_target);

remote_numa_send_ret_t remote_numa_transport_alloc_page_rcu(
	remote_numa_main_trprt_if_t *trprt,
	remote_numa_node_t *donor,
	struct remote_numa_cached_page *cached_target);

remote_numa_send_ret_t remote_numa_transport_refetch_page(
	remote_numa_main_trprt_if_t *trprt,
	u32 donor_node_id,
	u64 donor_pg_cookie,
	struct remote_numa_cached_page *cached_target);

/* Non-blocking variants - return immediately, use _check_transfer_complete to poll */
remote_numa_send_ret_t remote_numa_transport_alloc_page_async(
	remote_numa_main_trprt_if_t *trprt,
	remote_numa_node_t *donor,
	struct remote_numa_cached_page *cached_target);

remote_numa_send_ret_t remote_numa_transport_refetch_page_async(
	remote_numa_main_trprt_if_t *trprt,
	u32 donor_node_id,
	u64 donor_pg_cookie,
	struct remote_numa_cached_page *cached_target);

remote_numa_send_ret_t remote_numa_tx_mem_pg_sync_xfer_async(
	remote_numa_main_trprt_if_t *main_if,
	u64 donor_pg_cookie,
	struct page *pg,
	struct remote_numa_cached_page *victim);

/* Check if async transfer is complete. Returns 0 if complete, -EAGAIN if still in progress, <0 on error */
int remote_numa_check_transfer_complete(struct remote_numa_cached_page *cached_pg);

remote_numa_send_ret_t remote_numa_transport_refetch_page_async(
	remote_numa_main_trprt_if_t *trprt,
	u32 donor_node_id,
	u64 donor_pg_cookie,
	struct remote_numa_cached_page *cached_target);

bool remote_numa_transport_is_transfer_complete(
	struct remote_numa_cached_page *cached_target);

remote_numa_send_ret_t remote_numa_tx_mem_pg_sync_xfer_async(
	remote_numa_main_trprt_if_t *main_if,
	u64 donor_pg_cookie,
	struct page *pg,
	struct remote_numa_cached_page *victim);

remote_numa_receive_ret_t remote_numa_rx_mem_pg_sat_ack(
	remote_numa_main_trprt_if_t *main_if,
	remote_numa_mem_satisfaction_t *ack);

remote_numa_send_ret_t remote_numa_tx_mem_pg_sync_xfer(
	remote_numa_main_trprt_if_t *main_if,
	u64 donor_pg_cookie,
	struct page * pg,
	struct remote_numa_cached_page *victim);

remote_numa_receive_ret_t remote_numa_rx_mem_pg_sync_xfer(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_pg_xfer_t *xfer);

remote_numa_receive_ret_t remote_numa_rx_mem_pg_sync_ack(
	remote_numa_main_trprt_if_t *main_if,
	remote_numa_mem_pg_xfer_ack_t *xfer);

remote_numa_receive_ret_t remote_numa_rx_mem_pg_free(
	remote_numa_donor_trprt_if_t *donor_if,
	remote_numa_mem_free_t *pg_free);

remote_numa_receive_ret_t remote_numa_rx_mem_pg_free_ack(
	remote_numa_main_trprt_if_t *main_if,
	remote_numa_mem_free_ack_t *ack);

remote_numa_send_ret_t remote_numa_tx_mem_pg_free(
	struct remote_numa_main_trprt_if *main_if,
	u32 donor_id,
	u64 donor_pg_cookie,
	uintptr_t main_pg_cookie);

remote_numa_receive_ret_t remote_numa_rx_mem_pg_refetch_sat(
    remote_numa_main_trprt_if_t *main_if,
    remote_numa_mem_pg_xfer_t *sat);

remote_numa_receive_ret_t
remote_numa_rx_mem_pg_refetch_ack(struct remote_numa_donor_trprt_if *donor_if,
                                  remote_numa_mem_pg_xfer_ack_t *ack);

void tmp_init(void);

#endif

