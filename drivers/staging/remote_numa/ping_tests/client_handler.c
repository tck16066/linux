// kernel_custom_client.c

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/ktime.h>

#define MY_ETHERTYPE 0x88b5
#define DST_MAC "\x52\x54\x00\x00\x00\x01"	// Server MAC
#define SRC_MAC "\x52\x54\x00\x00\x00\x02"	// Client MAC
#define IFACE   "eth0"

static struct task_struct *ping_thread;
static struct net_device *dev;
static u64 last_tx_time;
static int waiting_reply;
static u64 rtt_ns;

static int
client_handler(struct sk_buff *skb, struct net_device *dev,
	       struct packet_type *pt, struct net_device *orig_dev)
{
	struct ethhdr *eth = eth_hdr(skb);
	if (memcmp(eth->h_source, DST_MAC, ETH_ALEN) == 0 &&
	    memcmp(eth->h_dest, SRC_MAC, ETH_ALEN) == 0 &&
	    skb->protocol == htons(MY_ETHERTYPE) && waiting_reply) {
		rtt_ns = ktime_get_ns() - last_tx_time;
		waiting_reply = 0;
		printk(KERN_INFO
		       "custom client: reply received, rtt = %llu ns (%llu us)\n",
		       rtt_ns, rtt_ns / 1000);
	}
	kfree_skb(skb);
	return NET_RX_SUCCESS;
}

static struct packet_type my_ptype __read_mostly = {
	.type = cpu_to_be16(MY_ETHERTYPE),
	.func = client_handler,
};

static int
ping_loop(void *data)
{
	struct sk_buff *skb;
	struct ethhdr *eth;
	int len = 60;		// Minimum Ethernet
	int i;

	allow_signal(SIGKILL);

	while (!kthread_should_stop()) {
		skb = alloc_skb(len + NET_IP_ALIGN, GFP_KERNEL);
		if (!skb)
			break;
		skb_reserve(skb, NET_IP_ALIGN);

		eth = (struct ethhdr *)skb_put(skb, sizeof(struct ethhdr));
		ether_addr_copy(eth->h_dest, DST_MAC);
		ether_addr_copy(eth->h_source, SRC_MAC);
		eth->h_proto = htons(MY_ETHERTYPE);

		memset(skb_put(skb, len - sizeof(struct ethhdr)), 0,
		       len - sizeof(struct ethhdr));

		skb->dev = dev;
		skb->protocol = htons(MY_ETHERTYPE);

		last_tx_time = ktime_get_ns();
		waiting_reply = 1;
		dev_queue_xmit(skb);

		// Wait up to 1 second for reply
		for (i = 0; i < 100; ++i) {
			if (!waiting_reply)
				break;
			msleep(10);
		}
		if (waiting_reply) {
			printk(KERN_INFO
			       "custom client: timeout waiting for reply\n");
			waiting_reply = 0;
		}
		ssleep(1);	// send next ping every 1s
	}
	return 0;
}

static int __init
ping_init(void)
{
	dev = dev_get_by_name(&init_net, IFACE);
	if (!dev)
		return -ENODEV;
	dev_add_pack(&my_ptype);
	ping_thread = kthread_run(ping_loop, NULL, "custom_ping_thread");
	printk(KERN_INFO "custom client: started\n");
	return 0;
}

static void __exit
ping_exit(void)
{
	if (ping_thread)
		kthread_stop(ping_thread);
	if (dev)
		dev_put(dev);
	dev_remove_pack(&my_ptype);
	printk(KERN_INFO "custom client: stopped\n");
}

module_init(ping_init);
module_exit(ping_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Custom Ethertype client with RTT timing");
