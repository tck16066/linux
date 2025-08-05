// SPDX-License-Identifier: GPL-2.0
/*
 * remote_numa_test.c - Test driver for remote NUMA page lifecycle
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/pid.h>
#include <linux/mm_types.h>
#include <linux/sched/mm.h> // for use_mm()

#include "client_cache.h"
#include "transport.h"

extern remote_numa_client_cache_t *client_cache;
remote_numa_client_cache_t *global_remote_cache = NULL;

#define TEST_CACHE_PAGES 2
#define TOTAL_TEST_PAGES 10
#define REMOTE_NUMA_RUN_TEST _IOW('R', 1, unsigned long)

static dev_t devno;
static struct class *test_class;
static struct cdev test_cdev;
static struct proc_dir_entry *test_entry;

static int test_page_lifecycle(remote_numa_client_cache_t *cache, unsigned long addr)
{
    struct vm_fault fake_vmf;
    struct vm_area_struct *vma;
    struct page *pages[TOTAL_TEST_PAGES];
    int ret;

    if (!current->mm) {
        printk(KERN_ERR "[TEST] current->mm is NULL\n");
        return -EINVAL;
    }

    down_read(&current->mm->mmap_lock);
    vma = find_vma(current->mm, addr);
    up_read(&current->mm->mmap_lock);
    if (!vma) {
        printk(KERN_ERR "[TEST] No VMA found for address %px\n", (void *)addr);
        return -EINVAL;
    }

    printk(KERN_INFO "=== TEST: Remote NUMA page lifecycle with eviction ===\n");

    struct vm_area_struct fake_vma = *vma;
    memset(&fake_vmf, 0, sizeof(fake_vmf));
    *((struct vm_area_struct **)&fake_vmf.vma) = &fake_vma;

    // Allocate and fill each page with a unique pattern
    for (int i = 0; i < TOTAL_TEST_PAGES; i++) {
        *((unsigned long *)&fake_vmf.address) = addr + (i * PAGE_SIZE);

        pages[i] = remote_numa_client_cache_alloc(cache, &fake_vmf);
        if (!pages[i]) {
            printk(KERN_ERR "[TEST] Allocation %d failed\n", i);
            return -ENOMEM;
        }
        printk(KERN_INFO "[TEST] Allocation %d succeeded\n", i);

        void *kaddr = kmap_local_page(pages[i]);
        memset(kaddr, i, PAGE_SIZE);
        kunmap_local(kaddr);
    }

    // Refault and verify each page
    for (int i = 0; i < TOTAL_TEST_PAGES - 1; i++) {
        *((unsigned long *)&fake_vmf.address) = addr + (i * PAGE_SIZE);

        ret = remote_numa_client_cache_refault(cache, pages[i], &fake_vmf);
        if (ret) {
            printk(KERN_ERR "[TEST] Refault %d failed with error %d\n", i, ret);
            continue;
        }

        struct page *pg = pages[i];  // assume the pointer is valid again
        void *kaddr = kmap_local_page(pg);
        unsigned char val = *(unsigned char *)kaddr;
        kunmap_local(kaddr);

        if (val != i)
            printk(KERN_ERR "[TEST] Refaulted page %d has incorrect data: %u\n", i, val);
        else
            printk(KERN_INFO "[TEST] Refaulted page %d verified\n", i);
    }

    printk(KERN_INFO "[TEST] Forcing eviction of first page\n");
    remote_numa_client_cache_free_page(cache, pages[0]);

    printk(KERN_INFO "[TEST] Attempting to refault evicted page...\n");
    *((unsigned long *)&fake_vmf.address) = addr; // address for pages[0]
    ret = remote_numa_client_cache_refault(cache, pages[0], &fake_vmf);
    if (ret)
        printk(KERN_ERR "[TEST] Refault failed with error %d\n", ret);
    else
        printk(KERN_INFO "[TEST] Refault succeeded\n");

    return 0;
}

static long remote_numa_test_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    if (cmd == REMOTE_NUMA_RUN_TEST) {
        printk(KERN_INFO "[TEST] IOCTL triggered with addr = %px\n", (void *)arg);
        return test_page_lifecycle(client_cache, arg);
    }
    return -ENOTTY;
}

static const struct file_operations remote_numa_test_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = remote_numa_test_ioctl,
};

static ssize_t test_trigger(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    unsigned long addr = (unsigned long)current;
    return test_page_lifecycle(client_cache, addr);
}

static const struct proc_ops test_fops = {
    .proc_read = test_trigger,
};

static int __init remote_numa_test_init(void)
{
    int ret;

    printk(KERN_INFO "[TEST MODULE] Loading remote NUMA test driver...\n");

    if (!client_cache) {
        printk(KERN_ERR "[TEST MODULE] global_remote_cache not initialized\n");
        return -EINVAL;
    }
    global_remote_cache = client_cache;

    // Register /proc entry
    test_entry = proc_create("remote_numa_test", 0, NULL, &test_fops);
    if (!test_entry) {
        printk(KERN_ERR "[TEST MODULE] Failed to create /proc entry\n");
        return -ENOMEM;
    }

    // Register character device
    ret = alloc_chrdev_region(&devno, 0, 1, "remote_numa_test");
    if (ret)
        return ret;

    cdev_init(&test_cdev, &remote_numa_test_fops);
    ret = cdev_add(&test_cdev, devno, 1);
    if (ret)
        return ret;

    test_class = class_create("remote_numa");
    if (IS_ERR(test_class))
        return PTR_ERR(test_class);

    device_create(test_class, NULL, devno, NULL, "remote_numa_test");

    printk(KERN_INFO "[TEST MODULE] /dev/remote_numa_test ready\n");
    return 0;
}

static void __exit remote_numa_test_exit(void)
{
    proc_remove(test_entry);
    device_destroy(test_class, devno);
    class_destroy(test_class);
    cdev_del(&test_cdev);
    unregister_chrdev_region(devno, 1);
    printk(KERN_INFO "[TEST MODULE] Remote NUMA test module unloaded\n");
}

module_init(remote_numa_test_init);
module_exit(remote_numa_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Trevor C Kemp");
MODULE_DESCRIPTION("Remote NUMA page lifecycle test module");

