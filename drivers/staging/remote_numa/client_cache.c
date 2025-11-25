#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/rmap.h>
#include <linux/hashtable.h>
#include <linux/page-flags.h>
#include <linux/delay.h>
#include <linux/rcupdate.h>
#include <linux/sched/mm.h>
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/workqueue.h>

#include "client_cache.h"
#include "transport.h"

#define REMOTE_NUMA_REFAULT_HASH_BITS 10

/* assumes lock is held */
static remote_numa_node_t *choose_donor(remote_numa_client_cache_t *cache)
{
	remote_numa_node_t *node;
	int bkt;

	rcu_read_lock();
	hash_for_each(cache->trprt->trprt_ctx->node_table, bkt, node, hnode) {
		if (node->valid_mem_resp && node->free_pages > 0) {
			node->free_pages--;
			break;
		}
	}
	rcu_read_unlock();
	return node;
}


DEFINE_HASHTABLE(refault_table, REMOTE_NUMA_REFAULT_HASH_BITS);

struct refault_entry {
	struct mm_struct *mm;
	unsigned long addr;
	u64 donor_pg_cookie;
	u32 donor_id;
	struct hlist_node node;
};

static u32 refault_hash_key(struct mm_struct *mm, unsigned long addr)
{
	return hash_long(((unsigned long)mm >> 4) ^ addr, REMOTE_NUMA_REFAULT_HASH_BITS);
}

static void refault_table_insert(struct mm_struct *mm, unsigned long addr,
				  u64 donor_pg_cookie, u32 donor_id)
{
	struct refault_entry *entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return;
	entry->mm = mm;
	entry->addr = addr;
	entry->donor_pg_cookie = donor_pg_cookie;
	entry->donor_id = donor_id;
	u32 key = refault_hash_key(mm, addr);
	hash_add(refault_table, &entry->node, key);
}

static struct refault_entry*
refault_table_lookup(struct mm_struct *mm, unsigned long addr)
{

	struct refault_entry *entry;
	u32 key = refault_hash_key(mm, addr);


int bkt;

printk("dummpit\n");
	hash_for_each_possible(refault_table, entry, node, key) {
		if (entry->mm == mm && entry->addr == addr)
			return entry;
		printk("mm  %px  entry addr  %llu\n", entry->mm, entry->addr);
	}
	return NULL;
}

static void refault_table_clear(void)
{
	int bkt;
	struct refault_entry *entry;
	struct hlist_node *tmp;

	hash_for_each_safe(refault_table, bkt, tmp, entry, node) {
		hash_del(&entry->node);
		kfree(entry);
	}
}

static int maybe_evict(remote_numa_client_cache_t *cache)
{
	int ret = 0;
	remote_numa_cached_page_t *victim;

	spin_lock(&cache->lock);
	if (cache->current_cached_pages < cache->max_cached_pages) {
		spin_unlock(&cache->lock);
		return 0;
	}

	if (list_empty(&cache->lru_head)) {
		spin_unlock(&cache->lock);
		return -ENOMEM;
	}

	/* Find a victim that's not currently being transferred */
	list_for_each_entry(victim, &cache->lru_head, lru_list) {
		if (atomic_read(&victim->transfer_in_progress) == 0) {
			/* Try to claim it for eviction */
			if (atomic_cmpxchg(&victim->transfer_in_progress, 0, 1) == 0) {
				/* Successfully claimed - remove from LRU */
				list_del(&victim->lru_list);
				hash_del(&victim->node);
				/* Don't decrement current_cached_pages yet - page isn't free */
				spin_unlock(&cache->lock);
				goto evict_victim;
			}
		}
	}

	/* No victim available - all pages have transfers in progress */
	spin_unlock(&cache->lock);
	return -EAGAIN;

evict_victim:
	/* 
	 * We're in atomic context (called from page fault), so we can't block.
	 * Just initiate the async transfer and leave the page marked as in-progress.
	 * The transfer flag will prevent this page from being used or evicted again
	 * until the background retry worker completes and cleans it up.
	 */
	printk("start eviction sync (async)\n");
	ret = remote_numa_tx_mem_pg_sync_xfer_async(cache->trprt,
					victim->donor_pg_cookie,
					victim->page,
					victim);
	if (ret != 0) {
		printk("sync xfer async initiation failed\n");
		/* Put it back and clear flag */
		atomic_set(&victim->transfer_in_progress, 0);
		spin_lock(&cache->lock);
		list_add(&victim->lru_list, &cache->lru_head);
		/* No need to increment current_cached_pages - we never decremented it */
		hash_add(cache->page_lookup, &victim->node, (uintptr_t)victim->page);
		spin_unlock(&cache->lock);
		return ret;
	}

	/* 
	 * Transfer initiated - move to evicting list so background worker can complete it.
	 * The page stays marked as in-progress.
	 */
	spin_lock(&cache->lock);
	list_add(&victim->lru_list, &cache->evicting_list);
	spin_unlock(&cache->lock);
	
	/* 
	 * Schedule worker if not already pending. Use short delay since network
	 * transfers typically complete in 5-20ms. If already running/pending, don't
	 * reset the timer - let it process all queued evictions when it runs.
	 */
	if (!delayed_work_pending(&cache->eviction_completion_work)) {
		schedule_delayed_work(&cache->eviction_completion_work, msecs_to_jiffies(10));
	}
	
	printk("eviction transfer initiated\n");
	/* Return -EAGAIN to indicate eviction is in progress, caller should retry */
	return -EAGAIN;
}

static remote_numa_cached_page_t *reuse_page(remote_numa_client_cache_t *cache)
{
	remote_numa_cached_page_t *entry = NULL;

	spin_lock(&cache->lock);
	if (!list_empty(&cache->free_list)) {
		entry = list_first_entry(&cache->free_list,
				 remote_numa_cached_page_t, lru_list);
		list_del(&entry->lru_list);
	}
	spin_unlock(&cache->lock);
	return entry;
}

static void cache_insert(remote_numa_client_cache_t *cache,
			 remote_numa_cached_page_t *entry)
{
	spin_lock(&cache->lock);
	list_add(&entry->lru_list, &cache->lru_head);
	hash_add(cache->page_lookup, &entry->node, (uintptr_t)entry->page);
	cache->current_cached_pages++;
	spin_unlock(&cache->lock);
}

/* Background worker to complete async evictions */
static void eviction_completion_worker(struct work_struct *work)
{
	remote_numa_client_cache_t *cache = container_of(work, 
		remote_numa_client_cache_t, eviction_completion_work.work);
	remote_numa_cached_page_t *victim, *tmp;
	bool need_reschedule = false;

	spin_lock(&cache->lock);
	list_for_each_entry_safe(victim, tmp, &cache->evicting_list, lru_list) {
		spin_unlock(&cache->lock);
		
		/* Check if transfer is complete */
		int status = remote_numa_check_transfer_complete(victim);
		if (status == -EAGAIN) {
			/* Still in progress */
			need_reschedule = true;
			spin_lock(&cache->lock);
			continue;
		}
		
		/* Transfer complete or error - do final cleanup */
		bool do_zap = victim->mm && victim->addr;
		if (do_zap && status == 0) {
			struct vm_area_struct *vma;
			mmap_write_lock(victim->mm);
			vma = find_vma(victim->mm, victim->addr);
			if (vma && victim->addr >= vma->vm_start &&
				victim->addr < vma->vm_end) {
				zap_page_range_single(vma, victim->addr, PAGE_SIZE, NULL);
				printk("insert cookie into refault  %llu\\n", victim->donor_pg_cookie);
				refault_table_insert(victim->mm, victim->addr,
						     victim->donor_pg_cookie,
						     victim->donor_id);
			}
			mmap_write_unlock(victim->mm);
		}

		/* Clear transfer flag and move to free list */
		atomic_set(&victim->transfer_in_progress, 0);
		spin_lock(&cache->lock);
		list_del(&victim->lru_list);
		list_add(&victim->lru_list, &cache->free_list);
		/* Now the page is truly free - decrement count */
		cache->current_cached_pages--;
	}
	spin_unlock(&cache->lock);

	/* Reschedule if there are still pending evictions - check again in 10ms */
	if (need_reschedule)
		schedule_delayed_work(&cache->eviction_completion_work, msecs_to_jiffies(10));
}

int remote_numa_client_cache_init(remote_numa_client_cache_t *cache,
				  remote_numa_main_trprt_if_t *trprt,
				  u32 max_cached_pages)
{
	INIT_LIST_HEAD(&cache->lru_head);
	INIT_LIST_HEAD(&cache->free_list);
	INIT_LIST_HEAD(&cache->evicting_list);
	spin_lock_init(&cache->lock);
	cache->max_cached_pages = max_cached_pages;
	cache->current_cached_pages = 0;
	cache->trprt = trprt;
	INIT_DELAYED_WORK(&cache->eviction_completion_work, eviction_completion_worker);

	hash_init(cache->page_lookup);
	hash_init(refault_table);

	for (u32 i = 0; i < max_cached_pages; ++i) {
		struct page *page = alloc_page(GFP_KERNEL);
		if (!page)
			return -ENOMEM;

		remote_numa_cached_page_t *entry = kzalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			__free_page(page);
			return -ENOMEM;
		}
		entry->page = page;
		entry->donor_pg_cookie = 0;
		atomic_set(&entry->transfer_in_progress, 0);

		list_add(&entry->lru_list, &cache->free_list);
	}
	return 0;
}

void remote_numa_client_cache_destroy(remote_numa_client_cache_t *cache)
{
	struct list_head *pos, *tmp;
	remote_numa_cached_page_t *entry;

	/* Cancel background worker */
	cancel_delayed_work_sync(&cache->eviction_completion_work);

	list_for_each_safe(pos, tmp, &cache->lru_head) {
		entry = list_entry(pos, remote_numa_cached_page_t, lru_list);
		mmdrop(entry->mm);
		list_del(pos);
		hash_del(&entry->node);
		__free_page(entry->page);
		kfree(entry);
	}
	list_for_each_safe(pos, tmp, &cache->free_list) {
		entry = list_entry(pos, remote_numa_cached_page_t, lru_list);
		mmdrop(entry->mm);
		list_del(pos);
		__free_page(entry->page);
		kfree(entry);
	}
	cache->current_cached_pages = 0;
	refault_table_clear();
}

struct page *remote_numa_client_cache_alloc(remote_numa_client_cache_t *cache,
	struct vm_fault *vmf)
{
	remote_numa_cached_page_t *existing;
	int bkt;
	
	/* Check if there's already an ongoing allocation for this address */
	spin_lock(&cache->lock);
	hash_for_each(cache->page_lookup, bkt, existing, node) {
		if (existing->mm == vmf->vma->vm_mm && existing->addr == vmf->address) {
			/* Found existing entry - check if transfer is complete */
			spin_unlock(&cache->lock);
			int status = remote_numa_check_transfer_complete(existing);
			if (status == -EAGAIN) {
				/* Still in progress */
				return ERR_PTR(-EAGAIN);
			} else if (status == 0) {
				/* Transfer complete */
				atomic_set(&existing->transfer_in_progress, 0);
				return existing->page;
			}
			/* Error case - fall through to retry */
			break;
		}
	}
	spin_unlock(&cache->lock);

	int evict_ret = maybe_evict(cache);
	if (evict_ret == -EAGAIN) {
		/* Eviction blocked by in-progress transfer, tell caller to retry */
		return ERR_PTR(-EAGAIN);
	} else if (evict_ret) {
		goto err;
	}

	remote_numa_cached_page_t *entry = reuse_page(cache);
	if (!entry) {
		printk(KERN_DEBUG "reuse_page() fail.\n");
		goto err;
	}

	/* Initialize transfer flag - this entry is now being allocated */
	atomic_set(&entry->transfer_in_progress, 1);

	/* XXX handle OOM on individual and all donors. Data race. */
	bool need_rcu_unlock = !rcu_read_lock_held();
	bool took_rcu_lock = false;
	if (need_rcu_unlock) {
		rcu_read_lock();
		took_rcu_lock = true;
	}

	remote_numa_node_t *donor = choose_donor(cache);

	entry->donor_pg_cookie = 0;
	entry->donor_id = donor->node_id;
	entry->donor_cookie = donor->donor_cookie;
	entry->mm = vmf->vma->vm_mm;
	mmgrab(entry->mm);
	entry->addr = vmf->address;

	if (need_rcu_unlock && took_rcu_lock)
		rcu_read_unlock();

	/* Initiate async transfer - returns immediately */
	if (remote_numa_transport_alloc_page_async(cache->trprt,
					     donor,
					     entry)) {
		printk(KERN_DEBUG "Call to " \
			"remote_numa_transport_alloc_page_async failed.\n");
		atomic_set(&entry->transfer_in_progress, 0);
		spin_lock(&cache->lock);
		list_add(&entry->lru_list, &cache->free_list);
		spin_unlock(&cache->lock);
		goto err;
	}

	/* Transfer initiated successfully - insert into cache */
	cache_insert(cache, entry);
	
	/* Don't clear transfer_in_progress yet - it will be cleared when complete */
	/* Return EAGAIN - caller should retry, transfer will complete async */
	return ERR_PTR(-EAGAIN);
err:
	printk(KERN_DEBUG "Failure to allocate remote page.\n");
	return NULL;
}

int here = 0;

int remote_numa_client_cache_refault(remote_numa_client_cache_t *cache,
				     struct page *faulting_page,
				     struct vm_fault *vmf)
{
	struct mm_struct *mm = vmf->vma->vm_mm;
	unsigned long addr = vmf->address;
	remote_numa_cached_page_t *existing;
	int bkt;

	/* Check if there's already an ongoing refault for this address */
	spin_lock(&cache->lock);
	hash_for_each(cache->page_lookup, bkt, existing, node) {
		if (existing->mm == mm && existing->addr == addr) {
			/* Found existing entry - check if transfer is complete */
			spin_unlock(&cache->lock);
			/* Check if transfer is complete */
			if (remote_numa_transport_is_transfer_complete(existing)) {
				atomic_set(&existing->transfer_in_progress, 0);
				return 0; /* Transfer complete */
			} else if (atomic_read(&existing->transfer_in_progress)) {
				/* Still in progress */
				return -EAGAIN;
			}
			/* Error case - fall through to retry */
			break;
		}
	}
	spin_unlock(&cache->lock);

	int evict_ret = maybe_evict(cache);
	if (evict_ret == -EAGAIN) {
		/* Eviction blocked by in-progress transfer, tell caller to retry */
		return -EAGAIN;
	} else if (evict_ret) {
		return -ENOMEM;
	}

	struct refault_entry *rentry = refault_table_lookup(mm, addr);
	if (!rentry)
		return -ENOENT;

	u64 donor_pg_cookie = rentry->donor_pg_cookie;
	u32 donor_id = rentry->donor_id;
	remote_numa_cached_page_t *entry = reuse_page(cache);

	if (!entry)
		return -ENOMEM;

	/* Mark this entry as having a transfer in progress */
	atomic_set(&entry->transfer_in_progress, 1);

	entry->page = faulting_page;
	entry->donor_pg_cookie = donor_pg_cookie;
	entry->donor_id = donor_id;
	entry->mm = vmf->vma->vm_mm;
	entry->addr = vmf->address;

	/* Initiate async refetch - returns immediately */
	if (remote_numa_transport_refetch_page_async(cache->trprt,
						donor_id,
						donor_pg_cookie,
						entry)) {
		atomic_set(&entry->transfer_in_progress, 0);
		spin_lock(&cache->lock);
		list_add(&entry->lru_list, &cache->free_list);
		spin_unlock(&cache->lock);
		return -EIO;
	}

	/* Transfer initiated - insert into cache but keep in-progress flag set */
	cache_insert(cache, entry);

	/* Return EAGAIN - caller must retry and check for completion */
	return -EAGAIN;
}

int remote_numa_client_cache_free_page(remote_numa_client_cache_t *cache,
				       struct page *page)
{
	remote_numa_cached_page_t *entry = NULL;
	u32 donor_id;
	u64 donor_pg_cookie;
	uintptr_t main_pg_cookie;

	spin_lock(&cache->lock);
	hash_for_each_possible(cache->page_lookup, entry, node, (uintptr_t)page) {
		if (entry->page == page) {
			list_del(&entry->lru_list);
			cache->current_cached_pages--;
			hash_del(&entry->node);  // Remove from hash
			
			/* Save info we need for remote free */
			donor_id = entry->donor_id;
			donor_pg_cookie = entry->donor_pg_cookie;
			main_pg_cookie = (uintptr_t)entry;
			
			list_add(&entry->lru_list, &cache->free_list);
			spin_unlock(&cache->lock);
			
			/* Do the blocking remote free call without holding spinlock */
			remote_numa_tx_mem_pg_free(cache->trprt,
				donor_id,
				donor_pg_cookie,
				main_pg_cookie);
			return 0;
		}
	}
	spin_unlock(&cache->lock);
	return -ENOENT;
}

EXPORT_SYMBOL_GPL(remote_numa_client_cache_init);
EXPORT_SYMBOL_GPL(remote_numa_client_cache_alloc);
EXPORT_SYMBOL_GPL(remote_numa_client_cache_refault);
EXPORT_SYMBOL_GPL(remote_numa_client_cache_destroy);
EXPORT_SYMBOL_GPL(remote_numa_client_cache_free_page);

