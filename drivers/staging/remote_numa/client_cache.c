#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/rmap.h>
#include <linux/hashtable.h>
#include <linux/page-flags.h>
#include <linux/delay.h>
#include <linux/rcupdate.h>
#include <linux/sched/mm.h>

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

hash_for_each(refault_table, bkt, entry, node) {
}

	hash_for_each_possible(refault_table, entry, node, key) {
		if (entry->mm == mm && entry->addr == addr)
			return entry;
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
	spin_lock(&cache->lock);
	if (cache->current_cached_pages >= cache->max_cached_pages) {
		remote_numa_cached_page_t *victim;
		if (list_empty(&cache->lru_head))
		{
			spin_unlock(&cache->lock);
			return -ENOMEM;
		}
		victim = list_last_entry(&cache->lru_head,
			remote_numa_cached_page_t, lru_list);
		list_del(&victim->lru_list);
		hash_del(&victim->node);
		cache->current_cached_pages--;
		spin_unlock(&cache->lock);

bool do_zap = victim->mm && victim->addr;
printk("start sync client\n");
		// XXX return code. we are timing out now and don't see it.
		ret = remote_numa_tx_mem_pg_sync_xfer(cache->trprt,
						victim->donor_pg_cookie,
						victim->page,
						victim);
printk("done sync client\n\n");
		if (do_zap) {
			struct vm_area_struct *vma;
			mmap_write_lock(victim->mm);
			vma = find_vma(victim->mm, victim->addr);
			if (vma && victim->addr >= vma->vm_start &&
				victim->addr < vma->vm_end) {

				zap_page_range_single(vma, victim->addr, PAGE_SIZE, NULL);
#if 0
printk("insert cookie into refault  %llu\n", victim->donor_pg_cookie);
				refault_table_insert(victim->mm, victim->addr,
						     victim->donor_pg_cookie,
						     victim->donor_id);
#endif
			}
			mmap_write_unlock(victim->mm);
		}


spin_lock(&cache->lock);
		list_add(&victim->lru_list, &cache->free_list);
	}
	spin_unlock(&cache->lock);
	return ret;
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

int remote_numa_client_cache_init(remote_numa_client_cache_t *cache,
				  remote_numa_main_trprt_if_t *trprt,
				  u32 max_cached_pages)
{
	INIT_LIST_HEAD(&cache->lru_head);
	INIT_LIST_HEAD(&cache->free_list);
	spin_lock_init(&cache->lock);
	cache->max_cached_pages = max_cached_pages;
	cache->current_cached_pages = 0;
	cache->trprt = trprt;

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

		list_add(&entry->lru_list, &cache->free_list);
	}
	return 0;
}

void remote_numa_client_cache_destroy(remote_numa_client_cache_t *cache)
{
	struct list_head *pos, *tmp;
	remote_numa_cached_page_t *entry;

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
	if (maybe_evict(cache))
		goto err;
		

	remote_numa_cached_page_t *entry = reuse_page(cache);
	if (!entry)
	{
		printk(KERN_DEBUG "reuse_page() fail.\n");
		goto err;
	}

	/* XXX handle OOM on individual and all donors. Data race. */
	bool need_rcu_unlock = !rcu_read_lock_held();
	bool took_rcu_lock = false;
	if (need_rcu_unlock)
	{
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


	if (remote_numa_transport_alloc_page_rcu(cache->trprt,
					     donor,
					     entry)) {
		printk(KERN_DEBUG "Call to " \
			"remote_numa_transport_alloc_page_rcu failed.\n");
		list_add(&entry->lru_list, &cache->free_list);
		goto err;
	}

	cache_insert(cache, entry);
	return entry->page;
err:
	printk(KERN_DEBUG "Failure to allocate remote page.\n");
	return NULL;
}

int remote_numa_client_cache_refault(remote_numa_client_cache_t *cache,
				     struct page *faulting_page,
				     struct vm_fault *vmf)
{
	if (maybe_evict(cache))
		return -ENOMEM;
	struct mm_struct *mm = vmf->vma->vm_mm;
	unsigned long addr = vmf->address;

	struct refault_entry *rentry = refault_table_lookup(mm, addr);
	u64 donor_pg_cookie = rentry->donor_pg_cookie;
	u32 donor_id = rentry->donor_id;
	remote_numa_cached_page_t *entry = reuse_page(cache);

	if (!entry)
		return -ENOMEM;

	entry->page = faulting_page;
	entry->donor_pg_cookie = donor_pg_cookie;
	entry->donor_id = donor_id;
	entry->mm = vmf->vma->vm_mm;
	entry->addr = vmf->address;

	if (remote_numa_transport_refetch_page(cache->trprt,
						donor_id,
						donor_pg_cookie,
						entry)) { // XXX NULL
		list_add(&entry->lru_list, &cache->free_list);
		return -EIO;
	}

	cache_insert(cache, entry);

	return 0;
}

int remote_numa_client_cache_free_page(remote_numa_client_cache_t *cache,
				       struct page *page)
{
	remote_numa_cached_page_t *entry;

	spin_lock(&cache->lock);
	hash_for_each_possible(cache->page_lookup, entry, node, (uintptr_t)page) {
		if (entry->page == page) {
			list_del(&entry->lru_list);
			cache->current_cached_pages--;

			hash_del(&entry->node);  // Remove from hash

			remote_numa_tx_mem_pg_free(cache->trprt,
				entry->donor_id,
				entry->donor_pg_cookie,
				(uintptr_t)entry);
			list_add(&entry->lru_list, &cache->free_list);
			spin_unlock(&cache->lock);
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

