/*
 * Functions related to io context handling
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/bitmap.h>
#include <linux/blkdev.h>
#include <linux/bootmem.h>	/* for max_pfn/max_low_pfn */
#include <linux/slab.h>

#include "blk.h"

/*
 * For io context allocations
 */
static struct kmem_cache *iocontext_cachep;

static void hlist_sched_dtor(struct io_context *ioc, struct hlist_head *list)
{
	if (!hlist_empty(list)) {
		struct cfq_io_context *cic;

		cic = hlist_entry(list->first, struct cfq_io_context, cic_list);
		cic->dtor(ioc);
	}
}

/**
 * put_io_context - put a reference of io_context
 * @ioc: io_context to put
 *
 * Decrement reference count of @ioc and release it if the count reaches
 * zero.
 */
void put_io_context(struct io_context *ioc)
{
	if (ioc == NULL)
		return;

	BUG_ON(atomic_long_read(&ioc->refcount) <= 0);

	if (!atomic_long_dec_and_test(&ioc->refcount))
		return;

	rcu_read_lock();
	hlist_sched_dtor(ioc, &ioc->cic_list);
	hlist_sched_dtor(ioc, &ioc->bfq_cic_list);
	rcu_read_unlock();

	kmem_cache_free(iocontext_cachep, ioc);
}
EXPORT_SYMBOL(put_io_context);

static void hlist_sched_exit(struct io_context *ioc, struct hlist_head *list)
{
	rcu_read_lock();

	if (!hlist_empty(list)) {
		struct cfq_io_context *cic;

		cic = hlist_entry(list->first, struct cfq_io_context, cic_list);
		cic->exit(ioc);
	}
	rcu_read_unlock();
}

/* Called by the exiting task */
void exit_io_context(struct task_struct *task)
{
	struct io_context *ioc;

	task_lock(task);
	ioc = task->io_context;
	task->io_context = NULL;
	task_unlock(task);

	if (atomic_dec_and_test(&ioc->nr_tasks)) {
		hlist_sched_exit(ioc, &ioc->cic_list);
		hlist_sched_exit(ioc, &ioc->bfq_cic_list);
	}
	put_io_context(ioc);
}

struct io_context *alloc_io_context(gfp_t gfp_flags, int node)
{
	struct io_context *ioc;

	ioc = kmem_cache_alloc_node(iocontext_cachep, gfp_flags | __GFP_ZERO,
				    node);
	if (unlikely(!ioc))
		return NULL;

	/* initialize */
	atomic_long_set(&ioc->refcount, 1);
	atomic_set(&ioc->nr_tasks, 1);
	spin_lock_init(&ioc->lock);
	bitmap_zero(ioc->ioprio_changed, IOC_IOPRIO_CHANGED_BITS);
	INIT_RADIX_TREE(&ioc->bfq_radix_root, GFP_ATOMIC | __GFP_HIGH);
	INIT_HLIST_HEAD(&ioc->bfq_cic_list);

	return ioc;
}

/**
 * current_io_context - get io_context of %current
 * @gfp_flags: allocation flags, used if allocation is necessary
 * @node: allocation node, used if allocation is necessary
 *
 * Return io_context of %current.  If it doesn't exist, it is created with
 * @gfp_flags and @node.  The returned io_context does NOT have its
 * reference count incremented.  Because io_context is exited only on task
 * exit, %current can be sure that the returned io_context is valid and
 * alive as long as it is executing.
 */
struct io_context *current_io_context(gfp_t gfp_flags, int node)
{
	struct task_struct *tsk = current;
	struct io_context *ret;

	ret = tsk->io_context;
	if (likely(ret))
		return ret;

	ret = alloc_io_context(gfp_flags, node);
	if (ret) {
		/* make sure set_task_ioprio() sees the settings above */
		smp_wmb();
		tsk->io_context = ret;
	}

	return ret;
}

/*
 * If the current task has no IO context then create one and initialise it.
 * If it does have a context, take a ref on it.
 *
 * This is always called in the context of the task which submitted the I/O.
 */
struct io_context *get_io_context(gfp_t gfp_flags, int node)
{
	struct io_context *ioc = NULL;

	/*
	 * Check for unlikely race with exiting task. ioc ref count is
	 * zero when ioc is being detached.
	 */
	do {
		ioc = current_io_context(gfp_flags, node);
		if (unlikely(!ioc))
			break;
	} while (!atomic_long_inc_not_zero(&ioc->refcount));

	return ioc;
}
EXPORT_SYMBOL(get_io_context);

static int __init blk_ioc_init(void)
{
	iocontext_cachep = kmem_cache_create("blkdev_ioc",
			sizeof(struct io_context), 0, SLAB_PANIC, NULL);
	return 0;
}
#ifdef CONFIG_FAST_RESUME
beforeresume_initcall(blk_ioc_init);
#else
subsys_initcall(blk_ioc_init);
#endif
