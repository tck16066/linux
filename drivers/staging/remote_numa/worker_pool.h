// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * transport.h - Remote Numa transport interface.
 *
 * Copyright (C) 2025 Trevor Kemp
 */

#ifndef _REMOTE_NUMA_WORKER_POOL_H
#define _REMOTE_NUMA_WORKER_POOL_H

#include "transport.h"

typedef remote_numa_receive_ret_t (*rx_func_t)(void *);

int remote_numa_worker_pool_init(rx_func_t handler, int num_workers);
void remote_numa_worker_pool_stop(void);
remote_numa_receive_ret_t remote_numa_submit_frame(void *frame);
void remote_numa_worker_pool_set_reject(bool reject);

#endif

