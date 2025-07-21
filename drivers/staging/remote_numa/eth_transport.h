// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * eth_transport.h - Remote Numa transport functions for ethernet.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#ifndef _REMOTE_NUMA_ETH_TRANSPORT_H
#define _REMOTE_NUMA_ETH_TRANSPORT_H

#include <linux/skbuff.h>

#include "memory.h"
#include "protocol.h"
#include "transport.h"

//#define REMOTE_NUMA_ETHERTYPE htons(0x88B5)
#define REMOTE_NUMA_ETHERTYPE htons(0xA1A1)

typedef custom_net_hook_ret_t (*skb_handler_t)(struct sk_buff *);

remote_numa_donor_trprt_if_t *remote_numa_eth_donor_init(remote_numa_mem_mgr_t *mem);
void remote_numa_clean_donor_trprt_if(remote_numa_donor_trprt_if_t *iface);

remote_numa_main_trprt_if_t *remote_numa_eth_main_init(void );
void remote_numa_clean_main_trprt_if(remote_numa_main_trprt_if_t *iface);

#endif
