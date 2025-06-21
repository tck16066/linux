#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/timekeeping.h>
#include <linux/if_ether.h>
#include <linux/string.h>
#include <linux/net_custom_hook.h>	// your patch's header!

#define ETH_P_CUSTOM 0x88B5
#define PAYLOAD "KERNELPING"
#define INTERVAL_MS 500

static char ifname[] = "eth0";
module_param_string(ifname, ifname, sizeof(ifname), 0444);

static u8 server_mac[ETH_ALEN] = { 0x52, 0x54, 0x00, 0x00, 0x00, 0x01 };
static u8 client_mac[ETH_ALEN] = { 0x52, 0x54, 0x00, 0x00, 0x00, 0x02 };

static struct net_device *dev;
static struct task_struct *kthread;
static int seq = 0;
static ktime_t last_send_time;
static bool handler_registered = false;

static custom_net_hook_ret_t
my_custom_handler(struct sk_buff *skb)
{
	struct ethhdr *eth;

	if (skb->protocol != htons(ETH_P_CUSTOM)) {
		return custom_net_hook_not_consumed;	// Not our proto
	}

	eth = eth_hdr(skb);
	// Is this an echo from the server?
	if (ether_addr_equal(eth->h_dest, client_mac) &&
	    ether_addr_equal(eth->h_source, server_mac) &&
	    skb->len >= sizeof(struct ethhdr) + sizeof(PAYLOAD)) {

		ktime_t now = ktime_get();
		u64 rtt_us = ktime_to_ns(ktime_sub(now, last_send_time)) / 1000;
		printk(KERN_INFO
		       "raw_eth_client: got echo reply! RTT: %llu us (seq %d)\n",
		       rtt_us, seq);
		kfree_skb(skb);
		return custom_net_hook_consumed;	// handled
	} else {
		printk(KERN_INFO "NOT A REPLY!");
	}

	printk(KERN_INFO "Error. Dropping frame.");
	kfree_skb(skb);
	return custom_net_hook_consumed_with_error;
}

static int
client_thread(void *data)
{
	while (!kthread_should_stop()) {
		struct sk_buff *skb;
		struct ethhdr *eth;
		size_t pkt_size = sizeof(struct ethhdr) + sizeof(PAYLOAD);
		int err;

		skb = alloc_skb(pkt_size + NET_IP_ALIGN, GFP_KERNEL);
		if (!skb) {
			msleep(INTERVAL_MS);
			continue;
		}

		skb_reserve(skb, NET_IP_ALIGN);
		skb_reset_network_header(skb);

		eth = (struct ethhdr *)skb_put(skb, sizeof(struct ethhdr));
		ether_addr_copy(eth->h_dest, server_mac);
		ether_addr_copy(eth->h_source, client_mac);
		eth->h_proto = htons(ETH_P_CUSTOM);

		memcpy(skb_put(skb, sizeof(PAYLOAD)), PAYLOAD, sizeof(PAYLOAD));

		skb->dev = dev;
		skb->protocol = eth->h_proto;
		skb->priority = 0;

		last_send_time = ktime_get();
		++seq;

		err = dev_queue_xmit(skb);
		if (err)
			printk(KERN_ERR
			       "raw_eth_client: dev_queue_xmit failed: %d\n",
			       err);

		msleep(INTERVAL_MS);
	}
	return 0;
}

static int __init
raw_eth_client_init(void)
{
	dev = dev_get_by_name(&init_net, ifname);
	if (!dev) {
		printk(KERN_ERR "raw_eth_client: cannot find interface %s\n",
		       ifname);
		return -ENODEV;
	}

	// Register handler
	if (custom_net_hook)
		printk(KERN_WARNING
		       "raw_eth_client: handler already registered!\n");
	custom_net_hook = my_custom_handler;
	handler_registered = true;

	kthread = kthread_run(client_thread, NULL, "raw_eth_client");
	if (IS_ERR(kthread)) {
		printk(KERN_ERR "raw_eth_client: kthread_run failed\n");
		custom_net_hook = NULL;
		handler_registered = false;
		dev_put(dev);
		return PTR_ERR(kthread);
	}

	printk(KERN_INFO "raw_eth_client: loaded, pinging %pM from %pM on %s\n",
	       server_mac, client_mac, ifname);
	return 0;
}

static void __exit
raw_eth_client_exit(void)
{
	if (kthread)
		kthread_stop(kthread);
	if (handler_registered)
		custom_net_hook = NULL;
	if (dev)
		dev_put(dev);
	printk(KERN_INFO "raw_eth_client: unloaded\n");
}

module_init(raw_eth_client_init);
module_exit(raw_eth_client_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Raw Ethernet kernel ping client (for early RX hook)");
