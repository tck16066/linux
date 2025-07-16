
// SPDX-License-Identifier: GPL-2.0
/*
 * remote_numa_test.c - Test driver for remote NUMA page lifecycle
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/slab.h>

// These headers must match your real interfaces
#include "client_cache.h"
#include "transport.h"

extern remote_numa_client_cache_t *client_cache;
remote_numa_client_cache_t *global_remote_cache = NULL;

static void test_remote_numa_page_flow(remote_numa_client_cache_t *cache)
{
	struct page *pg;
	u8 *va;
	int ret;

	printk(KERN_INFO "=== TEST: Remote NUMA page lifecycle ===\n");

	/* Step 1: Allocate */
	printk(KERN_INFO "[ALLOC] Requesting remote page...\n");

	struct vm_area_struct fake_vma = {
		.vm_mm = current->mm,
	};
	struct vm_fault fake_vmf = {
	    .vma = &fake_vma,
	    .address = 0xdeadbeef,  // Arbitrary test value
	};
	
	pg = remote_numa_client_cache_alloc(cache, &fake_vmf);

	if (!pg) {
		printk(KERN_ERR "[ALLOC] Failed to allocate remote page\n");
		return;
	}
	va = page_address(pg);
	memset(va, 0xAA, PAGE_SIZE);
	printk(KERN_INFO "[ALLOC] Page allocated and filled with pattern 0xAA\n");

	/* Step 2: Refault */
	printk(KERN_INFO "[REFAULT] Simulating eviction and refault...\n");
	ret = remote_numa_client_cache_refault(cache, pg, NULL);  // use real vmf if available
	if (ret) {
		printk(KERN_ERR "[REFAULT] Refetch failed: ret = %d\n", ret);
		return;
	}

	va = page_address(pg);
	if (va[0] != 0xAA) {
		printk(KERN_ERR "[REFAULT] Data mismatch after refault: expected 0xAA, got 0x%02x\n", va[0]);
	} else {
		printk(KERN_INFO "[REFAULT] Data verified after refetch: 0x%02x\n", va[0]);
	}

	/* Step 3: Free */
	printk(KERN_INFO "[FREE] Freeing page...\n");
	ret = remote_numa_client_cache_free_page(cache, pg);
	if (ret) {
		printk(KERN_ERR "[FREE] Failed to free page: ret = %d\n", ret);
	} else {
		printk(KERN_INFO "[FREE] Page successfully freed\n");
	}

	printk(KERN_INFO "=== TEST COMPLETE ===\n");
}


static int __init remote_numa_test_init(void)
{
    printk(KERN_INFO "[TEST MODULE] Loading remote NUMA test driver...\n");

    global_remote_cache = client_cache;

    if (!global_remote_cache) {
        printk(KERN_ERR "[TEST MODULE] global_remote_cache not initialized\n");
        return -EINVAL;
    }

    test_remote_numa_page_flow(global_remote_cache);
    return 0;
}

static void __exit remote_numa_test_exit(void)
{
    printk(KERN_INFO "[TEST MODULE] Remote NUMA test module unloaded\n");
}

module_init(remote_numa_test_init);
module_exit(remote_numa_test_exit);

MODULE_AUTHOR("Trevor C Kemp");
MODULE_DESCRIPTION("Remote NUMA page lifecycle test module");
MODULE_LICENSE("GPL");
