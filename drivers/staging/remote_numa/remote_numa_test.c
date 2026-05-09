// SPDX-License-Identifier: GPL-2.0
/*
 * remote_numa_test.c - Test driver for remote NUMA page lifecycle
 */

#include <linux/semaphore.h>
#include <linux/sched/mm.h>

#include <linux/atomic.h>

#include <linux/completion.h>


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
#include <linux/sched/mm.h>
#include <linux/sched/mm.h> // for use_mm()
#include <linux/err.h>
#include <linux/delay.h>

#include "client_cache.h"
#include "transport.h"

#include <linux/kthread.h>
#include <linux/random.h>

#define REMOTE_NUMA_STRESS_TEST 1
#define REMOTE_NUMA_STRESS_MAX_CONCURRENT_OPS 4
#define REMOTE_NUMA_STRESS_MINUTES 5
#define REMOTE_NUMA_STRESS_THREADS 5

/* Burst size for churn/refault/free operations in the main stress loop. */
#define REMOTE_NUMA_STRESS_BURST_OPS 128

/*
 * Number of distinct (addr,page) slots the stress test touches.
 * Must be > cache capacity (1200) to reliably trigger evictions/refaults.
 */
#define REMOTE_NUMA_STRESS_TEST_PAGES (1200 + 600)

struct stress_test_ctx {
    struct semaphore sem;
    atomic_long_t active_allocs;
};

#define REMOTE_NUMA_STRESS_CACHE_SIZE 1200

struct stress_thread_info {
    int id;
    struct page **owned_pages;
    u32 *expected_seed;
    int num_owned;
    struct task_struct *task;
    struct completion done;
    remote_numa_client_cache_t *cache;
    struct stress_test_ctx *ctx;
    unsigned long base_addr;
    int had_error;
    struct mm_struct *mm;
    unsigned long active_owned;
    // Statistics
    unsigned long alloc_success;
    unsigned long alloc_fail;
    unsigned long alloc_retries;
    unsigned long refault_success;
    unsigned long refault_fail;
    unsigned long refault_retries;
    unsigned long refault_noent;
    unsigned long data_corrupt;
    unsigned long free_success;
    unsigned long free_fail;
    unsigned long free_noent;
};

static inline u32 stress_seed_for_addr(unsigned long addr)
{
    return (u32)(addr >> PAGE_SHIFT) ^ 0x9e3779b9u;
}

static void stress_fill_page_pattern(struct page *pg, u32 seed)
{
    u8 *kaddr = kmap_local_page(pg);
    for (int j = 0; j < PAGE_SIZE; j++)
        kaddr[j] = (u8)(seed ^ (u32)j);
    kunmap_local(kaddr);
}

static bool stress_verify_page_pattern(struct page *pg, u32 seed, int *bad_off,
                       u8 *exp, u8 *got)
{
    u8 *kaddr = kmap_local_page(pg);
    for (int j = 0; j < PAGE_SIZE; j++) {
        u8 expected = (u8)(seed ^ (u32)j);
        if (kaddr[j] != expected) {
            if (bad_off)
                *bad_off = j;
            if (exp)
                *exp = expected;
            if (got)
                *got = kaddr[j];
            kunmap_local(kaddr);
            return false;
        }
    }
    kunmap_local(kaddr);
    return true;
}

static int stress_prepare_vmf(struct mm_struct *mm, unsigned long addr,
                struct vm_fault *fake_vmf)
{
    struct vm_area_struct *vma = NULL;

    memset(fake_vmf, 0, sizeof(*fake_vmf));
    down_read(&mm->mmap_lock);
    vma = find_vma(mm, addr);
    up_read(&mm->mmap_lock);
    if (!vma || vma->vm_start > addr || vma->vm_end <= addr)
        return -EINVAL;

    /* vm_fault fields are const in some kernels; populate via casts. */
    *((struct vm_area_struct **)&fake_vmf->vma) = vma;
    *((unsigned long *)&fake_vmf->address) = addr;
    return 0;
}

static int stress_alloc_idx(struct stress_thread_info *info, int idx, unsigned long cur_addr)
{
    struct vm_fault fake_vmf;
    int retry_count = 0;
    const int max_retries = 256;
    struct page *pg = NULL;
    int prep_ret;

    /* Never treat ERR_PTR pages as real pages */
    if (info->owned_pages[idx] && IS_ERR(info->owned_pages[idx]))
        info->owned_pages[idx] = NULL;
    if (info->owned_pages[idx])
        return 0;

    prep_ret = stress_prepare_vmf(info->mm, cur_addr, &fake_vmf);
    if (prep_ret) {
         printk(KERN_DEBUG "[STRESS %d] ALLOC VMA not found for idx=%d addr=%px active_total=%ld active_thread=%lu\n",
             info->id, idx, (void *)cur_addr,
             atomic_long_read(&info->ctx->active_allocs),
             info->active_owned);
        info->alloc_fail++;
        info->had_error = 1;
        return prep_ret;
    }

    if (down_interruptible(&info->ctx->sem)) {
         printk(KERN_DEBUG "[STRESS %d] ALLOC interrupted idx=%d addr=%px active_total=%ld active_thread=%lu\n",
               info->id, idx, (void *)cur_addr,
             atomic_long_read(&info->ctx->active_allocs),
             info->active_owned);
        info->alloc_fail++;
        info->had_error = 1;
        return -EINTR;
    }

    do {
        preempt_disable();
        pg = remote_numa_client_cache_alloc(info->cache, &fake_vmf);
        preempt_enable();

        if (IS_ERR(pg) && PTR_ERR(pg) == -EAGAIN) {
            msleep(5);
            retry_count++;
        } else {
            break;
        }
    } while (retry_count < max_retries);

    up(&info->ctx->sem);

    info->alloc_retries += retry_count;
    if (IS_ERR(pg) || !pg) {
         printk(KERN_DEBUG "[STRESS %d] ALLOC FAILED idx=%d addr=%px retries=%d active_total=%ld active_thread=%lu\n",
             info->id, idx, (void *)cur_addr, retry_count,
             atomic_long_read(&info->ctx->active_allocs),
             info->active_owned);
        info->alloc_fail++;
        info->had_error = 1;
        info->owned_pages[idx] = NULL;
        return IS_ERR(pg) ? (int)PTR_ERR(pg) : -ENOMEM;
    }

    info->alloc_success++;
    info->owned_pages[idx] = pg;
    atomic_long_inc(&info->ctx->active_allocs);
    info->active_owned++;

    /* Write a deterministic pattern so refault can validate content later. */
    if (info->expected_seed) {
        u32 seed = stress_seed_for_addr(cur_addr);
        info->expected_seed[idx] = seed;
        stress_fill_page_pattern(pg, seed);
    }
    return 0;
}

static int stress_refault_idx(struct stress_thread_info *info, int idx, unsigned long cur_addr)
{
    struct vm_fault fake_vmf;
    int retry_count = 0;
    const int max_retries = 256;
    int ret;
    int prep_ret;

    if (!info->owned_pages[idx] || IS_ERR(info->owned_pages[idx]))
        return 0;

    prep_ret = stress_prepare_vmf(info->mm, cur_addr, &fake_vmf);
    if (prep_ret) {
        printk(KERN_DEBUG "[STRESS %d] REFAULT VMA not found for idx=%d addr=%px\n",
               info->id, idx, (void *)cur_addr);
        info->refault_fail++;
        info->had_error = 1;
        return prep_ret;
    }

    /* If nothing recorded this address as evicted, refault can't succeed. */
    if (!remote_numa_client_cache_refault_pending(fake_vmf.vma->vm_mm, cur_addr)) {
        info->refault_noent++;
        return 0;
    }

    if (down_interruptible(&info->ctx->sem)) {
        info->refault_fail++;
        info->had_error = 1;
        return -EINTR;
    }

    do {
        preempt_disable();
        ret = remote_numa_client_cache_refault(info->cache, info->owned_pages[idx], &fake_vmf);
        preempt_enable();

        if (ret == -EAGAIN) {
            msleep(5);
            retry_count++;
        } else {
            break;
        }
    } while (retry_count < max_retries);

    up(&info->ctx->sem);

    info->refault_retries += retry_count;
    if (ret == -ENOENT) {
        /* Not evicted (no refault entry) is expected; count and move on. */
        info->refault_noent++;
        return 0;
    }
    if (ret) {
        printk(KERN_DEBUG "[STRESS %d] REFAULT FAILED idx=%d addr=%px ret=%d retries=%d\n",
               info->id, idx, (void *)cur_addr, ret, retry_count);
        info->refault_fail++;
        info->had_error = 1;
        return ret;
    }

    /* Verify the page data after a successful refault. */
    if (info->expected_seed) {
        u32 seed = info->expected_seed[idx];
        int bad_off;
        u8 exp, got;
        if (!seed)
            seed = stress_seed_for_addr(cur_addr);
        if (!stress_verify_page_pattern(info->owned_pages[idx], seed, &bad_off, &exp, &got)) {
            printk(KERN_ERR "[STRESS %d] DATA CORRUPT idx=%d addr=%px off=%d exp=%u got=%u\n",
                   info->id, idx, (void *)cur_addr, bad_off, exp, got);
            info->data_corrupt++;
            info->had_error = 1;
        }
    }
    info->refault_success++;
    return 0;
}

static int stress_free_idx(struct stress_thread_info *info, int idx, unsigned long cur_addr)
{
    int ret;

    if (!info->owned_pages[idx] || IS_ERR(info->owned_pages[idx])) {
        info->owned_pages[idx] = NULL;
        return 0;
    }

    if (down_interruptible(&info->ctx->sem)) {
        info->free_fail++;
        info->had_error = 1;
        return -EINTR;
    }
    preempt_disable();
    ret = remote_numa_client_cache_free_page(info->cache, info->owned_pages[idx]);
    preempt_enable();
    up(&info->ctx->sem);

	/* Busy/in-flight: do not treat as failure; retry later and keep ownership. */
	if (ret == -EAGAIN)
		return -EAGAIN;

    if (ret == -ENOENT) {
        info->free_noent++;
        atomic_long_dec(&info->ctx->active_allocs);
        if (info->active_owned)
            info->active_owned--;
        info->owned_pages[idx] = NULL;
        return 0;
    }
    if (ret) {
        printk(KERN_DEBUG "[STRESS %d] FREE FAILED idx=%d addr=%px ret=%d\n",
               info->id, idx, (void *)cur_addr, ret);
        info->free_fail++;
        info->had_error = 1;
        atomic_long_dec(&info->ctx->active_allocs);
        if (info->active_owned)
            info->active_owned--;
        info->owned_pages[idx] = NULL;
        return ret;
    }

    info->free_success++;
    atomic_long_dec(&info->ctx->active_allocs);
    if (info->active_owned)
        info->active_owned--;
    info->owned_pages[idx] = NULL;
    return 0;
}

static int stress_thread_fn(void *data) {
        // Use a semaphore to limit concurrent ops
        // (Initialized in run_stress_test)
    struct stress_thread_info *info = data;
    unsigned long duration_jiffies = (unsigned long)REMOTE_NUMA_STRESS_MINUTES * 60 * HZ;
        unsigned long start_jiffies;
        unsigned long end_time;
    // Zero stats
    info->active_owned = 0;
    info->alloc_success = 0;
    info->alloc_fail = 0;
    info->alloc_retries = 0;
    info->refault_success = 0;
    info->refault_fail = 0;
    info->refault_retries = 0;
    info->refault_noent = 0;
    info->data_corrupt = 0;
    info->free_success = 0;
    info->free_fail = 0;
    info->free_noent = 0;

	int cache_pages = REMOTE_NUMA_STRESS_CACHE_SIZE / REMOTE_NUMA_STRESS_THREADS;
	if (cache_pages < 1)
		cache_pages = 1;
	int victim_pages = info->num_owned;
	if (victim_pages > cache_pages)
		victim_pages = cache_pages;
	int churn_start = victim_pages;
	int churn_pages = info->num_owned - victim_pages;
	int churn_alloc_cursor = churn_start;
	int churn_free_cursor = churn_start;
	int victim_cursor = 0;

	if (churn_pages <= 0) {
		printk(KERN_WARNING "[STRESS %d] No churn region (num_owned=%d cache_pages=%d); eviction/refault may be absent\n",
		       info->id, info->num_owned, cache_pages);
	}

    /* Initial allocation phase: fill the victim set (cache-sized working set). */
    for (int idx = 0; idx < victim_pages; idx++) {
        unsigned long cur_addr = info->base_addr + (idx * PAGE_SIZE);
        (void)stress_alloc_idx(info, idx, cur_addr);
    }

    /* Start timing AFTER initial allocations so we actually stress for the full duration. */
    start_jiffies = jiffies;
    end_time = start_jiffies + duration_jiffies;
    printk(KERN_INFO "[STRESS %d] stress_start_jiffies=%lu end_time=%lu duration_jiffies=%lu HZ=%d\n",
           info->id, start_jiffies, end_time, duration_jiffies, HZ);

    // Main stress phase: deterministic bursts to force eviction/refault/free
    while (time_before(jiffies, end_time) && !kthread_should_stop()) {
        if ((jiffies % HZ) == 0) {
            printk(KERN_DEBUG "[STRESS %d] jiffies=%lu (seconds elapsed: %lu/%d)\n", info->id, jiffies, (jiffies - start_jiffies) / HZ, REMOTE_NUMA_STRESS_MINUTES * 60);
        }
		/* 1) Churn alloc burst: allocate pages beyond cache size to force evictions */
		for (int i = 0; i < REMOTE_NUMA_STRESS_BURST_OPS && churn_pages > 0; i++) {
			int idx = churn_alloc_cursor;
			unsigned long cur_addr;
			churn_alloc_cursor++;
			if (churn_alloc_cursor >= info->num_owned)
				churn_alloc_cursor = churn_start;
			cur_addr = info->base_addr + (idx * PAGE_SIZE);
			(void)stress_alloc_idx(info, idx, cur_addr);
		}

		/* 2) Refault burst: try victims; -ENOENT means not evicted (expected) */
		for (int i = 0; i < REMOTE_NUMA_STRESS_BURST_OPS && victim_pages > 0; i++) {
			int idx = victim_cursor;
			unsigned long cur_addr;
			victim_cursor++;
			if (victim_cursor >= victim_pages)
				victim_cursor = 0;
			cur_addr = info->base_addr + (idx * PAGE_SIZE);
			(void)stress_refault_idx(info, idx, cur_addr);
		}

		/* 3) Churn free burst: free some churn pages so we can re-alloc and keep pressure */
		for (int i = 0; i < REMOTE_NUMA_STRESS_BURST_OPS && churn_pages > 0; i++) {
			int idx = churn_free_cursor;
			unsigned long cur_addr;
			churn_free_cursor++;
			if (churn_free_cursor >= info->num_owned)
				churn_free_cursor = churn_start;
			cur_addr = info->base_addr + (idx * PAGE_SIZE);
			(void)stress_free_idx(info, idx, cur_addr);
		}
        msleep(1);
    }

    printk(KERN_INFO "[STRESS %d] STATS: alloc_success=%lu alloc_fail=%lu alloc_retries=%lu\n",
        info->id, info->alloc_success, info->alloc_fail, info->alloc_retries);
    printk(KERN_INFO "[STRESS %d] STATS: refault_success=%lu refault_fail=%lu refault_retries=%lu refault_noent=%lu data_corrupt=%lu\n",
        info->id, info->refault_success, info->refault_fail, info->refault_retries, info->refault_noent, info->data_corrupt);
    printk(KERN_INFO "[STRESS %d] STATS: free_success=%lu free_fail=%lu free_noent=%lu\n",
        info->id, info->free_success, info->free_fail, info->free_noent);
    complete(&info->done);

    /*
     * Keep the kthread alive until run_stress_test() calls kthread_stop().
     * Otherwise the task may exit and be freed before kthread_stop(), which
     * can trigger refcount_t use-after-free warnings.
     */
    while (!kthread_should_stop())
        msleep(100);
    return 0;
}

static int run_stress_test(remote_numa_client_cache_t *cache, unsigned long addr, struct mm_struct *mm) {
    struct stress_test_ctx ctx;
    sema_init(&ctx.sem, REMOTE_NUMA_STRESS_MAX_CONCURRENT_OPS);
    atomic_long_set(&ctx.active_allocs, 0);
    // Validate address range is covered by contiguous VMAs; clamp to what is mapped.
    unsigned long requested_end = addr + (unsigned long)REMOTE_NUMA_STRESS_TEST_PAGES * PAGE_SIZE;
    unsigned long covered_end = addr;
    struct vm_area_struct *vma = NULL;
    struct vma_iterator vmi;
    down_read(&mm->mmap_lock);
    vma_iter_init(&vmi, mm, addr);
    vma = vma_find(&vmi, requested_end);
    if (!vma || vma->vm_start > addr) {
        up_read(&mm->mmap_lock);
        printk(KERN_ERR "[STRESS TEST] Address %px is not covered by any VMA\n", (void *)addr);
        return -EINVAL;
    }

    if (vma->vm_end > covered_end)
        covered_end = vma->vm_end;

    while (covered_end < requested_end) {
        vma_iter_set(&vmi, covered_end);
        vma = vma_find(&vmi, requested_end);
        if (!vma)
            break;
        if (vma->vm_start > covered_end)
            break; /* gap */
        if (vma->vm_end > covered_end)
            covered_end = vma->vm_end;
    }
    up_read(&mm->mmap_lock);

    if (covered_end < addr + PAGE_SIZE) {
        printk(KERN_ERR "[STRESS TEST] Address %px does not have even one page mapped\n", (void *)addr);
        return -EINVAL;
    }

    unsigned long available_pages = (covered_end - addr) / PAGE_SIZE;
    unsigned long test_pages = REMOTE_NUMA_STRESS_TEST_PAGES;
    if (available_pages < test_pages) {
        printk(KERN_WARNING "[STRESS TEST] Requested %lu pages (%px - %px) but only %lu pages are mapped contiguously (%px - %px). Clamping.\n",
               test_pages, (void *)addr, (void *)requested_end,
               available_pages, (void *)addr, (void *)(addr + available_pages * PAGE_SIZE));
        test_pages = available_pages;
    }

    if (test_pages % REMOTE_NUMA_STRESS_THREADS) {
        unsigned long new_pages = (test_pages / REMOTE_NUMA_STRESS_THREADS) * REMOTE_NUMA_STRESS_THREADS;
        printk(KERN_WARNING "[STRESS TEST] test_pages (%lu) not divisible by threads (%d); clamping to %lu pages\n",
               test_pages, REMOTE_NUMA_STRESS_THREADS, new_pages);
        test_pages = new_pages;
    }
    if (!test_pages) {
        printk(KERN_ERR "[STRESS TEST] No pages available for stress test\n");
        return -EINVAL;
    }

    if (test_pages <= REMOTE_NUMA_STRESS_CACHE_SIZE) {
        printk(KERN_WARNING "[STRESS TEST] test_pages=%lu <= cache_size=%d; evictions/refaults may be rare or absent. Increase userspace mapping or REMOTE_NUMA_STRESS_TEST_PAGES.\n",
               test_pages, REMOTE_NUMA_STRESS_CACHE_SIZE);
    }

    struct stress_thread_info *thread_infos = kzalloc(sizeof(*thread_infos) * REMOTE_NUMA_STRESS_THREADS, GFP_KERNEL);
    if (!thread_infos) {
        printk(KERN_ERR "[STRESS TEST] Failed to allocate thread_infos\n");
        return -ENOMEM;
    }
    struct page **all_pages = kzalloc(sizeof(*all_pages) * test_pages, GFP_KERNEL);
    if (!all_pages) {
        printk(KERN_ERR "[STRESS TEST] Failed to allocate all_pages\n");
        kfree(thread_infos);
        return -ENOMEM;
    }
    memset(all_pages, 0, sizeof(*all_pages) * test_pages);

    u32 *all_expected = kzalloc(sizeof(*all_expected) * test_pages, GFP_KERNEL);
    if (!all_expected) {
        printk(KERN_ERR "[STRESS TEST] Failed to allocate all_expected\n");
        kfree(all_pages);
        kfree(thread_infos);
        return -ENOMEM;
    }
    // Partition pages among threads
    int pages_per_thread = test_pages / REMOTE_NUMA_STRESS_THREADS;
    for (int t = 0; t < REMOTE_NUMA_STRESS_THREADS; t++) {
        thread_infos[t].id = t;
        thread_infos[t].cache = cache;
        thread_infos[t].ctx = &ctx;
        thread_infos[t].base_addr = addr + (unsigned long)t * pages_per_thread * PAGE_SIZE;
        thread_infos[t].num_owned = pages_per_thread;
        thread_infos[t].owned_pages = &all_pages[t * pages_per_thread];
        thread_infos[t].expected_seed = &all_expected[t * pages_per_thread];
        thread_infos[t].had_error = 0;
        thread_infos[t].mm = mm;
    }
    /*
     * Do NOT pre-allocate here. It was previously storing ERR_PTR(-EAGAIN)
     * into all_pages[] (no retry / no IS_ERR handling), which then gets
     * treated as a real struct page* by refault/free, causing massive failures.
     * Each thread performs its own robust initial allocation with retries.
     */
    // Start threads
    for (int t = 0; t < REMOTE_NUMA_STRESS_THREADS; t++) {
        init_completion(&thread_infos[t].done);
        thread_infos[t].task = kthread_run(stress_thread_fn, &thread_infos[t], "numa_stress_%d", t);
    }

    /*
     * Wait for threads to finish their full-duration stress phase.
     * (Duration starts after initial alloc inside the thread.)
     */
    for (int t = 0; t < REMOTE_NUMA_STRESS_THREADS; t++) {
        if (thread_infos[t].task) {
            unsigned long timeout = ((unsigned long)REMOTE_NUMA_STRESS_MINUTES * 60 + 30) * HZ;
            if (!wait_for_completion_timeout(&thread_infos[t].done, timeout))
                printk(KERN_WARNING "[STRESS TEST] Thread %d did not finish before timeout; stopping\n", t);
        }
    }

    /* Stop/join threads for cleanup (also releases kthread resources). */
    for (int t = 0; t < REMOTE_NUMA_STRESS_THREADS; t++) {
        if (thread_infos[t].task)
            kthread_stop(thread_infos[t].task);
    }
    int any_error = 0;
    for (int t = 0; t < REMOTE_NUMA_STRESS_THREADS; t++) {
        if (thread_infos[t].had_error) {
            any_error = 1;
            printk(KERN_ERR "[STRESS TEST] Thread %d encountered errors!\n", t);
        }
    }
    if (!any_error)
        printk(KERN_INFO "[STRESS TEST] Completed %d threads for %d minutes with no errors\n", REMOTE_NUMA_STRESS_THREADS, REMOTE_NUMA_STRESS_MINUTES);
    else
        printk(KERN_ERR "[STRESS TEST] Errors occurred during stress test!\n");
    kfree(all_pages);
    kfree(all_expected);
    kfree(thread_infos);
    return any_error ? -EIO : 0;
}

extern remote_numa_client_cache_t *client_cache;
remote_numa_client_cache_t *global_remote_cache = NULL;

/* Allocate enough pages to trigger cache eviction (cache holds 1200) */
#define TOTAL_REFAULTS 600
#define TOTAL_TEST_PAGES (1200 + TOTAL_REFAULTS)
#define REMOTE_NUMA_RUN_TEST _IOW('R', 1, unsigned long)

static dev_t devno;
static struct class *test_class;
static struct cdev test_cdev;
static struct proc_dir_entry *test_entry;

static int test_page_lifecycle(remote_numa_client_cache_t *cache, unsigned long addr)
{
    struct vm_fault fake_vmf;
    struct vm_area_struct *vma;
    struct page **pages;
    int ret;

#ifdef REMOTE_NUMA_STRESS_TEST
    struct mm_struct *mm = current->mm;
    if (!mm) {
        printk(KERN_ERR "[STRESS TEST] current->mm is NULL\n");
        return -EINVAL;
    }
    mmgrab(mm);
    ret = run_stress_test(cache, addr, mm);
    mmdrop(mm);
    return ret;
#endif

    pages = kmalloc_array(TOTAL_TEST_PAGES, sizeof(struct page *), GFP_KERNEL);
    if (!pages) {
        printk(KERN_ERR "[TEST] Failed to allocate pages array\n");
        return -ENOMEM;
    }

    if (!current->mm) {
        printk(KERN_ERR "[TEST] current->mm is NULL\n");
        kfree(pages);
        return -EINVAL;
    }

    down_read(&current->mm->mmap_lock);
    vma = find_vma(current->mm, addr);
    up_read(&current->mm->mmap_lock);
    if (!vma) {
        printk(KERN_ERR "[TEST] No VMA found for address %px\n", (void *)addr);
        kfree(pages);
        return -EINVAL;
    }

    printk(KERN_INFO "=== TEST: Remote NUMA page lifecycle with eviction ===\n");

    struct vm_area_struct fake_vma = *vma;
    memset(&fake_vmf, 0, sizeof(fake_vmf));
    *((struct vm_area_struct **)&fake_vmf.vma) = &fake_vma;

    // Allocate and fill each page with a unique pattern
    for (int i = 0; i < TOTAL_TEST_PAGES; i++) {
        *((unsigned long *)&fake_vmf.address) = addr + (i * PAGE_SIZE);

        int retry_count = 0;
        const int max_retries = 256;
        while (retry_count < max_retries) {
            /* Simulate atomic context as in kernel page fault handler */
            preempt_disable();
            pages[i] = remote_numa_client_cache_alloc(cache, &fake_vmf);
            preempt_enable();
            if (IS_ERR(pages[i])) {
                if (PTR_ERR(pages[i]) == -EAGAIN) {
                    printk(KERN_INFO "[TEST] Allocation %d: transfer in progress, retrying (attempt %d)...\n",
                           i, retry_count + 1);
                    msleep(5); /* Brief delay before retry */
                    retry_count++;
                    continue;
                } else {
                    printk(KERN_ERR "[TEST] Allocation %d failed with error %ld\n", i, PTR_ERR(pages[i]));
                    kfree(pages);
                    return PTR_ERR(pages[i]);
                }
            } else if (!pages[i]) {
                printk(KERN_ERR "[TEST] Allocation %d failed\n", i);
                kfree(pages);
                return -ENOMEM;
            }
            break;
        }

        if (retry_count >= max_retries) {
            printk(KERN_ERR "[TEST] Allocation %d timed out after %d retries\n", i, max_retries);
            kfree(pages);
            return -ETIMEDOUT;
        }

        printk(KERN_INFO "[TEST] Allocation %d succeeded\n", i);

        void *kaddr = kmap_local_page(pages[i]);
        for (int j = 0; j < PAGE_SIZE; j++) {
            ((unsigned char *)kaddr)[j] = (unsigned char)(i ^ j);
        }
        kunmap_local(kaddr);
    }

    printk(KERN_INFO "[TEST] All %d pages allocated successfully\n", TOTAL_TEST_PAGES);
    printk(KERN_INFO "[TEST] Pages should have been evicted to make room for later pages\n");

    // Refault and verify all TOTAL_REFAULTS evicted pages
    bool all_valid = true;
    for (int i = 0; i < TOTAL_REFAULTS; i++) {
        printk(KERN_INFO "[TEST] Attempting to refault evicted page %d...\n", i);
        *((unsigned long *)&fake_vmf.address) = addr + (i * PAGE_SIZE);
        
        int retry_count = 0;
        const int max_retries = 256;
        while (retry_count < max_retries) {
            /* Simulate atomic context as in kernel page fault handler */
            preempt_disable();
            ret = remote_numa_client_cache_refault(cache, pages[i], &fake_vmf);
            preempt_enable();
            if (ret == -EAGAIN) {
                printk(KERN_INFO "[TEST] Refault %d: transfer in progress, retrying (attempt %d)...\n",
                       i, retry_count + 1);
                msleep(5);
                retry_count++;
                continue;
            }
            break;
        }

        if (retry_count >= max_retries) {
            printk(KERN_ERR "[TEST] Refault %d timed out after %d retries\n", i, max_retries);
            kfree(pages);
            return -ETIMEDOUT;
        }

        if (ret) {
            printk(KERN_ERR "[TEST] Refault %d failed with error %d\n", i, ret);
            kfree(pages);
            return ret;
        }

        printk(KERN_INFO "[TEST] Refault %d succeeded\n", i);

        // Verify the refaulted page data (do NOT overwrite)
        void *kaddr = kmap_local_page(pages[i]);
        bool valid = true;
        for (int j = 0; j < PAGE_SIZE; j++) {
            unsigned char expected = (unsigned char)(i ^ j);
            if (((unsigned char *)kaddr)[j] != expected) {
                valid = false;
                printk(KERN_ERR "[TEST] Page %d data corrupted at offset %d: expected %u, got %u\n",
                       i, j, expected, ((unsigned char *)kaddr)[j]);
                break;
            }
        }
        kunmap_local(kaddr);

        if (valid)
            printk(KERN_INFO "[TEST] Page %d data verified successfully\n", i);
        else {
            printk(KERN_ERR "[TEST] Page %d data verification FAILED\n", i);
            all_valid = false;
        }
    }

    printk(KERN_INFO "=== TEST COMPLETE ===\n");

    // Clean up all allocated pages to avoid filling the cache across test runs
    printk(KERN_INFO "[TEST] Freeing all %d allocated pages...\n", TOTAL_TEST_PAGES);
    for (int i = 0; i < TOTAL_TEST_PAGES; i++) {
        /*
		 * Simulate atomic context as in kernel page fault handler.
		 * If a free races an in-flight transfer, reject (-EAGAIN) and retry.
		 */
		for (int tries = 0; tries < 256; tries++) {
			preempt_disable();
			ret = remote_numa_client_cache_free_page(cache, pages[i]);
			preempt_enable();
			if (ret != -EAGAIN)
				break;
			msleep(1);
		}
        if (ret && ret != -ENOENT) {
            printk(KERN_WARNING "[TEST] Failed to free page %d: error %d\n", i, ret);
        }
    }
    printk(KERN_INFO "[TEST] Cleanup complete\n");

    kfree(pages);
    return all_valid ? 0 : -EIO;
}

static long remote_numa_test_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    if (cmd == REMOTE_NUMA_RUN_TEST) {
        unsigned long end_addr = arg + (TOTAL_TEST_PAGES * PAGE_SIZE);
        
        /* Validate userspace address range */
        if (!access_ok((void __user *)arg, TOTAL_TEST_PAGES * PAGE_SIZE)) {
            printk(KERN_ERR "[TEST] Invalid userspace address range\n");
            return -EFAULT;
        }
        
        /* Check for overflow */
        if (end_addr < arg) {
            printk(KERN_ERR "[TEST] Address range overflow\n");
            return -EINVAL;
        }
        
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

