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

#define REMOTE_NUMA_REFAULT_HASH_BITS 12

struct remote_numa_pg_free_work {
	struct work_struct work;
	remote_numa_main_trprt_if_t *trprt;
	u32 donor_id;
	u64 donor_pg_cookie;
	uintptr_t main_pg_cookie;
};

static void remote_numa_pg_free_workfn(struct work_struct *work)
{
	struct remote_numa_pg_free_work *w =
		container_of(work, struct remote_numa_pg_free_work, work);
	/*
	 * Do the blocking remote free call in worker context.
	 * We cannot do much about an error. We can leak, or the donor
	 * can leak. We would rather the donor leak, because we have
	 * a smaller number of pages locally (in the cache).
	 * 
	 * Note that it is preferable to follow the existing model of how async works
	 * with the other functions where the EAGAIN is passed up to the caller,
	 * for two reasons. Firstly, consistency. Secondly, it makes the caller handle
	 * back-pressure rather than stacking it up silently. This hacky
	 * approach is quicker for now.
	 */
	int ret = remote_numa_tx_mem_pg_free(w->trprt, w->donor_id, w->donor_pg_cookie, w->main_pg_cookie);
	if (ret) {
		 printk(KERN_WARNING "remote_numa: Error freeing remote page: ret=%d, donor_id=%u, donor_pg_cookie=%llu, main_pg_cookie=0x%lx\n",
			 ret, w->donor_id, w->donor_pg_cookie, (unsigned long)w->main_pg_cookie);
	}
	kfree(w);
}

static void remote_numa_tx_mem_pg_free_async(remote_numa_main_trprt_if_t *trprt,
											 u32 donor_id,
											 u64 donor_pg_cookie,
											 uintptr_t main_pg_cookie)
{
	struct remote_numa_pg_free_work *w =
		kmalloc(sizeof(*w), GFP_ATOMIC);
	if (!w) {
		/* fallback: leak or warn */
		WARN_ON_ONCE(1);
		return;
	}
	INIT_WORK(&w->work, remote_numa_pg_free_workfn);
	w->trprt = trprt;
	w->donor_id = donor_id;
	w->donor_pg_cookie = donor_pg_cookie;
	w->main_pg_cookie = main_pg_cookie;
	queue_work(system_unbound_wq, &w->work);
}

/* Hash helpers for known_pages: key is (__mm, __addr) */
static inline u64 known_page_hash(struct page *page) {
	return hash_long((unsigned long)page, REMOTE_NUMA_CLIENT_CACHE_HASH_BITS);
}

static inline void known_pages_insert(remote_numa_client_cache_t *cache, remote_numa_known_page_t *kp, struct page *page, struct mm_struct *mm, unsigned long addr) {
	kp->page = page;
	kp->mm = mm;
	kp->addr = addr;
	hash_add(cache->known_pages, &kp->node, known_page_hash(page));
}

static inline remote_numa_known_page_t *known_pages_lookup(remote_numa_client_cache_t *cache, struct page *page) {
	remote_numa_known_page_t *kp;
	u64 key = known_page_hash(page);
	hash_for_each_possible(cache->known_pages, kp, node, key) {
		  if (kp->page == page)
			    return kp;
	}
	return NULL;
}

static inline void known_pages_remove(remote_numa_client_cache_t *cache, struct page *page) {
	remote_numa_known_page_t *kp = known_pages_lookup(cache, page);
	if (kp)
		  hash_del(&kp->node);
}

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
static DEFINE_SPINLOCK(refault_table_lock);

struct mm_drop_work {
    struct work_struct work;
    struct mm_struct *mm;
};

static void mm_drop_workfn(struct work_struct *work)
{
    struct mm_drop_work *w =
        container_of(work, struct mm_drop_work, work);

    mmdrop(w->mm);
    kfree(w);
}

/* atomic context */
static void defer_mmdrop(struct mm_struct *mm)
{
    struct mm_drop_work *w;

    w = kmalloc(sizeof(*w), GFP_ATOMIC);
    if (!w) {
        /* last-ditch fallback: leak or WARN */
        WARN_ON_ONCE(1);
        return;
    }

    INIT_WORK(&w->work, mm_drop_workfn);
    w->mm = mm;

    queue_work(system_unbound_wq, &w->work);
}

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
	spin_lock(&refault_table_lock);
	hash_add(refault_table, &entry->node, key);
	spin_unlock(&refault_table_lock);
}

static bool refault_table_peek(struct mm_struct *mm, unsigned long addr)
{
	struct refault_entry *entry;
	u32 key = refault_hash_key(mm, addr);
	bool found = false;

	spin_lock(&refault_table_lock);
	hash_for_each_possible(refault_table, entry, node, key) {
		if (entry->mm == mm && entry->addr == addr) {
			found = true;
			break;
		}
	}
	spin_unlock(&refault_table_lock);
	return found;
}

static bool refault_table_consume(struct mm_struct *mm, unsigned long addr,
				 u64 *donor_pg_cookie, u32 *donor_id)
{
	struct refault_entry *entry;
	u32 key = refault_hash_key(mm, addr);

	spin_lock(&refault_table_lock);
	hash_for_each_possible(refault_table, entry, node, key) {
		if (entry->mm == mm && entry->addr == addr) {
			*donor_pg_cookie = entry->donor_pg_cookie;
			*donor_id = entry->donor_id;
			hash_del(&entry->node);
			spin_unlock(&refault_table_lock);
			kfree(entry);
			return true;
		}
	}
	spin_unlock(&refault_table_lock);
	return false;
}

static void refault_table_clear(void)
{
	int bkt;
	struct refault_entry *entry;
	struct hlist_node *tmp;
	spin_lock(&refault_table_lock);
	hash_for_each_safe(refault_table, bkt, tmp, entry, node) {
		hash_del(&entry->node);
		kfree(entry);
	}
	spin_unlock(&refault_table_lock);
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
	ret = remote_numa_tx_mem_pg_sync_xfer_async(cache->trprt,
					victim->known_page->donor_pg_cookie,
					victim->known_page->page,
					victim);
	if (ret != 0) {
		/* Put it back and clear flag */
		atomic_set(&victim->transfer_in_progress, 0);
		spin_lock(&cache->lock);
		list_add(&victim->lru_list, &cache->lru_head);
		/* No need to increment current_cached_pages - we never decremented it */
		hash_add(cache->page_lookup, &victim->node, (uintptr_t)victim->known_page->page);
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
			       /*
				* Entry is now reserved/in-use (even before it is inserted into
				* the LRU/hash). Count it so capacity checks remain consistent.
				*/
			       cache->current_cached_pages++;
			       /* Do NOT remove known_page here. It must persist until explicit free. */
		       }
		       spin_unlock(&cache->lock);
		       return entry;
}

static void cache_insert(remote_numa_client_cache_t *cache,
			 remote_numa_cached_page_t *entry)
{
	spin_lock(&cache->lock);
	list_add(&entry->lru_list, &cache->lru_head);
	hash_add(cache->page_lookup, &entry->node, (uintptr_t)entry->known_page->page);
	spin_unlock(&cache->lock);
}

/* Background worker to complete async evictions */
static void eviction_completion_worker(struct work_struct *work)
{
	remote_numa_client_cache_t *cache = container_of(work, 
		remote_numa_client_cache_t, eviction_completion_work.work);
	remote_numa_cached_page_t *victim;
	bool need_reschedule = false;
	remote_numa_cached_page_t *pass_start = NULL;
	bool made_progress = false;
	int iters = 0;
	const int max_iters = 1024;

	for (;;) {
		int status;

		if (iters++ >= max_iters) {
			need_reschedule = true;
			break;
		}

		spin_lock(&cache->lock);
		if (list_empty(&cache->evicting_list)) {
			spin_unlock(&cache->lock);
			break;
		}
		victim = list_first_entry(&cache->evicting_list,
					 remote_numa_cached_page_t, lru_list);
		list_del_init(&victim->lru_list);
		spin_unlock(&cache->lock);

		if (!pass_start)
			pass_start = victim;

		/* Check if transfer is complete */
		status = remote_numa_check_transfer_complete(victim);
		if (status == -EAGAIN) {
			/* Still in progress: rotate and try others */
			need_reschedule = true;
			spin_lock(&cache->lock);
			list_add_tail(&victim->lru_list, &cache->evicting_list);
			spin_unlock(&cache->lock);
			/*
			 * If we made a full pass without completing anything, stop
			 * and let the delayed work run again later. Otherwise we can
			 * spin forever and trigger the watchdog.
			 */
			if (victim == pass_start && !made_progress)
				break;
			cond_resched();
			continue;
		}
		made_progress = true;
		pass_start = NULL;

		/* Transfer complete or error - do final cleanup */
		if (status == 0 && victim->known_page && victim->known_page->mm && victim->known_page->addr) {
			struct vm_area_struct *vma;
			mmap_write_lock(victim->known_page->mm);
			vma = find_vma(victim->known_page->mm, victim->known_page->addr);
			if (vma && victim->known_page->addr >= vma->vm_start &&
			    victim->known_page->addr < vma->vm_end) {
				zap_page_range_single(vma, victim->known_page->addr, PAGE_SIZE, NULL);
				refault_table_insert(victim->known_page->mm, victim->known_page->addr,
						   victim->known_page->donor_pg_cookie,
						   victim->known_page->donor_id);
			}
			mmap_write_unlock(victim->known_page->mm);
		}

		victim->known_page = NULL;

		/* Clear transfer flag and move to free list */
		atomic_set(&victim->transfer_in_progress, 0);
		spin_lock(&cache->lock);
		list_add(&victim->lru_list, &cache->free_list);
		cache->current_cached_pages--;
		spin_unlock(&cache->lock);
		cond_resched();
	}

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
	hash_init(cache->known_pages);
	hash_init(refault_table);

	for (u32 i = 0; i < max_cached_pages; ++i) {
		remote_numa_cached_page_t *entry = kzalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			return -ENOMEM;
		}
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
		list_del(pos);
		hash_del(&entry->node);
		__free_page(entry->known_page->page);
		kfree(entry->known_page);
		       if (entry->known_page && entry->known_page->mm) {
			       mmdrop(entry->known_page->mm);
		       }
		kfree(entry);
	}
	list_for_each_safe(pos, tmp, &cache->free_list) {
		entry = list_entry(pos, remote_numa_cached_page_t, lru_list);
		list_del(pos);
		__free_page(entry->known_page->page);
		kfree(entry->known_page);
		       if (entry->known_page && entry->known_page->mm) {
			       mmdrop(entry->known_page->mm);
		       }
		kfree(entry);
	}
	       cache->current_cached_pages = 0;

	       /* Clear known_pages hash table */
		       int bkt;
		       remote_numa_known_page_t *kp;
		       struct hlist_node *tmp2;
		       hash_for_each_safe(cache->known_pages, bkt, tmp2, kp, node) {
			       hash_del(&kp->node);
			       kfree(kp);
		       }

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
		       remote_numa_known_page_t *kp = existing->known_page;
		       if (kp && kp->mm == vmf->vma->vm_mm && kp->addr == vmf->address) {
			       /* Fast path: already complete */
			       if (atomic_read(&existing->transfer_in_progress) == 0) {
				       spin_unlock(&cache->lock);
				       return kp->page;
			       }

			       /* In progress: check transfer completion */
			       spin_unlock(&cache->lock);
			       int status = remote_numa_check_transfer_complete(existing);
			       if (status == -EAGAIN) {
				       /* Still in progress */
				       return ERR_PTR(-EAGAIN);
			       } else if (status == 0) {
				       /* Transfer complete */
				       atomic_set(&existing->transfer_in_progress, 0);
				       return kp->page;
			       }

			       /* Missing xfer state or other error: don't create a duplicate entry. */
			       return ERR_PTR(-EAGAIN);
		       }
	       }
	spin_unlock(&cache->lock);

	int evict_ret = maybe_evict(cache);

	if (evict_ret == -EAGAIN) {
		/* Eviction in progress; caller should retry. */
		return ERR_PTR(-EAGAIN);
	} else if (evict_ret) {
		goto err;
	}

	remote_numa_cached_page_t *entry = reuse_page(cache);
	if (!entry) {
		/*
		 * Under contention, another thread may have consumed the last free entry
		 * after our eviction check. Try to kick eviction once more and ask the
		 * caller to retry rather than failing the allocation.
		 */
		evict_ret = maybe_evict(cache);
		if (evict_ret == -EAGAIN || evict_ret == 0)
			return ERR_PTR(-EAGAIN);
		if (evict_ret)
			goto err;

		       unsigned int free_len = 0, lru_len = 0, lru_busy = 0, evict_len = 0;
		       remote_numa_cached_page_t *it;
		       u32 cur_cached, max_cached;
		       spin_lock(&cache->lock);
		       cur_cached = cache->current_cached_pages;
		       max_cached = cache->max_cached_pages;
		       list_for_each_entry(it, &cache->free_list, lru_list)
			       free_len++;
		       list_for_each_entry(it, &cache->lru_head, lru_list) {
			       lru_len++;
			       if (atomic_read(&it->transfer_in_progress))
				       lru_busy++;
		       }
		       list_for_each_entry(it, &cache->evicting_list, lru_list)
			       evict_len++;
		       spin_unlock(&cache->lock);
		       printk_ratelimited(KERN_DEBUG "reuse_page() fail: cur_cached=%u max=%u free=%u lru=%u lru_busy=%u evicting=%u\n",
			       cur_cached, max_cached, free_len, lru_len, lru_busy, evict_len);
		       return ERR_PTR(-EAGAIN);
	       }

	       /* Always allocate a new known_page for this (mm, addr) */
		       struct page *page = alloc_page(GFP_KERNEL);
		       if (!page) {
			       spin_lock(&cache->lock);
			       list_add(&entry->lru_list, &cache->free_list);
			       cache->current_cached_pages--;
			       spin_unlock(&cache->lock);
			       return ERR_PTR(-ENOMEM);
		       }
		       struct remote_numa_known_page *kp = kzalloc(sizeof(*kp), GFP_KERNEL);
		       if (!kp) {
			       __free_page(page);
			       spin_lock(&cache->lock);
			       list_add(&entry->lru_list, &cache->free_list);
			       cache->current_cached_pages--;
			       spin_unlock(&cache->lock);
			       goto err;
		       }
		       kp->donor_pg_cookie = 0;
		       kp->donor_id = 0;
		       kp->donor_cookie = 0;
		       known_pages_insert(cache, kp, page, vmf->vma->vm_mm, vmf->address);
		       entry->known_page = kp;

	       /* Initialize transfer flag - this entry is now being allocated */
	       atomic_set(&entry->transfer_in_progress, 1);

	       bool need_rcu_unlock = !rcu_read_lock_held();
	       bool took_rcu_lock = false;
	       if (need_rcu_unlock) {
		       rcu_read_lock();
		       took_rcu_lock = true;
	       }

	       remote_numa_node_t *donor = choose_donor(cache);

	       entry->known_page->donor_pg_cookie = 0;
	       entry->known_page->donor_id = donor->node_id;
	       entry->known_page->donor_cookie = donor->donor_cookie;
		mmgrab(kp->mm);

		       if (need_rcu_unlock && took_rcu_lock) {
			       rcu_read_unlock();
		       }

		       if (remote_numa_transport_alloc_page_async(cache->trprt,
						    donor,
						    entry)) {
			       printk(KERN_DEBUG "Call to remote_numa_transport_alloc_page_async failed.\n");
			       atomic_set(&entry->transfer_in_progress, 0);
			       spin_lock(&cache->lock);
			       list_add(&entry->lru_list, &cache->free_list);
			       cache->current_cached_pages--;
			       spin_unlock(&cache->lock);
			       known_pages_remove(cache, kp->page);
			       kfree(kp);
			       goto err;
		       }

	       cache_insert(cache, entry);
	       return ERR_PTR(-EAGAIN);
err:
	printk(KERN_DEBUG "Failure to allocate remote page.\n");
	return ERR_PTR(-ENOMEM);
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
			   if (existing->known_page && existing->known_page->mm == mm && existing->known_page->addr == addr) {
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

	u64 donor_pg_cookie;
	u32 donor_id;
	if (!refault_table_consume(mm, addr, &donor_pg_cookie, &donor_id))
		return -ENOENT;

	remote_numa_cached_page_t *entry = reuse_page(cache);
	if (!entry) {
		/* Cache full under contention; try to evict and retry. */
		evict_ret = maybe_evict(cache);
		if (evict_ret == -EAGAIN || evict_ret == 0)
			return -EAGAIN;
		return -ENOMEM;
	}

	       /* Look up known_page for this (mm, addr) */
		       remote_numa_known_page_t *kp = known_pages_lookup(cache, faulting_page);
		       if (!kp) {
			       /*
				* Should never happen, but under concurrency we must not leak
				* entries from free_list.
				*/
			       atomic_set(&entry->transfer_in_progress, 0);
			       spin_lock(&cache->lock);
			       list_add(&entry->lru_list, &cache->free_list);
			       cache->current_cached_pages--;
			       spin_unlock(&cache->lock);
			       return -ENOENT;
		       }
		       entry->known_page = kp;

	       /* Mark this entry as having a transfer in progress */
	       atomic_set(&entry->transfer_in_progress, 1);

	       entry->known_page->page = faulting_page;
	       /* donor_pg_cookie and donor_id are already set in kp */
		mmgrab(kp->mm);  /* Hold ref - eviction will zap PTE asynchronously */

	       /* Initiate async refetch - returns immediately */
	       if (remote_numa_transport_refetch_page_async(cache->trprt,
					       donor_id,
					       donor_pg_cookie,
					       entry)) {
		       atomic_set(&entry->transfer_in_progress, 0);
		       spin_lock(&cache->lock);
		       list_add(&entry->lru_list, &cache->free_list);
		       cache->current_cached_pages--;
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
	u32 donor_id;
	u64 donor_pg_cookie;
	uintptr_t main_pg_cookie;

	       /* Remove from known_pages and free known_page and its page. Drive found/not found by this. */
		       remote_numa_known_page_t *kp = known_pages_lookup(cache, page);
		       if (!kp) {
			       printk(KERN_WARNING "No entry found for free.");
			       return -ENOENT;
		       }
		       donor_id = kp->donor_id;
		       donor_pg_cookie = kp->donor_pg_cookie;
		       spin_lock(&cache->lock);
		       hash_del(&kp->node);
		       /* Remove from cache table if present */
		       remote_numa_cached_page_t *entry = NULL;
		       hash_for_each_possible(cache->page_lookup, entry, node, (uintptr_t)page) {
			       if (entry->known_page && entry->known_page->page == page) {
				       list_del(&entry->lru_list);
				       cache->current_cached_pages--;
				       hash_del(&entry->node);
				       /* Drop mm reference before moving to free list */
				       if (kp->mm) {
					       defer_mmdrop(kp->mm);
					       kp->mm = NULL;
				       }
				       /* Add to free list immediately; remote free will complete in background. */
				       list_add(&entry->lru_list, &cache->free_list);
			       }
		       }
		       __free_page(kp->page);
		       main_pg_cookie = (uintptr_t)kp;
		       kfree(kp);
		       spin_unlock(&cache->lock);

	       /* Async remote free: schedule work to do the remote free in process context. */
	       remote_numa_tx_mem_pg_free_async(cache->trprt,
		       donor_id,
		       donor_pg_cookie,
		       main_pg_cookie);

	       return 0;
}

bool remote_numa_client_cache_refault_pending(struct mm_struct *mm,
					     unsigned long addr)
{
	return refault_table_peek(mm, addr);
}

EXPORT_SYMBOL_GPL(remote_numa_client_cache_init);
EXPORT_SYMBOL_GPL(remote_numa_client_cache_alloc);
EXPORT_SYMBOL_GPL(remote_numa_client_cache_refault);
EXPORT_SYMBOL_GPL(remote_numa_client_cache_destroy);
EXPORT_SYMBOL_GPL(remote_numa_client_cache_free_page);
EXPORT_SYMBOL_GPL(remote_numa_client_cache_refault_pending);

