// SPDX-License-Identifier: GPL-2.0
/*
 * remote_numa_test.c - Test driver for remote NUMA page lifecycle
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/pid.h>
#include <linux/mm_types.h>
#include <linux/sched/mm.h> // for use_mm()
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/sched/signal.h>

#include "client_cache.h"
#include "transport.h"

extern remote_numa_client_cache_t *client_cache;
remote_numa_client_cache_t *global_remote_cache = NULL;

#define TEST_CACHE_PAGES 2
#define TOTAL_TEST_PAGES 50  // Increased for stress testing
#define REMOTE_NUMA_RUN_TEST _IOW('R', 1, unsigned long)

#define MAX_TEST_ERROR_EVENTS 128
#define MAX_TEST_EVENT_AGG_ENTRIES 16

#define REMOTE_NUMA_TEST_STRESS_2H

#ifdef REMOTE_NUMA_TEST_STRESS_2H
#define REMOTE_NUMA_TEST_STRESS_DURATION_MS (2UL * 60 * 60 * 1000)
#endif

struct test_error_event {
    const char *operation;
    int index;
    int err;
    bool timeout;
};

struct test_event_aggregate_entry {
    const char *operation;
    unsigned int count;
    unsigned int timeout_count;
};

struct test_event_aggregate {
    struct test_event_aggregate_entry entries[MAX_TEST_EVENT_AGG_ENTRIES];
    unsigned int entry_count;
    unsigned long total_events;
    unsigned long overflow_events;
};

static void record_test_event(struct test_error_event *events, int *count,
                 const char *operation, int index, int err,
                 bool timeout)
{
    if (!events || !count || *count >= MAX_TEST_ERROR_EVENTS)
        return;
    events[*count].operation = operation;
    events[*count].index = index;
    events[*count].err = err;
    events[*count].timeout = timeout;
    (*count)++;
}

static void dump_test_events(struct test_error_event *events, int count)
{
    int i;
    if (!count)
        return;
    printk(KERN_ERR "[TEST] ---- error summary (%d entries) ----\n", count);
    for (i = 0; i < count; i++)
        printk(KERN_ERR "[TEST]   %s in %s (index %d) err=%d\n",
               events[i].timeout ? "Timeout" : "Error",
               events[i].operation,
               events[i].index,
               events[i].err);
}

static void aggregate_test_events(struct test_event_aggregate *agg,
                       struct test_error_event *events,
                       int count)
{
    int i, j;
    if (!agg || !events)
        return;
    for (i = 0; i < count; i++) {
        struct test_error_event *evt = &events[i];
        struct test_event_aggregate_entry *entry = NULL;
        for (j = 0; j < agg->entry_count; j++) {
            if (agg->entries[j].operation == evt->operation) {
                entry = &agg->entries[j];
                break;
            }
        }
        if (!entry) {
            if (agg->entry_count >= MAX_TEST_EVENT_AGG_ENTRIES) {
                agg->overflow_events += (count - i);
                break;
            }
            entry = &agg->entries[agg->entry_count++];
            entry->operation = evt->operation;
            entry->count = 0;
            entry->timeout_count = 0;
        }
        entry->count++;
        if (evt->timeout)
            entry->timeout_count++;
    }
    agg->total_events += count;
}

static void dump_test_event_aggregate(struct test_event_aggregate *agg)
{
    unsigned int i;
    if (!agg || !agg->total_events)
        return;
    printk(KERN_ERR "[TEST] ---- aggregate failure summary (%lu events) ----\n",
           agg->total_events);
    for (i = 0; i < agg->entry_count; i++)
        printk(KERN_ERR "[TEST]   %s: total=%u timeout=%u\n",
               agg->entries[i].operation,
               agg->entries[i].count,
               agg->entries[i].timeout_count);
    if (agg->overflow_events)
        printk(KERN_ERR "[TEST]   (truncated %lu additional events)\n",
               agg->overflow_events);
}

static dev_t devno;
static struct class *test_class;
static struct cdev test_cdev;
static struct proc_dir_entry *test_entry;

static int test_page_lifecycle(remote_numa_client_cache_t *cache,
                     unsigned long addr,
                     struct test_event_aggregate *agg)
{
    struct vm_fault fake_vmf;
    struct vm_area_struct *vma;
    struct page *pages[TOTAL_TEST_PAGES];
    int ret;
    int rc = 0;
    struct test_error_event *events;
    int event_count = 0;

    events = kcalloc(MAX_TEST_ERROR_EVENTS,
              sizeof(*events), GFP_KERNEL);
    if (!events) {
        printk(KERN_ERR "[TEST] Failed to alloc event buffer\n");
        return -ENOMEM;
    }

    if (!current->mm) {
        printk(KERN_ERR "[TEST] current->mm is NULL\n");
        rc = -EINVAL;
        goto out;
    }

    down_read(&current->mm->mmap_lock);
    vma = find_vma(current->mm, addr);
    up_read(&current->mm->mmap_lock);
    if (!vma) {
        printk(KERN_ERR "[TEST] No VMA found for address %px\n", (void *)addr);
        rc = -EINVAL;
        goto out;
    }

    printk(KERN_INFO "=== TEST: Remote NUMA stress test [PID %d] ===\n", current->pid);

    struct vm_area_struct fake_vma = *vma;
    memset(&fake_vmf, 0, sizeof(fake_vmf));
    *((struct vm_area_struct **)&fake_vmf.vma) = &fake_vma;

    // Phase 1: Allocate many pages to force evictions
    for (int i = 0; i < TOTAL_TEST_PAGES; i++) {
        *((unsigned long *)&fake_vmf.address) = addr + (i * PAGE_SIZE);

        int retry_count = 0;
        const int max_retries = 10;
        while (retry_count < max_retries) {
            pages[i] = remote_numa_client_cache_alloc(cache, &fake_vmf);
            if (IS_ERR(pages[i])) {
                if (PTR_ERR(pages[i]) == -EAGAIN) {
                    printk(KERN_INFO "[TEST] Allocation %d: transfer in progress, retrying (attempt %d)...\n",
                           i, retry_count + 1);
                    msleep(50); /* Brief delay before retry */
                    retry_count++;
                    continue;
                } else {
                    ret = PTR_ERR(pages[i]);
                    printk(KERN_ERR "[TEST] Allocation %d failed with error %ld\n", i, PTR_ERR(pages[i]));
                    record_test_event(events, &event_count,
                              "remote_numa_client_cache_alloc",
                              i, ret, false);
                    rc = ret;
                    goto out;
                }
            } else if (!pages[i]) {
                printk(KERN_ERR "[TEST] Allocation %d failed\n", i);
                record_test_event(events, &event_count,
                          "remote_numa_client_cache_alloc",
                          i, -ENOMEM, false);
                rc = -ENOMEM;
                goto out;
            }
            break;
        }

        if (retry_count >= max_retries) {
            printk(KERN_ERR "[TEST] Allocation %d timed out after %d retries\n", i, max_retries);
            record_test_event(events, &event_count,
                      "remote_numa_client_cache_alloc",
                      i, -ETIMEDOUT, true);
            rc = -ETIMEDOUT;
            goto out;
        }

        if ((i % 10) == 0)
            printk(KERN_INFO "[TEST] Allocation %d/%d succeeded\n", i, TOTAL_TEST_PAGES);

        void *kaddr = kmap_local_page(pages[i]);
        memset(kaddr, i, PAGE_SIZE);
        kunmap_local(kaddr);
    }

    printk(KERN_INFO "[TEST] Phase 1 complete: %d pages allocated\n", TOTAL_TEST_PAGES);

    // Phase 2: Free random pages to create churn
    printk(KERN_INFO "[TEST] Phase 2: Freeing random pages\n");
    for (int i = 0; i < TOTAL_TEST_PAGES / 4; i++) {
        int idx = i * 4;  // Free every 4th page
        remote_numa_client_cache_free_page(cache, pages[idx]);
        if ((i % 5) == 0)
            printk(KERN_INFO "[TEST] Freed page %d\n", idx);
    }

    // Phase 3: Refault and verify remaining pages (skip freed ones)
    printk(KERN_INFO "[TEST] Phase 3: Refaulting and verifying\n");
    int verify_count = 0;
    for (int i = 0; i < TOTAL_TEST_PAGES - 1; i++) {
        // Skip pages we freed in phase 2
        if ((i % 4) == 0)
            continue;
            
        *((unsigned long *)&fake_vmf.address) = addr + (i * PAGE_SIZE);

        int retry_count = 0;
        const int max_retries = 10;
        while (retry_count < max_retries) {
            ret = remote_numa_client_cache_refault(cache, pages[i], &fake_vmf);
            if (ret == -EAGAIN) {
                if ((retry_count % 3) == 0)
                    printk(KERN_INFO "[TEST] Refault %d: retrying (attempt %d)...\n",
                           i, retry_count + 1);
                msleep(20); /* Brief delay before retry */
                retry_count++;
                continue;
            } else if (ret) {
                printk(KERN_ERR "[TEST] Refault %d failed with error %d\n", i, ret);
                record_test_event(events, &event_count,
                          "remote_numa_client_cache_refault",
                          i, ret, false);
                break;
            }
            break;
        }

        if (retry_count >= max_retries) {
            printk(KERN_ERR "[TEST] Refault %d timed out after %d retries\n", i, max_retries);
            record_test_event(events, &event_count,
                      "remote_numa_client_cache_refault",
                      i, -ETIMEDOUT, true);
            continue;
        }

        if (ret) {
            printk(KERN_ERR "[TEST] Skipping verification for page %d due to refault failure\n", i);
            continue;
        }

        struct page *pg = pages[i];  // assume the pointer is valid again
        void *kaddr = kmap_local_page(pg);
	bool valid = true;
	u8 *dat = kaddr;
	
	for (int j = 0; j < PAGE_SIZE; j++)
	{
		if (dat[j] != i)
		{
			valid = false;
            		printk(KERN_ERR "[TEST] Refaulted page %d has incorrect data at offset %d: %u (expected %d)\n", 
			       i, j, dat[j], i);
			break;
		}
	}
        
        kunmap_local(kaddr);

	if (valid) {
            verify_count++;
            if ((verify_count % 5) == 0)
                printk(KERN_INFO "[TEST] Verified %d pages\n", verify_count);
        } else {
            printk(KERN_ERR "[TEST] Page %d verification FAILED\n", i);
            record_test_event(events, &event_count,
                      "verify_page_data",
                      i, -EIO, false);
        }
    }

    printk(KERN_INFO "[TEST] Phase 3 complete: %d pages verified\n", verify_count);

    // Phase 4: Free more pages and test final refault
    /* Cleanup */
    printk(KERN_INFO "[TEST] Phase 4: Cleanup remaining pages\n");
    int cleanup_count = 0;
    for (int i = 0; i < TOTAL_TEST_PAGES; i++) {
        if (pages[i]) {
            remote_numa_client_cache_free_page(cache, pages[i]);
            cleanup_count++;
        }
    }
    printk(KERN_INFO "[TEST] Cleanup complete: freed %d pages\n", cleanup_count);

    printk(KERN_INFO "[TEST] ========== TEST SUITE COMPLETE ==========\n");
    rc = 0;
out:
    dump_test_events(events, event_count);
    aggregate_test_events(agg, events, event_count);
    kfree(events);
    return rc;
}

static int remote_numa_run_tests(remote_numa_client_cache_t *cache, unsigned long addr)
{
#ifdef REMOTE_NUMA_TEST_STRESS_2H
    unsigned long deadline = jiffies +
        msecs_to_jiffies(REMOTE_NUMA_TEST_STRESS_DURATION_MS);
    unsigned int iterations = 0;
    unsigned int successes = 0;
    unsigned int failures = 0;
    int rc = 0;
    struct test_event_aggregate agg = { };

    printk(KERN_INFO "[TEST] Stress mode enabled: running for up to 2 hours\n");
    ssleep(5); // brief pause before starting
    while (time_before(jiffies, deadline)) {
        iterations++;
        rc = test_page_lifecycle(cache, addr, &agg);
        if (!rc)
            successes++;
        else
            failures++;

        if (signal_pending(current)) {
            printk(KERN_INFO "[TEST] Stress run interrupted after %u iterations\n",
                   iterations);
            break;
        }
        cond_resched();
    }

    if (!iterations)
        printk(KERN_WARNING "[TEST] Stress mode exit with no iterations\n");
    printk(KERN_INFO "[TEST] Stress summary: iterations=%u successes=%u failures=%u\n",
           iterations, successes, failures);
    dump_test_event_aggregate(&agg);
    return rc;
#else
    printk(KERN_INFO "[TEST] Running single iteration test\n");
    ssleep(5); // brief pause before starting
    return test_page_lifecycle(cache, addr, NULL);
#endif
}

static long remote_numa_test_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    if (cmd == REMOTE_NUMA_RUN_TEST) {
        printk(KERN_INFO "[TEST] IOCTL triggered with addr = %px\n", (void *)arg);
        return remote_numa_run_tests(client_cache, arg);
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
    return remote_numa_run_tests(client_cache, addr);
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

