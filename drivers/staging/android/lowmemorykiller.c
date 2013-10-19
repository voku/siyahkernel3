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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/swap.h>
#include <linux/rcupdate.h>
#include <linux/profile.h>
#include <linux/notifier.h>
#include <linux/earlysuspend.h>

#ifdef CONFIG_ZRAM_FOR_ANDROID
#include <linux/fs.h>
#include <linux/swap.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/mm_inline.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <asm/atomic.h>

#define MIN_FREESWAP_PAGES 8192 /* 32MB */
#define MIN_RECLAIM_PAGES 512  /* 2MB */
#define MIN_CSWAP_INTERVAL (10*HZ)  /* 10 senconds */
#define RTCC_DAEMON_PROC "rtccd"
#define _KCOMPCACHE_DEBUG 0
#if _KCOMPCACHE_DEBUG
#define lss_dbg(x...) printk("lss: " x)
#else
#define lss_dbg(x...)
#endif

struct soft_reclaim {
    unsigned long nr_total_soft_reclaimed;
    unsigned long nr_total_soft_scanned;
    unsigned long nr_last_soft_reclaimed;
    unsigned long nr_last_soft_scanned;
    int nr_empty_reclaimed;

    atomic_t kcompcached_running;
    atomic_t need_to_reclaim;
    atomic_t lmk_running;
    atomic_t kcompcached_enable;
    atomic_t idle_report;
    struct task_struct *kcompcached;
    struct task_struct *rtcc_daemon;
};

static struct soft_reclaim s_reclaim = {
    .nr_total_soft_reclaimed = 0,
    .nr_total_soft_scanned = 0,
    .nr_last_soft_reclaimed = 0,
    .nr_last_soft_scanned = 0,
    .nr_empty_reclaimed = 0,
    .kcompcached = NULL,
};
extern atomic_t kswapd_thread_on;
static unsigned long prev_jiffy;
int hidden_cgroup_counter = 0;
static uint32_t minimum_freeswap_pages = MIN_FREESWAP_PAGES;
static uint32_t minimun_reclaim_pages = MIN_RECLAIM_PAGES;
static uint32_t minimum_interval_time = MIN_CSWAP_INTERVAL;
#endif /* CONFIG_ZRAM_FOR_ANDROID */

static uint32_t lowmem_debug_level = 1;
static uint32_t lowmem_auto_oom = 1;
static short lowmem_adj[6] = {
	0,
	1,
	6,
	12,
	13,
	15,
};
static int lowmem_adj_size = 6;
static int lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
	20 * 1024,	/* 80MB */
	28 * 1024,	/* 112MB */
};
static int lowmem_minfree_screen_off[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
	20 * 1024,	/* 80MB */
	28 * 1024,	/* 112MB */
};
static int lowmem_minfree_screen_on[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
	20 * 1024,	/* 80MB */
	28 * 1024,	/* 112MB */
};
static int lowmem_minfree_size = 6;

static unsigned long lowmem_deathpending_timeout;

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			pr_info(x);			\
	} while (0)

static bool avoid_to_kill(uid_t uid)
{

	if (uid == 0 || /* root */
		uid == 1001 || /* radio */
		uid == 1002 || /* bluetooth */
		uid == 1010 || /* wifi */
		uid == 1012 || /* install */
		uid == 1013 || /* media */
		uid == 1014 || /* dhcp */
		uid == 1017 || /* keystore */
		uid == 1019)	/* drm */
	{
		return 1;
	}
	return 0;
}

static bool protected_apps(char *comm)
{
	if (strcmp(comm, "d.process.acore") == 0 ||
		strcmp(comm, "ndroid.systemui") == 0 ||
		strcmp(comm, "ndroid.contacts") == 0 ||
		strcmp(comm, "d.process.media") == 0) {
		return 1;
	}
	return 0;
}

static unsigned long lowmem_count(struct shrinker *s,
				  struct shrink_control *sc)
{
	return global_page_state(NR_ACTIVE_ANON) +
		global_page_state(NR_ACTIVE_FILE) +
		global_page_state(NR_INACTIVE_ANON) +
		global_page_state(NR_INACTIVE_FILE);
}

static unsigned long lowmem_scan(struct shrinker *s, struct shrink_control *sc)
{
	struct task_struct *tsk;
	struct task_struct *selected = NULL;
	const struct cred *cred = current_cred(), *pcred; 
	unsigned int uid = 0;
	unsigned long rem = 0;
	int tasksize;
	int i;
	short min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	int minfree = 0;
	int selected_tasksize = 0;
	short selected_oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
#ifndef CONFIG_DMA_CMA
/*	int other_free = global_page_state(NR_FREE_PAGES) -
					totalreserve_pages; WILL SOD IF USED! */
	int other_free = global_page_state(NR_FREE_PAGES);
#else
	int other_free = global_page_state(NR_FREE_PAGES) -
					global_page_state(NR_FREE_CMA_PAGES);
#endif
	int other_file = global_page_state(NR_FILE_PAGES) -
						global_page_state(NR_SHMEM);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
		minfree = lowmem_minfree[i];
		if (other_free < minfree && other_file < minfree) {
			min_score_adj = lowmem_adj[i];
			break;
		}
	}

	lowmem_print(3, "lowmem_scan %lu, %x, ofree %d %d, ma %hd\n",
			sc->nr_to_scan, sc->gfp_mask, other_free,
			other_file, min_score_adj);

	if (min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
		lowmem_print(5, "lowmem_scan %lu, %x, return 0\n",
			     sc->nr_to_scan, sc->gfp_mask);
		return 0;
	}

	selected_oom_score_adj = min_score_adj;

#ifdef CONFIG_ZRAM_FOR_ANDROID
	atomic_set(&s_reclaim.lmk_running, 1);
#endif
	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		short oom_score_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		if (test_tsk_thread_flag(p, TIF_MEMDIE) &&
		    time_before_eq(jiffies, lowmem_deathpending_timeout)) {
			task_unlock(p);
			rcu_read_unlock();
			/* give the system time to free up the memory */
			msleep_interruptible(20);
#ifdef CONFIG_ZRAM_FOR_ANDROID
			atomic_set(&s_reclaim.lmk_running, 0);
#endif
			return 0;
		}
		oom_score_adj = p->signal->oom_score_adj;
		if (oom_score_adj < min_score_adj) {
			task_unlock(p);
			continue;
		}
		tasksize = get_mm_rss(p->mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;
		if (selected) {
			if (oom_score_adj < selected_oom_score_adj)
				continue;
			if (oom_score_adj == selected_oom_score_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
		pcred = __task_cred(p);
		uid = pcred->uid;

		if ((!avoid_to_kill(uid) && !protected_apps(p->comm)) ||
				tasksize * (long)(PAGE_SIZE / 1024) >= 80000) {
			selected = p;
			selected_tasksize = tasksize;
			selected_oom_score_adj = oom_score_adj;
			lowmem_print(2, "select '%s' (%d), adj %hd, size %ldkB, to kill\n",
				 p->comm, p->pid, oom_score_adj, tasksize * (long)(PAGE_SIZE / 1024));
		} else {
			lowmem_print(3, "selected skipped '%s' (%d), adj %hd, size %ldkB, no to kill\n",
				 p->comm, p->pid, oom_score_adj, tasksize * (long)(PAGE_SIZE / 1024));
		}
	}
	if (selected) {
		lowmem_print(1, "Killing '%s' (%d), adj %hd,\n" \
				"   to free %ldkB on behalf of '%s' (%d) because\n" \
				"   cache %ldkB is below limit %ldkB for oom_score_adj %hd\n" \
				"   Free memory is %ldkB above reserved\n",
			     selected->comm, selected->pid,
			     selected_oom_score_adj,
			     selected_tasksize * (long)(PAGE_SIZE / 1024),
			     current->comm, current->pid,
			     other_file * (long)(PAGE_SIZE / 1024),
			     minfree * (long)(PAGE_SIZE / 1024),
			     min_score_adj,
			     other_free * (long)(PAGE_SIZE / 1024));
		lowmem_deathpending_timeout = jiffies + HZ;
		do_send_sig_info(SIGKILL, SEND_SIG_FORCED, selected, true);
		set_tsk_thread_flag(selected, TIF_MEMDIE);
		rem += selected_tasksize;
	}

	lowmem_print(4, "lowmem_scan %lu, %x, return %lu\n",
		     sc->nr_to_scan, sc->gfp_mask, rem);
	rcu_read_unlock();
#ifdef CONFIG_ZRAM_FOR_ANDROID
	atomic_set(&s_reclaim.lmk_running, 0);
#endif
	return rem;
}

#ifdef CONFIG_ZRAM_FOR_ANDROID
void could_cswap(void)
{
    if ((hidden_cgroup_counter <= 0) && (atomic_read(&s_reclaim.need_to_reclaim) != 1))
        return;

    if (time_before(jiffies, prev_jiffy + minimum_interval_time))
        return;

    if (atomic_read(&s_reclaim.lmk_running) == 1 || atomic_read(&kswapd_thread_on) == 1)
        return;

    //if (nr_swap_pages < minimum_freeswap_pages)
    //    return;

    if (unlikely(s_reclaim.kcompcached == NULL))
        return;

    if (likely(atomic_read(&s_reclaim.kcompcached_enable) == 0))
        return;

    if (idle_cpu(task_cpu(s_reclaim.kcompcached)) && this_cpu_loadx(4) == 0) {
        if ((atomic_read(&s_reclaim.idle_report) == 1) && (hidden_cgroup_counter > 0)) {
            if(s_reclaim.rtcc_daemon){
                send_sig(SIGUSR1, s_reclaim.rtcc_daemon, 0);
                hidden_cgroup_counter -- ;
                atomic_set(&s_reclaim.idle_report, 0);
                prev_jiffy = jiffies;
                return;
            }
        }

        if (atomic_read(&s_reclaim.need_to_reclaim) != 1) {
            atomic_set(&s_reclaim.idle_report, 1);
            return;
        }

        if (atomic_read(&s_reclaim.kcompcached_running) == 0) {
            wake_up_process(s_reclaim.kcompcached);
            atomic_set(&s_reclaim.kcompcached_running, 1);
            atomic_set(&s_reclaim.idle_report, 1);
            prev_jiffy = jiffies;
        }
    }
}

inline void enable_soft_reclaim(void)
{
    atomic_set(&s_reclaim.kcompcached_enable, 1);
}

inline void disable_soft_reclaim(void)
{
    atomic_set(&s_reclaim.kcompcached_enable, 0);
}

inline void need_soft_reclaim(void)
{
    atomic_set(&s_reclaim.need_to_reclaim, 1);
}

inline void cancel_soft_reclaim(void)
{
    atomic_set(&s_reclaim.need_to_reclaim, 0);
}


int get_soft_reclaim_status(void)
{
    int kcompcache_running = atomic_read(&s_reclaim.kcompcached_running);
    return kcompcache_running;
}

static int soft_reclaim(void)
{
    int nid;
    int i;
    unsigned long nr_soft_reclaimed;
    unsigned long nr_soft_scanned;
    unsigned long nr_reclaimed = 0;

    for_each_node_state(nid, N_HIGH_MEMORY) {
        pg_data_t *pgdat = NODE_DATA(nid);
        for (i = 0; i <= 1; i++) {
            struct zone *zone = pgdat->node_zones + i;
            if (!populated_zone(zone))
                continue;
			// TODO
            //if (!zone_reclaimable(zone))
            //    continue;

            nr_soft_scanned = 0;
            nr_soft_reclaimed = mem_cgroup_soft_limit_reclaim(zone,
                        0, GFP_KERNEL,
                        &nr_soft_scanned);

            s_reclaim.nr_last_soft_reclaimed = nr_soft_reclaimed;
            s_reclaim.nr_last_soft_reclaimed = nr_soft_reclaimed;
            s_reclaim.nr_last_soft_scanned = nr_soft_scanned;
            s_reclaim.nr_total_soft_reclaimed += nr_soft_reclaimed;
            s_reclaim.nr_total_soft_scanned += nr_soft_scanned;
            nr_reclaimed += nr_soft_reclaimed;
        }
    }

    lss_dbg("soft reclaimed %ld pages\n", nr_reclaimed);
    return nr_reclaimed;
}

static int do_compcache(void * nothing)
{
    int ret;
    set_freezable();

    for ( ; ; ) {
        ret = try_to_freeze();
        if (kthread_should_stop())
            break;

        if (soft_reclaim() < minimun_reclaim_pages)
        cancel_soft_reclaim();

        atomic_set(&s_reclaim.kcompcached_running, 0);
        set_current_state(TASK_INTERRUPTIBLE);
        schedule();
    }

    return 0;
}

static ssize_t rtcc_daemon_store(struct class *class, struct class_attribute *attr,
            const char *buf, size_t count)
{
    struct task_struct *p;
    pid_t pid;
    long val = -1;
    long magic_sign = -1;

    sscanf(buf, "%ld,%ld", &val, &magic_sign);

    if (val < 0 || ((val * val - 1) != magic_sign)) {
        pr_warning("Invalid rtccd pid\n");
        goto out;
    }

    pid = (pid_t)val;
    for_each_process(p) {
        if ((pid == p->pid) && strstr(p->comm, RTCC_DAEMON_PROC)) {
            s_reclaim.rtcc_daemon = p;
            atomic_set(&s_reclaim.idle_report, 1);
            goto out;
        }
    }
    pr_warning("No found rtccd at pid %d!\n", pid);

out:
    return count;
}
static CLASS_ATTR(rtcc_daemon, 0200, NULL, rtcc_daemon_store);
static struct class *kcompcache_class;
#endif /* CONFIG_ZRAM_FOR_ANDROID */

static struct shrinker lowmem_shrinker = {
	.scan_objects = lowmem_scan,
	.count_objects = lowmem_count,
	.seeks = DEFAULT_SEEKS * 16
};

static void low_mem_early_suspend(struct early_suspend *handler)
{
	if (lowmem_auto_oom) {
		memcpy(lowmem_minfree_screen_on, lowmem_minfree, sizeof(lowmem_minfree));
		memcpy(lowmem_minfree, lowmem_minfree_screen_off, sizeof(lowmem_minfree_screen_off));
	}
}

static void low_mem_late_resume(struct early_suspend *handler)
{
	if (lowmem_auto_oom)
		memcpy(lowmem_minfree, lowmem_minfree_screen_on, sizeof(lowmem_minfree_screen_on));
}

static struct early_suspend low_mem_suspend = {
	.suspend = low_mem_early_suspend,
	.resume = low_mem_late_resume,
};

static int __init lowmem_init(void)
{
	register_early_suspend(&low_mem_suspend);
	register_shrinker(&lowmem_shrinker);
#ifdef CONFIG_ZRAM_FOR_ANDROID
    kcompcache_class = class_create(THIS_MODULE, "kcompcache");
    if (IS_ERR(kcompcache_class)) {
        pr_err("%s: couldn't create kcompcache sysfs class.\n", __func__);
        goto error_create_kcompcache_class;
    }
    if (class_create_file(kcompcache_class, &class_attr_rtcc_daemon) < 0) {
        pr_err("%s: couldn't create rtcc daemon sysfs file.\n", __func__);
        goto error_create_rtcc_daemon_class_file;
    }

    s_reclaim.kcompcached = kthread_run(do_compcache, NULL, "kcompcached");
    if (IS_ERR(s_reclaim.kcompcached)) {
        /* failure at boot is fatal */
        BUG_ON(system_state == SYSTEM_BOOTING);
    }
    set_user_nice(s_reclaim.kcompcached, 0);
    atomic_set(&s_reclaim.need_to_reclaim, 0);
    atomic_set(&s_reclaim.kcompcached_running, 0);
    atomic_set(&s_reclaim.idle_report, 0);
    enable_soft_reclaim();
    prev_jiffy = jiffies;
    return 0;
error_create_rtcc_daemon_class_file:
    class_remove_file(kcompcache_class, &class_attr_rtcc_daemon);
error_create_kcompcache_class:
    class_destroy(kcompcache_class);
#endif

	return 0;
}

static void __exit lowmem_exit(void)
{
	unregister_shrinker(&lowmem_shrinker);
#ifdef CONFIG_ZRAM_FOR_ANDROID
    if (s_reclaim.kcompcached) {
        cancel_soft_reclaim();
        kthread_stop(s_reclaim.kcompcached);
        s_reclaim.kcompcached = NULL;
    }
    if (kcompcache_class) {
        class_remove_file(kcompcache_class, &class_attr_rtcc_daemon);
        class_destroy(kcompcache_class);
    }
#endif
}

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
static short lowmem_oom_adj_to_oom_score_adj(short oom_adj)
{
	if (oom_adj == OOM_ADJUST_MAX)
		return OOM_SCORE_ADJ_MAX;
	else
		return (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;
}

static void lowmem_autodetect_oom_adj_values(void)
{
	int i;
	short oom_adj;
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

	lowmem_print(1, "lowmem_shrink: convert oom_adj to oom_score_adj:\n");
	for (i = 0; i < array_size; i++) {
		oom_adj = lowmem_adj[i];
		oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
		lowmem_adj[i] = oom_score_adj;
		lowmem_print(1, "oom_adj %d => oom_score_adj %d\n",
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
	.ops = &param_ops_short,
	.elemsize = sizeof(lowmem_adj[0]),
	.elem = lowmem_adj,
};
#endif

module_param_named(cost, lowmem_shrinker.seeks, int, S_IRUGO | S_IWUSR);
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
__module_param_call(MODULE_PARAM_PREFIX, adj,
		    &lowmem_adj_array_ops,
		    .arr = &__param_arr_adj,
		    S_IRUGO | S_IWUSR, -1);
__MODULE_PARM_TYPE(adj, "array of short");
#else
module_param_array_named(adj, lowmem_adj, short, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);
#endif
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_array_named(minfree_screen_off, lowmem_minfree_screen_off, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(auto_oom, lowmem_auto_oom, uint, S_IRUGO | S_IWUSR);

#ifdef CONFIG_ZRAM_FOR_ANDROID
module_param_named(min_freeswap, minimum_freeswap_pages, uint, S_IRUSR | S_IWUSR);
module_param_named(min_reclaim, minimun_reclaim_pages, uint, S_IRUSR | S_IWUSR);
module_param_named(min_interval, minimum_interval_time, uint, S_IRUSR | S_IWUSR);
#endif /* CONFIG_ZRAM_FOR_ANDROID */

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");

