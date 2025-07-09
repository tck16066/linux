#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/rmap.h>
#include <linux/hashtable.h>
#include "client_cache.h"
#include <linux/rcupdate.h>
#include "transport.h"

/* assumes lock is held */
static remote_numa_node_t *choose_donor(remote_numa_client_cache_t *cache)
{
	remote_numa_node_t *node;
	int bkt;

	hash_for_each(cache->trprt->trprt_ctx->node_table, bkt, node, hnode) {
		if (node->valid_mem_resp && node->free_pages > 0) {
			node->free_pages--;
			break;
		}
	}

	return node;
}


#define REFAULT_HASH_BITS 10
DEFINE_HASHTABLE(refault_table, REFAULT_HASH_BITS);

struct refault_entry {
	struct mm_struct *mm;
	unsigned long addr;
	u64 donor_pg_cookie;
	u32 donor_id;
	struct hlist_node node;
};

static u32 refault_hash_key(struct mm_struct *mm, unsigned long addr)
{
	return hash_long(((unsigned long)mm >> 4) ^ addr, REFAULT_HASH_BITS);
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
	hash_add(refault_table, &entry->node, refault_hash_key(mm, addr));
}

static struct refault_entry*
refault_table_lookup(struct mm_struct *mm, unsigned long addr)
{
	struct refault_entry *entry;
	u32 key = refault_hash_key(mm, addr);

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
	if (cache->current_cached_pages >= cache->max_cached_pages) {
		remote_numa_cached_page_t *victim;

		if (list_empty(&cache->lru_head))
			return -ENOMEM;

		victim = list_last_entry(&cache->lru_head,
			remote_numa_cached_page_t, lru_list);
		list_del(&victim->lru_list);
		cache->current_cached_pages--;

		if (victim->mm && victim->addr) {
			struct vm_area_struct *vma;
			down_read(&victim->mm->mmap_lock);
			vma = find_vma(victim->mm, victim->addr);
			if (vma && victim->addr >= vma->vm_start) {
				zap_page_range_single(vma, victim->addr, PAGE_SIZE, NULL);
				refault_table_insert(victim->mm, victim->addr,
						     victim->donor_pg_cookie,
						     victim->donor_id);
			}
			up_read(&victim->mm->mmap_lock);
		}

		remote_numa_tx_mem_pg_sync_xfer(cache->trprt,
						victim->donor_pg_cookie,
						victim->page,
						victim);

		list_add(&victim->lru_list, &cache->free_list);
	}
	return 0;
}

static remote_numa_cached_page_t *reuse_page(remote_numa_client_cache_t *cache)
{
	remote_numa_cached_page_t *entry = NULL;

	if (!list_empty(&cache->free_list)) {
		entry = list_first_entry(&cache->free_list,
				 remote_numa_cached_page_t, lru_list);
		list_del(&entry->lru_list);
	}
	return entry;
}

static void cache_insert(remote_numa_client_cache_t *cache,
			 remote_numa_cached_page_t *entry)
{
	spin_lock(&cache->lock);
	list_add(&entry->lru_list, &cache->lru_head);
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
	init_waitqueue_head(&cache->waitq);
	cache->max_cached_pages = max_cached_pages;
	cache->current_cached_pages = 0;
	cache->trprt = trprt;

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
		list_del(pos);
		__free_page(entry->page);
		kfree(entry);
	}
	list_for_each_safe(pos, tmp, &cache->free_list) {
		entry = list_entry(pos, remote_numa_cached_page_t, lru_list);
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
	spin_lock(&cache->lock);
	if (maybe_evict(cache))
		goto err;
		

	remote_numa_cached_page_t *entry = reuse_page(cache);
	if (!entry)
		goto err;

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
	entry->mm = vmf->vma->vm_mm;
	entry->addr = vmf->address;

	spin_unlock(&cache->lock);

	if (remote_numa_transport_alloc_page_rcu(cache->trprt,
					     donor,
					     entry->page, entry)) {
		spin_lock(&cache->lock);
		list_add(&entry->lru_list, &cache->free_list);
		goto err;
	}

	if (need_rcu_unlock && took_rcu_lock)
		rcu_read_unlock();

	cache_insert(cache, entry);
	return entry->page;
err:
	spin_unlock(&cache->lock);
	if (need_rcu_unlock && took_rcu_lock)
		rcu_read_unlock();
	return NULL;
}

int remote_numa_client_cache_refault(remote_numa_client_cache_t *cache,
				     struct page *faulting_page,
				     struct vm_fault *vmf)
{
	if (maybe_evict(cache))
		return -ENOMEM;

	struct mm_struct *mm = current->mm;
	unsigned long addr = (unsigned long)page_address(faulting_page);

	spin_lock(&cache->lock);
	struct refault_entry *rentry = refault_table_lookup(mm, addr);
	u64 donor_pg_cookie = rentry->donor_pg_cookie;
	u32 donor_id = rentry->donor_id;

	remote_numa_cached_page_t *entry = reuse_page(cache);
	spin_unlock(&cache->lock);

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
						faulting_page,
						NULL)) { // XXX NULL
		spin_lock(&cache->lock);
		list_add(&entry->lru_list, &cache->free_list);
		spin_unlock(&cache->lock);
		return -EIO;
	}

	cache_insert(cache, entry);
	return 0;
}

