#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include <linux/net_custom_hook.h>

static int __init
show_hook_init(void)
{
	printk(KERN_INFO "custom_net_hook pointer: %px\n", custom_net_hook);
	return -EIO;		// fail so it auto-unloads (or return 0 to keep loaded)
}

static void __exit
show_hook_exit(void)
{
}

module_init(show_hook_init);
module_exit(show_hook_exit);
MODULE_LICENSE("GPL");
