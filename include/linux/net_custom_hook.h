// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * custom_net_hook.h - Hook for low-latency network frame rx.
 *
 * Copyright (C) 2024 Trevor Kemp
 */


#ifndef _CUSTOM_NET_HOOK_H
#define _CUSTOM_NET_HOOK_H
#include <linux/skbuff.h>

typedef enum custom_net_hook_ret
{
	custom_net_hook_consumed = 0,
	custom_net_hook_consumed_with_error,
	custom_net_hook_not_consumed,
} custom_net_hook_ret_t;

typedef custom_net_hook_ret_t (*custom_net_hook_fn_t)(struct sk_buff *skb);

/*
 * Called inside napi_gro_receive() to give us a low-latency path to
 * exmaine and act on network frames. By doing this here, we get the
 * lowest-latency path possible without having to modify netdev drivers.
 */
extern volatile custom_net_hook_fn_t custom_net_hook;

#endif
