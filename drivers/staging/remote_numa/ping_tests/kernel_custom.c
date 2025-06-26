#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/net_custom_hook.h>
#include <linux/rcupdate.h>

#define MY_PROTO htons(0x88B5)

#define ETH_MIN_FRAME 60

#define ETH_MIN_FRAME 60

static custom_net_hook_ret_t
my_custom_handler(struct sk_buff *skb)
{
	struct ethhdr *eth = eth_hdr(skb);
	struct net_device *dev = skb->dev;
	int pay_len = skb->len - sizeof(struct ethhdr);
	int tx_len = sizeof(struct ethhdr) + (pay_len > 0 ? pay_len : 0);
	int pad_len = (tx_len < ETH_MIN_FRAME) ? (ETH_MIN_FRAME - tx_len) : 0;
	struct sk_buff *txskb;
	struct ethhdr *eth_tx;
	u8 orig_src[ETH_ALEN];

	if (skb->protocol != htons(0x88b5))
		return custom_net_hook_not_consumed;
	if (skb->len < sizeof(struct ethhdr))
		goto err;

	ether_addr_copy(orig_src, eth->h_source);

	txskb = alloc_skb(ETH_MIN_FRAME + NET_IP_ALIGN, GFP_ATOMIC);
	if (!txskb) {
		goto err;
	}
	skb_reserve(txskb, NET_IP_ALIGN);

	// Ethernet header first
	eth_tx = (struct ethhdr *)skb_put(txskb, sizeof(struct ethhdr));
	ether_addr_copy(eth_tx->h_dest, orig_src);
	ether_addr_copy(eth_tx->h_source, dev->dev_addr);
	eth_tx->h_proto = htons(0x88b5);

	// Copy payload
	if (pay_len > 0)
		skb_put_data(txskb, skb->data + sizeof(struct ethhdr), pay_len);

	// Pad to minimum length
	if (pad_len > 0)
		memset(skb_put(txskb, pad_len), 0, pad_len);

	txskb->dev = dev;
	txskb->protocol = htons(0x88b5);
	txskb->pkt_type = PACKET_OUTGOING;
	skb_reset_network_header(txskb);

	if (0 == dev_queue_xmit(txskb))
		return custom_net_hook_consumed;

err:
	kfree_skb(skb);
	return custom_net_hook_consumed_with_error;
}

static int __init
custom_eth_init(void)
{
	printk(KERN_INFO "Registering custom low-latency ethertype handler\n");
	rcu_assign_pointer(custom_net_hook, my_custom_handler);
	return 0;
}

static void __exit
custom_eth_exit(void)
{
	rcu_assign_pointer(custom_net_hook, my_custom_handler);
	printk(KERN_INFO "Unregistered custom handler\n");
}

module_init(custom_eth_init);
module_exit(custom_eth_exit);
MODULE_LICENSE("GPL");
