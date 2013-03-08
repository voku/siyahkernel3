/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_score_adj values will get killed. Specify
 * the minimum oom_score_adj values in
 * /sys/module/lowmemorykiller/parameters/adj and the number of free pages in
 * /sys/module/lowmemorykiller/parameters/minfree. Both files take a comma
 * separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill
 * processes with a oom_score_adj value of 8 or higher when the free memory
 * drops below 4096 pages and kill processes with a oom_score_adj value of 0 or
 * higher when the free memory drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/rcupdate.h>
#include <linux/notifier.h>
#include <linux/swap.h>
#include <linux/slab.h>
#include <linux/string.h>
#ifdef CONFIG_LMK_SCREEN_STATE
#include <linux/earlysuspend.h>
#endif

static uint32_t lowmem_debug_level = 1;
static short lowmem_adj[6] = {
	0,
	1,
	6,
	12,
	16,
	17,
};
static int lowmem_adj_size = 6;
static int lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	5 * 1024,	/* 20MB */
	8 * 1024,	/* 32MB */
	16 * 1024,	/* 64MB */
};
#ifdef CONFIG_LMK_SCREEN_STATE
static int lowmem_minfree_screen_off[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	5 * 1024,	/* 20MB */
	8 * 1024,	/* 32MB */
	16 * 1024,	/* 64MB */
};
static int lowmem_minfree_screen_on[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	5 * 1024,	/* 20MB */
	8 * 1024,	/* 32MB */
	16 * 1024,	/* 64MB */
};
#endif
static int lowmem_minfree_size = 6;
#ifdef CONFIG_KILL_ONCE_IF_SCREEN_OFF
static bool screen_off = false;
static unsigned int *uids = NULL;
static unsigned int max_alloc = 0;
static unsigned int counter = 0;
#endif
static unsigned long lowmem_deathpending_timeout;

#ifdef CONFIG_KILL_ONCE_IF_SCREEN_OFF
#define ALLOC_SIZE 32
#endif

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			printk(x);			\
	} while (0)

static int lowmem_shrink(struct shrinker *s, struct shrink_control *sc)
{
	struct task_struct *tsk;
#ifdef CONFIG_ENHANCED_LMK_ROUTINE
	struct task_struct *selected[CONFIG_LOWMEM_DEATHPENDING_DEPTH] = {NULL,};
#else
	struct task_struct *selected = NULL;
#endif
#ifdef CONFIG_KILL_ONCE_IF_SCREEN_OFF
	const struct cred *cred = current_cred(), *pcred;
	unsigned int uid = 0;
#endif
	int rem = 0;
	int tasksize;
	int i;
	short min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	int target_free = 0;
#ifdef CONFIG_ENHANCED_LMK_ROUTINE
	int selected_tasksize[CONFIG_LOWMEM_DEATHPENDING_DEPTH] = {0,};
	short selected_oom_score_adj[CONFIG_LOWMEM_DEATHPENDING_DEPTH] = {OOM_ADJUST_MAX,};
	int selected_target_offset[CONFIG_LOWMEM_DEATHPENDING_DEPTH] = {OOM_ADJUST_MAX,};
	int all_selected_oom = 0;
	int max_selected_oom_idx = 0;
#else
	int selected_tasksize = 0;
	short selected_oom_score_adj;
	int selected_target_offset;
#endif
	int target_offset = 0;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free = global_page_state(NR_FREE_PAGES) - totalreserve_pages;
	int other_file = global_page_state(NR_FILE_PAGES) - global_page_state(NR_SHMEM);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;

	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;

	for (i = 0; i < array_size; i++) {
#if defined(CONFIG_VMWARE_MVP)
		if (other_file < lowmem_minfree[i]) {
#else
		if (other_free < lowmem_minfree[i] && other_file < lowmem_minfree[i]) {
#endif
			min_score_adj = lowmem_adj[i];
			target_free = lowmem_minfree[i] - (other_free + other_file);
			break;
		}
	}

	if (sc->nr_to_scan > 0)
		lowmem_print(3, "lowmemkill: lowmem_shrink %lu, %x, ofree %d %d, ma %hd\n",
			     sc->nr_to_scan, sc->gfp_mask, other_free, other_file, min_score_adj);

	rem = global_page_state(NR_ACTIVE_ANON) +
		global_page_state(NR_ACTIVE_FILE) +
		global_page_state(NR_INACTIVE_ANON) +
		global_page_state(NR_INACTIVE_FILE);

	if (sc->nr_to_scan <= 0 || min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
		lowmem_print(5, "lowmemkill: lowmem_shrink %lu, %x, return %d\n",
			     sc->nr_to_scan, sc->gfp_mask, rem);

		return rem;
	}

#ifdef CONFIG_ENHANCED_LMK_ROUTINE
	for (i = 0; i < CONFIG_LOWMEM_DEATHPENDING_DEPTH; i++)
		selected_oom_score_adj[i] = min_score_adj;
#else
	selected_oom_score_adj = min_score_adj;
#endif

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		short oom_score_adj;
#ifdef CONFIG_ENHANCED_LMK_ROUTINE
		int is_exist_oom_task = 0;
#endif
#ifdef CONFIG_KILL_ONCE_IF_SCREEN_OFF
		bool uid_test = false;
#endif

		if (tsk->flags & PF_KTHREAD)
			continue;

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		if (test_tsk_thread_flag(p, TIF_MEMDIE) &&
		    time_before_eq(jiffies, lowmem_deathpending_timeout)) {
			task_unlock(p);
			rcu_read_unlock();

			return 0;
		}

#ifdef CONFIG_LMK_APP_PROTECTION
		if (	strcmp(p->comm, "d.process.acore") == 0 ||
				strcmp(p->comm, "d.process.media") == 0 ||
				strcmp(p->comm, "putmethod.latin") == 0 ||
				strcmp(p->comm, "ainfire.supersu") == 0
		) {
			lowmem_print(2, "lowmemkill: protected %s, screen_off %d\n",
						 p->comm, screen_off);

			task_unlock(p);
			continue;
		}
#endif

		oom_score_adj = p->signal->oom_score_adj;
		if (oom_score_adj < min_score_adj) {
			task_unlock(p);
			continue;
		}

#ifdef CONFIG_KILL_ONCE_IF_SCREEN_OFF
		pcred = __task_cred(p);
		uid = pcred->uid;

		if (screen_off == true) {
			for (i = 0; i < counter; i++) {
				if (uids[i] == uid) {
					uid_test = true;
				}
			}
			if (uid_test == true) {
				uid_test = false;
				lowmem_print(1, "lowmemkill: skiped %d (%s), adj %hd, uid %d, screen_off %d\n",
			    		     p->pid, p->comm, oom_score_adj, uid, screen_off);
				task_unlock(p);
				continue;
			}
		}
		else {
			if (uids != NULL) {
				lowmem_print(3, "lowmemkill: free memory from uids: %d\n",
							 counter);
				kfree(uids);
				uids = NULL;
				max_alloc = 0;
				counter = 0;
			}
		}
#endif

		tasksize = get_mm_rss(p->mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;

#ifdef CONFIG_ENHANCED_LMK_ROUTINE
		if (all_selected_oom < CONFIG_LOWMEM_DEATHPENDING_DEPTH) {
			for (i = 0; i < CONFIG_LOWMEM_DEATHPENDING_DEPTH; i++) {
				if (!selected[i]) {
					is_exist_oom_task = 1;
					max_selected_oom_idx = i;
					target_offset = abs(target_free - tasksize);
					break;
				}
			}
		} else if (selected_oom_score_adj[max_selected_oom_idx] < oom_score_adj ||
					  (selected_oom_score_adj[max_selected_oom_idx] == oom_score_adj &&
					  target_offset >= selected_target_offset[max_selected_oom_idx])) {
			is_exist_oom_task = 1;
		}

		if (is_exist_oom_task) {
			selected[max_selected_oom_idx] = p;
			selected_tasksize[max_selected_oom_idx] = tasksize;
			selected_oom_score_adj[max_selected_oom_idx] = oom_score_adj;

			if (all_selected_oom < CONFIG_LOWMEM_DEATHPENDING_DEPTH)
				all_selected_oom++;

			if (all_selected_oom == CONFIG_LOWMEM_DEATHPENDING_DEPTH) {
				for (i = 0; i < CONFIG_LOWMEM_DEATHPENDING_DEPTH; i++) {
					if (selected_oom_score_adj[i] < selected_oom_score_adj[max_selected_oom_idx])
						max_selected_oom_idx = i;
					else if (selected_oom_score_adj[i] == selected_oom_score_adj[max_selected_oom_idx] &&
  						  selected_tasksize[i] < selected_tasksize[max_selected_oom_idx])
						max_selected_oom_idx = i;
				}
			}

			selected_target_offset[max_selected_oom_idx] = target_offset;

			lowmem_print(2, "lowmemkill: select %d (%s), adj %d, size %d, screen_off %d, to kill\n", 
					 p->pid, p->comm, oom_score_adj, tasksize, screen_off);
		}
#else
		target_offset = abs(target_free - tasksize);
		if (selected) {
			if (oom_score_adj < selected_oom_score_adj)
				continue;
			if (oom_score_adj == selected_oom_score_adj && target_offset >= selected_target_offset)
				continue;
		}
		selected = p;
		selected_tasksize = tasksize;
		selected_target_offset = target_offset;
		selected_oom_score_adj = oom_score_adj;
#ifdef CONFIG_KILL_ONCE_IF_SCREEN_OFF
		lowmem_print(2, "lowmemkill: select %d (%s), adj %hd, size %d, uid %d, screen_off %d, to kill\n",
			     p->pid, p->comm, oom_score_adj, tasksize, uid, screen_off);
#else
#ifdef CONFIG_LMK_SCREEN_STATE
		lowmem_print(2, "lowmemkill: select %d (%s), adj %hd, size %d, screen_off %d, to kill\n",
			     p->pid, p->comm, oom_score_adj, tasksize, screen_off);                             	
#else
		lowmem_print(2, "lowmemkill: select %d (%s), adj %hd, size %d, to kill\n",
			     p->pid, p->comm, oom_score_adj, tasksize);
#endif /* CONFIG_LMK_SCREEN_STATE */                         	
#endif /* CONFIG_KILL_ONCE_IF_SCREEN_OFF */
#endif /* CONFIG_ENHANCED_LMK_ROUTINE */
	}
#ifdef CONFIG_ENHANCED_LMK_ROUTINE
	for (i = 0; i < CONFIG_LOWMEM_DEATHPENDING_DEPTH; i++) {
		if (selected[i]) {

#ifdef CONFIG_KILL_ONCE_IF_SCREEN_OFF
			pcred = __task_cred(selected[i]);
			uid = pcred->uid;

			if (screen_off == true) {
				if (counter >= max_alloc) {
					max_alloc += ALLOC_SIZE;
				}
				uids = (unsigned int *)krealloc(uids, max_alloc*sizeof(unsigned int), GFP_KERNEL);
				if (uids == NULL) {
					goto no_mem;
				}
				uids[counter++] = uid;
				lowmem_print(2, "lowmemkill: skip next time for %s, uid %d, screen_off %d\n",
					     selected[i]->comm, uid, screen_off);
			}
no_mem:
#endif /* CONFIG_KILL_ONCE_IF_SCREEN_OFF */
#ifdef CONFIG_LMK_SCREEN_STATE
			lowmem_print(1, "lowmemkill: send sigkill to %d (%s), adj %hd, size %d screen_off %d\n",
					selected[i]->pid, selected[i]->comm,
					selected_oom_score_adj[i], selected_tasksize[i],
					screen_off);
#else
			lowmem_print(1, "lowmemkill: send sigkill to %d (%s), adj %hd, size %d\n",
					selected[i]->pid, selected[i]->comm,
					selected_oom_score_adj[i], selected_tasksize[i]);
#endif
			lowmem_deathpending_timeout = jiffies + 100;
			force_sig(SIGKILL, selected[i]);
			set_tsk_thread_flag(selected[i], TIF_MEMDIE);
			rem -= selected_tasksize[i];
		}
	}
#else
	if (selected) {
#ifdef CONFIG_KILL_ONCE_IF_SCREEN_OFF
		pcred = __task_cred(selected);
		uid = pcred->uid;

		if (screen_off == true) {
			if (counter >= max_alloc) {
				max_alloc += ALLOC_SIZE;
			}
			uids = (unsigned int *)krealloc(uids, max_alloc*sizeof(unsigned int), GFP_KERNEL);
			if (uids == NULL) {
				goto no_mem;
			}
			uids[counter++] = uid;
			lowmem_print(2, "lowmemkill: skip next time for %s, uid %d, screen_off %d\n",
		     	     selected->comm, uid, screen_off);
		}
no_mem:

		lowmem_print(1, "lowmemkill: send sigkill to %d (%s), adj %hd, size %d, uid %d, screen_off %d\n",
			     selected->pid, selected->comm, selected_oom_score_adj, selected_tasksize, uid, screen_off);
#else
#ifdef CONFIG_LMK_SCREEN_STATE
		lowmem_print(1, "lowmemkill: send sigkill to %d (%s), adj %hd, size %d, screen_off %d\n",
			     selected->pid, selected->comm, selected_oom_score_adj, selected_tasksize, screen_off);
#else
		lowmem_print(1, "lowmemkill: send sigkill to %d (%s), adj %hd, size %d\n",
			     selected->pid, selected->comm, selected_oom_score_adj, selected_tasksize);
#endif /* CONFIG_LMK_SCREEN_STATE */
#endif /* CONFIG_KILL_ONCE_IF_SCREEN_OFF */
		lowmem_deathpending_timeout = jiffies + 100;
		force_sig(SIGKILL, selected);
		set_tsk_thread_flag(selected, TIF_MEMDIE);
		rem -= selected_tasksize;
	}
#endif /* CONFIG_ENHANCED_LMK_ROUTINE */
	lowmem_print(4, "lowmemkill: lowmem_shrink %lu, %x, return %d\n",
		     sc->nr_to_scan, sc->gfp_mask, rem);
	rcu_read_unlock();

	return rem;
}

static struct shrinker lowmem_shrinker = {
	.shrink = lowmem_shrink,
	.seeks = DEFAULT_SEEKS * 16
};

#ifdef CONFIG_LMK_SCREEN_STATE
static void low_mem_early_suspend(struct early_suspend *handler)
{
	memcpy(lowmem_minfree_screen_on, lowmem_minfree, sizeof(lowmem_minfree));
	memcpy(lowmem_minfree, lowmem_minfree_screen_off, sizeof(lowmem_minfree_screen_off));

	screen_off = true;
}

static void low_mem_late_resume(struct early_suspend *handler)
{
	memcpy(lowmem_minfree, lowmem_minfree_screen_on, sizeof(lowmem_minfree_screen_on));

	screen_off = false;
}

static struct early_suspend low_mem_suspend = {
	.suspend = low_mem_early_suspend,
	.resume = low_mem_late_resume,
};
#endif

static int __init lowmem_init(void)
{
#ifdef CONFIG_LMK_SCREEN_STATE
	register_early_suspend(&low_mem_suspend);
#endif
	register_shrinker(&lowmem_shrinker);

	return 0;
}

static void __exit lowmem_exit(void)
{
	unregister_shrinker(&lowmem_shrinker);
}

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
static int lowmem_oom_adj_to_oom_score_adj(int oom_adj)
{
	if (oom_adj == OOM_ADJUST_MAX)
		return OOM_SCORE_ADJ_MAX;
	else
		return (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;
}

static void lowmem_autodetect_oom_adj_values(void)
{
	int i;
	int oom_adj;
	short oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;

	if (array_size <= 0)
		return;

	oom_adj = lowmem_adj[array_size - 1];
	if (oom_adj > OOM_ADJUST_MAX)
		return;

	oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
	if (oom_score_adj <= OOM_ADJUST_MAX)
		return;

	lowmem_print(1, "lowmemkill: lowmem_shrink => convert oom_adj to oom_score_adj:\n");
	for (i = 0; i < array_size; i++) {
		oom_adj = lowmem_adj[i];
		oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
		lowmem_adj[i] = oom_score_adj;
		lowmem_print(1, "lowmemkill: oom_adj %d => oom_score_adj %d\n",
			     oom_adj, oom_score_adj);
	}
}

static int lowmem_adj_array_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_array_ops.set(val, kp);

	/* HACK: Autodetect oom_adj values in lowmem_adj array */
	lowmem_autodetect_oom_adj_values();

	return ret;
}

static int lowmem_adj_array_get(char *buffer, const struct kernel_param *kp)
{
	return param_array_ops.get(buffer, kp);
}

static void lowmem_adj_array_free(void *arg)
{
	param_array_ops.free(arg);
}

static struct kernel_param_ops lowmem_adj_array_ops = {
	.set = lowmem_adj_array_set,
	.get = lowmem_adj_array_get,
	.free = lowmem_adj_array_free,
};

static const struct kparam_array __param_arr_adj = {
	.max = ARRAY_SIZE(lowmem_adj),
	.num = &lowmem_adj_size,
	.ops = &param_ops_int,
	.elemsize = sizeof(lowmem_adj[0]),
	.elem = lowmem_adj,
};
#endif

module_param_named(cost, lowmem_shrinker.seeks, int, S_IRUGO | S_IWUSR);
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
__module_param_call(MODULE_PARAM_PREFIX, adj,
		    &lowmem_adj_array_ops,
		    .arr = &__param_arr_adj,
		    S_IRUGO | S_IWUSR, 0);
__MODULE_PARM_TYPE(adj, "array of int");
#else
module_param_array_named(adj, lowmem_adj, short, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);
#endif
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
#ifdef CONFIG_LMK_SCREEN_STATE
module_param_array_named(minfree_screen_off, lowmem_minfree_screen_off, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
#endif
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");
