// kernel_custom_server.c

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>

#define MY_ETHERTYPE 0x88b5

static int
my_proto_handler(struct sk_buff *skb, struct net_device *dev,
		 struct packet_type *pt, struct net_device *orig_dev)
{
	struct ethhdr *rx_eth = eth_hdr(skb);
	struct sk_buff *txskb;
	struct ethhdr *tx_eth;
	int payload_len = skb->len - sizeof(struct ethhdr);
	int min_payload = 46;
	int total_len =
	    sizeof(struct ethhdr) + max_t(int, payload_len, min_payload);

	// Sanity check incoming frame
	if (skb->len < sizeof(struct ethhdr))
		goto drop;

	// Allocate new skb for reply
	txskb = alloc_skb(total_len + NET_IP_ALIGN, GFP_ATOMIC);
	if (!txskb)
		goto drop;
	skb_reserve(txskb, NET_IP_ALIGN);

	// Set Ethernet header
	tx_eth = (struct ethhdr *)skb_put(txskb, sizeof(struct ethhdr));
	ether_addr_copy(tx_eth->h_dest, rx_eth->h_source);	// Reply to sender
	ether_addr_copy(tx_eth->h_source, dev->dev_addr);	// From us
	tx_eth->h_proto = htons(MY_ETHERTYPE);

	// Copy payload (if present)
	if (payload_len > 0)
		skb_put_data(txskb, skb->data + sizeof(struct ethhdr),
			     payload_len);

	// Pad as necessary
	if (payload_len < min_payload)
		memset(skb_put(txskb, min_payload - payload_len), 0,
		       min_payload - payload_len);

	txskb->dev = dev;
	txskb->protocol = htons(MY_ETHERTYPE);

	// Required for most devices:
	skb_reset_mac_header(txskb);
	skb->dev = dev;

	dev_queue_xmit(txskb);

      drop:
	kfree_skb(skb);
	return NET_RX_SUCCESS;
}

static struct packet_type my_ptype __read_mostly = {
	.type = cpu_to_be16(MY_ETHERTYPE),
	.func = my_proto_handler,
};

static int __init
my_proto_init(void)
{
	dev_add_pack(&my_ptype);
	printk(KERN_INFO "custom server: registered protocol handler\n");
	return 0;
}

static void __exit
my_proto_exit(void)
{
	dev_remove_pack(&my_ptype);
	printk(KERN_INFO "custom server: unregistered protocol handler\n");
}

module_init(my_proto_init);
module_exit(my_proto_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Custom Ethertype server (echo)");
