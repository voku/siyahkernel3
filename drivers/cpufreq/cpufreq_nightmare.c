/*
 *  drivers/cpufreq/cpufreq_nightmare.c
 *
 *  Copyright (C)  2011 Samsung Electronics co. ltd
 *    ByungChang Cha <bc.cha@samsung.com>
 *
 *  Based on ondemand governor
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 * 
 * Created by Alucard_24@xda
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/reboot.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#define EARLYSUSPEND_HOTPLUGLOCK 1
/*
 * dbs is used in this file as a shortform for demandbased switching
 * It helps to keep variable names smaller, simpler
 */

#define MAX_HOTPLUG_RATE		(40)
#define HOTPLUG_DOWN_INDEX		(0)
#define HOTPLUG_UP_INDEX		(1)

static unsigned int hotplug_freq[4][2] = {
	{0, 500000},
	{200000, 500000},
	{200000, 500000},
	{200000, 0}
};

static void do_nightmare_timer(struct work_struct *work);
static int cpufreq_governor_nightmare(struct cpufreq_policy *policy,
				unsigned int event);

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_NIGHTMARE
static
#endif
struct cpufreq_governor cpufreq_gov_nightmare = {
	.name                   = "nightmare",
	.governor               = cpufreq_governor_nightmare,
	.owner                  = THIS_MODULE,
};

struct cpufreq_nightmare_cpuinfo {
	u64 prev_cpu_idle;
	u64 prev_cpu_iowait;
	u64 prev_cpu_wall;
	cputime64_t prev_cpu_nice;
	struct delayed_work work;
	struct work_struct up_work;
	struct work_struct down_work;
	int cpu;
	/*
	 * percpu mutex that serializes governor limit change with
	 * do_nightmare_timer invocation. We do not want do_nightmare_timer to run
	 * when user is changing the governor or limits.
	 */
	struct mutex timer_mutex;
};
static DEFINE_PER_CPU(struct cpufreq_nightmare_cpuinfo, od_nightmare_cpuinfo);

static unsigned int nightmare_enable;	/* number of CPUs using this policy */
/*
 * nightmare_mutex protects nightmare_enable in governor start/stop.
 */
static DEFINE_MUTEX(nightmare_mutex);

static struct workqueue_struct *dvfs_workqueue;

/* nightmare tuners */
static struct nightmare_tuners {
	atomic_t sampling_rate;
	atomic_t tpc_sampling_rate;
	atomic_t io_is_busy;
	atomic_t hotplug_lock;
	atomic_t hotplug_enable;
	atomic_t max_cpu_lock;
	atomic_t min_cpu_lock;
	atomic_t cpu_up_rate;
	atomic_t cpu_down_rate;
	atomic_t hotplug_compare_level;
	atomic_t up_avg_load;
	atomic_t down_avg_load;
	atomic_t ignore_nice;
	atomic_t inc_cpu_load_at_min_freq;
	atomic_t inc_cpu_load;
	atomic_t dec_cpu_load;
	atomic_t freq_for_responsiveness;
	atomic_t freq_for_responsiveness_max;
	atomic_t freq_up_brake_at_min_freq;
	atomic_t freq_up_brake;
	atomic_t freq_step_at_min_freq;
	atomic_t freq_step;
	atomic_t freq_step_dec;
	atomic_t freq_step_dec_at_max_freq;
	atomic_t freq_for_calc_incr;
	atomic_t freq_for_calc_decr;
	atomic_t up_sf_step;
	atomic_t down_sf_step;
	atomic_t earlysuspend;
} nightmare_tuners_ins = {
	.sampling_rate = ATOMIC_INIT(60000),
	.tpc_sampling_rate = ATOMIC_INIT(0),
	.hotplug_lock = ATOMIC_INIT(0),
	.hotplug_enable = ATOMIC_INIT(0),
	.max_cpu_lock = ATOMIC_INIT(0),
	.min_cpu_lock = ATOMIC_INIT(0),
	.cpu_up_rate = ATOMIC_INIT(10),
	.cpu_down_rate = ATOMIC_INIT(5),
	.hotplug_compare_level = ATOMIC_INIT(1),
	.up_avg_load = ATOMIC_INIT(65),
	.down_avg_load = ATOMIC_INIT(30),
	.ignore_nice = ATOMIC_INIT(0),
	.inc_cpu_load_at_min_freq = ATOMIC_INIT(60),
	.inc_cpu_load = ATOMIC_INIT(70),
	.dec_cpu_load = ATOMIC_INIT(50),
	.freq_for_responsiveness = ATOMIC_INIT(400000),
	.freq_for_responsiveness_max = ATOMIC_INIT(1200000),
	.freq_step_at_min_freq = ATOMIC_INIT(20),
	.freq_step = ATOMIC_INIT(20),
	.freq_up_brake_at_min_freq = ATOMIC_INIT(30),
	.freq_up_brake = ATOMIC_INIT(30),
	.freq_step_dec = ATOMIC_INIT(10),
	.freq_step_dec_at_max_freq = ATOMIC_INIT(10),
	.freq_for_calc_incr = ATOMIC_INIT(200000),
	.freq_for_calc_decr = ATOMIC_INIT(200000),
	.up_sf_step = ATOMIC_INIT(0),
	.down_sf_step = ATOMIC_INIT(0),
	.earlysuspend = ATOMIC_INIT(0),
};

/*
 * CPU hotplug lock interface
 */

static void apply_hotplug_lock(void)
{
	int online, possible, lock, flag;
	struct work_struct *work;
	struct cpufreq_nightmare_cpuinfo *nightmare_cpuinfo;

	/* do turn_on/off cpus */
	nightmare_cpuinfo = &per_cpu(od_nightmare_cpuinfo, 0); /* from CPU0 */
	online = num_online_cpus();
	possible = num_possible_cpus();
	lock = atomic_read(&nightmare_tuners_ins.hotplug_lock);
	flag = lock - online;

	if (lock == 0 || flag == 0)
		return;

	work = flag > 0 ? &nightmare_cpuinfo->up_work : &nightmare_cpuinfo->down_work;

	/*pr_debug("%s online %d possible %d lock %d flag %d %d\n",
		 __func__, online, possible, lock, flag, (int)abs(flag));*/

	queue_work_on(nightmare_cpuinfo->cpu, dvfs_workqueue, work);
}

/*
 * History of CPU usage
 */
struct nightmare_cpu_usage {
	unsigned int freq[NR_CPUS];
	int load[NR_CPUS];
};

struct nightmare_cpu_usage_history {
	struct nightmare_cpu_usage usage[MAX_HOTPLUG_RATE];
	int num_hist;
};

static struct nightmare_cpu_usage_history *hotplug_history;

static inline cputime64_t get_cpu_iowait_time(unsigned int cpu, cputime64_t *wall)
{
	u64 iowait_time = get_cpu_iowait_time_us(cpu, wall);

	if (iowait_time == -1ULL)
		return 0;

	return iowait_time;
}

/************************** sysfs interface ************************/

/* cpufreq_nightmare Governor Tunables */
#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%d\n", atomic_read(&nightmare_tuners_ins.object));		\
}
show_one(sampling_rate, sampling_rate);
show_one(tpc_sampling_rate, tpc_sampling_rate);
show_one(io_is_busy, io_is_busy);
show_one(max_cpu_lock, max_cpu_lock);
show_one(min_cpu_lock, min_cpu_lock);
show_one(hotplug_enable, hotplug_enable);
show_one(cpu_up_rate, cpu_up_rate);
show_one(cpu_down_rate, cpu_down_rate);
show_one(hotplug_compare_level,hotplug_compare_level);
show_one(up_avg_load, up_avg_load);
show_one(down_avg_load, down_avg_load);
show_one(ignore_nice_load, ignore_nice);
show_one(inc_cpu_load_at_min_freq, inc_cpu_load_at_min_freq);
show_one(inc_cpu_load, inc_cpu_load);
show_one(dec_cpu_load, dec_cpu_load);
show_one(freq_for_responsiveness, freq_for_responsiveness);
show_one(freq_for_responsiveness_max, freq_for_responsiveness_max);
show_one(freq_step_at_min_freq, freq_step_at_min_freq);
show_one(freq_step, freq_step);
show_one(freq_up_brake_at_min_freq, freq_up_brake_at_min_freq);
show_one(freq_up_brake, freq_up_brake);
show_one(freq_step_dec, freq_step_dec);
show_one(freq_step_dec_at_max_freq, freq_step_dec_at_max_freq);
show_one(freq_for_calc_incr, freq_for_calc_incr);
show_one(freq_for_calc_decr, freq_for_calc_decr);
show_one(up_sf_step, up_sf_step);
show_one(down_sf_step, down_sf_step);

#define show_hotplug_param(file_name, num_core, up_down)		\
static ssize_t show_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", file_name[num_core - 1][up_down]);	\
}

#define store_hotplug_param(file_name, num_core, up_down)		\
static ssize_t store_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr,				\
	const char *buf, size_t count)					\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	file_name[num_core - 1][up_down] = input;			\
	return count;							\
}

show_hotplug_param(hotplug_freq, 1, 1);
show_hotplug_param(hotplug_freq, 2, 0);
#ifndef CONFIG_CPU_EXYNOS4210
show_hotplug_param(hotplug_freq, 2, 1);
show_hotplug_param(hotplug_freq, 3, 0);
show_hotplug_param(hotplug_freq, 3, 1);
show_hotplug_param(hotplug_freq, 4, 0);
#endif

store_hotplug_param(hotplug_freq, 1, 1);
store_hotplug_param(hotplug_freq, 2, 0);
#ifndef CONFIG_CPU_EXYNOS4210
store_hotplug_param(hotplug_freq, 2, 1);
store_hotplug_param(hotplug_freq, 3, 0);
store_hotplug_param(hotplug_freq, 3, 1);
store_hotplug_param(hotplug_freq, 4, 0);
#endif

define_one_global_rw(hotplug_freq_1_1);
define_one_global_rw(hotplug_freq_2_0);
#ifndef CONFIG_CPU_EXYNOS4210
define_one_global_rw(hotplug_freq_2_1);
define_one_global_rw(hotplug_freq_3_0);
define_one_global_rw(hotplug_freq_3_1);
define_one_global_rw(hotplug_freq_4_0);
#endif

/* sampling_rate */
static ssize_t store_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input,10000);
	
	if (input == atomic_read(&nightmare_tuners_ins.sampling_rate))
		return count;

	atomic_set(&nightmare_tuners_ins.sampling_rate,input);

	return count;
}

/* tpc_sampling_rate */
static ssize_t store_tpc_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input,0);
	
	if (input == atomic_read(&nightmare_tuners_ins.tpc_sampling_rate))
		return count;

	atomic_set(&nightmare_tuners_ins.tpc_sampling_rate,input);

	return count;
}


/* io_is_busy */
static ssize_t store_io_is_busy(struct kobject *a, struct attribute *b,
				const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	atomic_set(&nightmare_tuners_ins.io_is_busy,!!input);

	return count;
}

/* hotplug_enable */
static ssize_t store_hotplug_enable(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = input > 0; 

	if (atomic_read(&nightmare_tuners_ins.hotplug_enable) == input)
		return count;

	if (input > 0) {
		apply_hotplug_lock();
	} else {
		apply_hotplug_lock();
	}
	atomic_set(&nightmare_tuners_ins.hotplug_enable, input);

	return count;
}

/* max_cpu_lock */
static ssize_t store_max_cpu_lock(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;
	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > num_possible_cpus())
		input = num_possible_cpus();

	atomic_set(&nightmare_tuners_ins.max_cpu_lock,input);

	return count;
}

/* min_cpu_lock */
static ssize_t store_min_cpu_lock(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;
	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > num_possible_cpus())
		input = num_possible_cpus();

	atomic_set(&nightmare_tuners_ins.min_cpu_lock,input);

	return count;
}

/* cpu_up_rate */
static ssize_t store_cpu_up_rate(struct kobject *a, struct attribute *b,
				 const char *buf, size_t count)
{
	int input;
	int ret;
	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,MAX_HOTPLUG_RATE),1);

	if (input == atomic_read(&nightmare_tuners_ins.cpu_up_rate))
		return count;

	atomic_set(&nightmare_tuners_ins.cpu_up_rate,input);

	return count;
}

/* cpu_down_rate */
static ssize_t store_cpu_down_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,MAX_HOTPLUG_RATE),1);

	if (input == atomic_read(&nightmare_tuners_ins.cpu_down_rate))
		return count;

	atomic_set(&nightmare_tuners_ins.cpu_down_rate,input);
	return count;
}

/* hotplug_compare_level */
static ssize_t store_hotplug_compare_level(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = (input > 0);

	if (input == atomic_read(&nightmare_tuners_ins.hotplug_compare_level)) { /* nothing to do */
		return count;
	}

	atomic_set(&nightmare_tuners_ins.hotplug_compare_level,input);

	return count;
}

/* up_avg_load */
static ssize_t store_up_avg_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,100),0);

	if (input == atomic_read(&nightmare_tuners_ins.up_avg_load))
		return count;

	atomic_set(&nightmare_tuners_ins.up_avg_load,input);

	return count;
}

/* down_avg_load */
static ssize_t store_down_avg_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;
	
	input = max(min(input,95),5);

	if (input == atomic_read(&nightmare_tuners_ins.down_avg_load))
		return count;

	atomic_set(&nightmare_tuners_ins.down_avg_load,input);

	return count;
}

/* ignore_nice_load */
static ssize_t store_ignore_nice_load(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	int input;
	int ret;
	unsigned int j;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = input > 0;

	if (input == atomic_read(&nightmare_tuners_ins.ignore_nice)) {/* nothing to do */
		return count;
	}
	atomic_set(&nightmare_tuners_ins.ignore_nice,input);

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct cpufreq_nightmare_cpuinfo *nightmare_cpuinfo;
		nightmare_cpuinfo = &per_cpu(od_nightmare_cpuinfo, j);
		nightmare_cpuinfo->prev_cpu_idle =
			get_cpu_idle_time(j, &nightmare_cpuinfo->prev_cpu_wall);
		if (atomic_read(&nightmare_tuners_ins.ignore_nice))
			nightmare_cpuinfo->prev_cpu_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];
	}
	return count;
}

/* inc_cpu_load_at_min_freq */
static ssize_t store_inc_cpu_load_at_min_freq(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1) {
		return -EINVAL;
	}

	input = min(input,atomic_read(&nightmare_tuners_ins.inc_cpu_load));

	if (input == atomic_read(&nightmare_tuners_ins.inc_cpu_load_at_min_freq))
		return count;

	atomic_set(&nightmare_tuners_ins.inc_cpu_load_at_min_freq,input);

	return count;
}

/* inc_cpu_load */
static ssize_t store_inc_cpu_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,100),0);

	if (input == atomic_read(&nightmare_tuners_ins.inc_cpu_load))
		return count;

	atomic_set(&nightmare_tuners_ins.inc_cpu_load,input);

	return count;
}

/* dec_cpu_load */
static ssize_t store_dec_cpu_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,95),5);

	if (input == atomic_read(&nightmare_tuners_ins.dec_cpu_load))
		return count;

	atomic_set(&nightmare_tuners_ins.dec_cpu_load,input);

	return count;
}

/* freq_for_responsiveness */
static ssize_t store_freq_for_responsiveness(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	if (input == atomic_read(&nightmare_tuners_ins.freq_for_responsiveness))
		return count;

	atomic_set(&nightmare_tuners_ins.freq_for_responsiveness,input);

	return count;
}

/* freq_for_responsiveness_max */
static ssize_t store_freq_for_responsiveness_max(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	if (input == atomic_read(&nightmare_tuners_ins.freq_for_responsiveness_max))
		return count;

	atomic_set(&nightmare_tuners_ins.freq_for_responsiveness_max,input);

	return count;
}

/* freq_step_at_min_freq */
static ssize_t store_freq_step_at_min_freq(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,100),0);

	if (input == atomic_read(&nightmare_tuners_ins.freq_step_at_min_freq))
		return count;

	atomic_set(&nightmare_tuners_ins.freq_step_at_min_freq,input);

	return count;
}

/* freq_step */
static ssize_t store_freq_step(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,100),0);

	if (input == atomic_read(&nightmare_tuners_ins.freq_step))
		return count;

	atomic_set(&nightmare_tuners_ins.freq_step,input);

	return count;
}

/* freq_up_brake_at_min_freq */
static ssize_t store_freq_up_brake_at_min_freq(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,100),0);

	if (input == atomic_read(&nightmare_tuners_ins.freq_up_brake_at_min_freq)) {/* nothing to do */
		return count;
	}

	atomic_set(&nightmare_tuners_ins.freq_up_brake_at_min_freq,input);

	return count;
}

/* freq_up_brake */
static ssize_t store_freq_up_brake(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,100),0);

	if (input == atomic_read(&nightmare_tuners_ins.freq_up_brake)) {/* nothing to do */
		return count;
	}

	atomic_set(&nightmare_tuners_ins.freq_up_brake,input);

	return count;
}

/* freq_step_dec */
static ssize_t store_freq_step_dec(struct kobject *a, struct attribute *b,
				       const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,100),0);

	if (input == atomic_read(&nightmare_tuners_ins.freq_step_dec)) {/* nothing to do */
		return count;
	}

	atomic_set(&nightmare_tuners_ins.freq_step_dec,input);


	return count;
}

/* freq_step_dec_at_max_freq */
static ssize_t store_freq_step_dec_at_max_freq(struct kobject *a, struct attribute *b,
				       const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,100),0);

	if (input == atomic_read(&nightmare_tuners_ins.freq_step_dec_at_max_freq)) {/* nothing to do */
		return count;
	}

	atomic_set(&nightmare_tuners_ins.freq_step_dec_at_max_freq,input);

	return count;
}

/* freq_for_calc_incr */
static ssize_t store_freq_for_calc_incr(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,400000),50000);

	if (input == atomic_read(&nightmare_tuners_ins.freq_for_calc_incr))
		return count;

	atomic_set(&nightmare_tuners_ins.freq_for_calc_incr,input);

	return count;
}

/* freq_for_calc_decr */
static ssize_t store_freq_for_calc_decr(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,400000),125000);

	if (input == atomic_read(&nightmare_tuners_ins.freq_for_calc_decr))
		return count;

	atomic_set(&nightmare_tuners_ins.freq_for_calc_decr,input);

	return count;
}

/* up_sf_step */
static ssize_t store_up_sf_step(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,99),0);

	if (input == atomic_read(&nightmare_tuners_ins.up_sf_step))
		return count;

	 atomic_set(&nightmare_tuners_ins.up_sf_step,input);

	return count;
}

/* down_sf_step */
static ssize_t store_down_sf_step(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,99),0);

	if (input == atomic_read(&nightmare_tuners_ins.down_sf_step))
		return count;

	atomic_set(&nightmare_tuners_ins.down_sf_step,input);

	return count;
}

define_one_global_rw(sampling_rate);
define_one_global_rw(tpc_sampling_rate);
define_one_global_rw(io_is_busy);
define_one_global_rw(hotplug_enable);
define_one_global_rw(max_cpu_lock);
define_one_global_rw(min_cpu_lock);
define_one_global_rw(cpu_up_rate);
define_one_global_rw(cpu_down_rate);
define_one_global_rw(hotplug_compare_level);
define_one_global_rw(up_avg_load);
define_one_global_rw(down_avg_load);
define_one_global_rw(ignore_nice_load);
define_one_global_rw(inc_cpu_load_at_min_freq);
define_one_global_rw(inc_cpu_load);
define_one_global_rw(dec_cpu_load);
define_one_global_rw(freq_for_responsiveness);
define_one_global_rw(freq_for_responsiveness_max);
define_one_global_rw(freq_step_at_min_freq);
define_one_global_rw(freq_step);
define_one_global_rw(freq_up_brake_at_min_freq);
define_one_global_rw(freq_up_brake);
define_one_global_rw(freq_step_dec);
define_one_global_rw(freq_step_dec_at_max_freq);
define_one_global_rw(freq_for_calc_incr);
define_one_global_rw(freq_for_calc_decr);
define_one_global_rw(up_sf_step);
define_one_global_rw(down_sf_step);

static struct attribute *nightmare_attributes[] = {
	&sampling_rate.attr,
	&tpc_sampling_rate.attr,
	&io_is_busy.attr,
	&hotplug_enable.attr,
	&max_cpu_lock.attr,
	&min_cpu_lock.attr,
	/* priority: hotplug_lock > max_cpu_lock > min_cpu_lock
	   Exception: hotplug_lock on early_suspend uses min_cpu_lock */
	&hotplug_freq_1_1.attr,
	&hotplug_freq_2_0.attr,
#ifndef CONFIG_CPU_EXYNOS4210
	&hotplug_freq_2_1.attr,
	&hotplug_freq_3_0.attr,
	&hotplug_freq_3_1.attr,
	&hotplug_freq_4_0.attr,
#endif
	&cpu_up_rate.attr,
	&cpu_down_rate.attr,
	&hotplug_compare_level.attr,
	&up_avg_load.attr,
	&down_avg_load.attr,
	&ignore_nice_load.attr,
	&inc_cpu_load_at_min_freq.attr,
	&inc_cpu_load.attr,
	&dec_cpu_load.attr,
	&freq_for_responsiveness.attr,
	&freq_for_responsiveness_max.attr,
	&freq_step_at_min_freq.attr,
	&freq_step.attr,
	&freq_up_brake_at_min_freq.attr,
	&freq_up_brake.attr,
	&freq_step_dec.attr,
	&freq_step_dec_at_max_freq.attr,
	&freq_for_calc_incr.attr,
	&freq_for_calc_decr.attr,
	&up_sf_step.attr,
	&down_sf_step.attr,
	NULL
};

static struct attribute_group nightmare_attr_group = {
	.attrs = nightmare_attributes,
	.name = "nightmare",
};

/************************** sysfs end ************************/

static void cpu_up_work(struct work_struct *work)
{
	int cpu;
	int online = num_online_cpus();
	int nr_up = 1;
	int min_cpu_lock = atomic_read(&nightmare_tuners_ins.min_cpu_lock);
	int hotplug_lock = atomic_read(&nightmare_tuners_ins.hotplug_lock);

	if (hotplug_lock && min_cpu_lock)
		nr_up = max(hotplug_lock, min_cpu_lock) - online;
	else if (hotplug_lock)
		nr_up = hotplug_lock - online;
	else if (min_cpu_lock)
		nr_up = max(nr_up, min_cpu_lock - online);

	if (online == 1) {
		/* printk(KERN_ERR "CPU_UP %d\n", online); */
		cpu_up(num_possible_cpus() - 1);
		nr_up -= 1;
	}

	for_each_cpu_not(cpu, cpu_online_mask) {
		if (nr_up-- == 0)
			break;
		if (cpu == 0)
			continue;
		/* printk(KERN_ERR "CPU_UP %d\n", cpu); */
		cpu_up(cpu);
	}
}

static void cpu_down_work(struct work_struct *work)
{
	int cpu;
	int online = num_online_cpus();
	int nr_down = 1;
	int hotplug_lock = atomic_read(&nightmare_tuners_ins.hotplug_lock);

	if (hotplug_lock)
		nr_down = online - hotplug_lock;

	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
		/* printk(KERN_ERR "CPU_DOWN %d\n", cpu); */
		cpu_down(cpu);
		if (--nr_down == 0)
			break;
	}
}

static int check_up(void)
{
	int num_hist = hotplug_history->num_hist;
	struct nightmare_cpu_usage *usage;
	unsigned int freq;
	int avg_load;
	int i;
	int up_rate = atomic_read(&nightmare_tuners_ins.cpu_up_rate);
	int up_avg_load = atomic_read(&nightmare_tuners_ins.up_avg_load);
	int max_cpu_lock = atomic_read(&nightmare_tuners_ins.max_cpu_lock);
	int min_cpu_lock = atomic_read(&nightmare_tuners_ins.min_cpu_lock);
	bool hotplug_compare_level = atomic_read(&nightmare_tuners_ins.hotplug_compare_level) > 0;
	unsigned int up_freq;
	unsigned int min_freq = UINT_MAX;
	int min_avg_load = 100;
	int online;

	if (atomic_read(&nightmare_tuners_ins.hotplug_lock) > 0) {
		return 0;
	}

	online = num_online_cpus();
	up_freq = hotplug_freq[online - 1][HOTPLUG_UP_INDEX];

	if (online == num_possible_cpus())
		return 0;

	if (max_cpu_lock != 0 
		&& online >= max_cpu_lock)
		return 0;

	if (min_cpu_lock != 0
		&& online < min_cpu_lock)
		return 1;

	if (num_hist == 0 || num_hist % up_rate)
		return 0;

	if (hotplug_compare_level) {
		usage = &hotplug_history->usage[num_hist - 1];
		min_freq = usage->freq[0];
		min_avg_load = usage->load[0];
	} else {
		for (i = num_hist - 1; i >= num_hist - up_rate; --i) {
			usage = &hotplug_history->usage[i];
			freq = usage->freq[0];
			avg_load = usage->load[0];
			min_freq = min(min_freq, freq);
			min_avg_load = min(min_avg_load, avg_load);
		}
	}
	if (min_freq >= up_freq) {
		if (min_avg_load < up_avg_load) {
			return 0;
		}
		/* printk(KERN_ERR "[HOTPLUG IN] %s %u>=%u\n",
			__func__, min_freq, up_freq); */
		hotplug_history->num_hist = 0;
		return 1;
	}
	return 0;
}

static int check_down(void)
{
	int num_hist = hotplug_history->num_hist;
	struct nightmare_cpu_usage *usage;
	unsigned int freq;
	int avg_load;
	int i;
	int down_rate = atomic_read(&nightmare_tuners_ins.cpu_down_rate);
	int down_avg_load = atomic_read(&nightmare_tuners_ins.down_avg_load);
	int max_cpu_lock = atomic_read(&nightmare_tuners_ins.max_cpu_lock);
	int min_cpu_lock = atomic_read(&nightmare_tuners_ins.min_cpu_lock);
	bool hotplug_compare_level = atomic_read(&nightmare_tuners_ins.hotplug_compare_level) > 0;
	unsigned int down_freq;
	unsigned int max_freq = 0;
	int max_avg_load = 0;
	int online;

	if (atomic_read(&nightmare_tuners_ins.hotplug_lock) > 0) {
		return 0;
	}

	online = num_online_cpus();
	down_freq = hotplug_freq[online - 1][HOTPLUG_DOWN_INDEX];

	if (online == 1)
		return 0;

	if (max_cpu_lock != 0
		&& online > max_cpu_lock)
		return 1;

	if (min_cpu_lock != 0
		&& online <= min_cpu_lock)
		return 0;

	if (num_hist == 0 || num_hist % down_rate)
		return 0;

	if (hotplug_compare_level) {
		usage = &hotplug_history->usage[num_hist - 1];
		max_freq = usage->freq[1];
		max_avg_load = usage->load[1];
	} else {
		for (i = num_hist - 1; i >= num_hist - down_rate; --i) {
			usage = &hotplug_history->usage[i];
			freq = usage->freq[1];
			avg_load = usage->load[1];
			max_freq = max(max_freq, freq);
			max_avg_load = max(max_avg_load, avg_load);
		}
	}
	if ((max_freq <= down_freq) 
		|| (max_avg_load < down_avg_load)) {
		/* printk(KERN_ERR "[HOTPLUG OUT] %s %u<=%u\n",
			__func__, max_freq, down_freq); */
		hotplug_history->num_hist = 0;
		return 1;
	}
	return 0;
}

static unsigned int nightmare_frequency_adjust(unsigned int next_freq, unsigned int cur_freq, unsigned int min_freq, unsigned int max_freq, int scaling_freq_step)
{
	unsigned int adjust_freq = 0;

	if (next_freq >= max_freq)
		return max_freq;
	else if (next_freq <= min_freq)
		return min_freq;

	adjust_freq = (next_freq / 100000) * 100000;

	/* Avoid to manage freq with up_sf_step or down_sf_step */
	if (adjust_freq == cur_freq)
		return cur_freq;

	if ((int)next_freq % 100000 > (scaling_freq_step * 1000))
		adjust_freq += 100000;

	return adjust_freq;
		
}

static void nightmare_check_cpu(struct cpufreq_nightmare_cpuinfo *this_nightmare_cpuinfo)
{
	unsigned int j;
	int num_hist = hotplug_history->num_hist;
	int max_hotplug_rate = max(atomic_read(&nightmare_tuners_ins.cpu_up_rate),atomic_read(&nightmare_tuners_ins.cpu_down_rate));
	/* add total_load, avg_load to get average load */
	bool hotplug_enable = atomic_read(&nightmare_tuners_ins.hotplug_enable) > 0;
	bool ignore_nice = atomic_read(&nightmare_tuners_ins.ignore_nice) > 0;
	bool io_is_busy = atomic_read(&nightmare_tuners_ins.io_is_busy) > 0;
	bool earlysuspend = atomic_read(&nightmare_tuners_ins.earlysuspend) > 0;
	int freq_for_responsiveness = atomic_read(&nightmare_tuners_ins.freq_for_responsiveness);
	int freq_for_responsiveness_max = atomic_read(&nightmare_tuners_ins.freq_for_responsiveness_max);
	int up_sf_step = atomic_read(&nightmare_tuners_ins.up_sf_step);
	int down_sf_step = atomic_read(&nightmare_tuners_ins.down_sf_step);	
	int freq_for_calc_incr = atomic_read(&nightmare_tuners_ins.freq_for_calc_incr);
	int freq_for_calc_decr = atomic_read(&nightmare_tuners_ins.freq_for_calc_decr);
	int dec_cpu_load = atomic_read(&nightmare_tuners_ins.dec_cpu_load);
	unsigned int prev_freq_set = 0;

	/* set num_hist used */
	++hotplug_history->num_hist;

	for_each_possible_cpu(j) {
		struct cpufreq_nightmare_cpuinfo *j_nightmare_cpuinfo;
		struct cpufreq_policy *cpu_policy;
		u64 cur_wall_time, cur_idle_time, cur_iowait_time;
		unsigned int idle_time, wall_time, iowait_time;
		/* Extrapolated load of this CPU */
		unsigned int load_at_max_freq = 0;
		/* Current load across this CPU */
		int cur_load = 0;
		int inc_cpu_load = 0;		
		int freq_step = 0;
		int freq_up_brake = 0;
		int freq_step_dec = 0;
		int inc_load=0;
		int inc_brake=0;
		int dec_load = 0;
		unsigned int freq_up = 0;
		unsigned int freq_down = 0;
		unsigned int min_freq = 0;
		unsigned int max_freq = 0;

		j_nightmare_cpuinfo = &per_cpu(od_nightmare_cpuinfo, j);
		
		hotplug_history->usage[num_hist].freq[j] = 0;
		hotplug_history->usage[num_hist].load[j] = 0;
		
		cur_idle_time = get_cpu_idle_time(j, &cur_wall_time);
		cur_iowait_time = get_cpu_iowait_time(j, &cur_wall_time);

		wall_time = (unsigned int)
				(cur_wall_time - j_nightmare_cpuinfo->prev_cpu_wall);
		j_nightmare_cpuinfo->prev_cpu_wall = cur_wall_time;

		idle_time = (unsigned int)
				(cur_idle_time - j_nightmare_cpuinfo->prev_cpu_idle);
		j_nightmare_cpuinfo->prev_cpu_idle = cur_idle_time;

		iowait_time = (unsigned int)
				(cur_iowait_time - j_nightmare_cpuinfo->prev_cpu_iowait);
		j_nightmare_cpuinfo->prev_cpu_iowait = cur_iowait_time;

		if (!cpu_online(j)) {
			continue;
		}
		
		if (ignore_nice) {
			u64 cur_nice;
			unsigned long cur_nice_jiffies;

			cur_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE] -
						 j_nightmare_cpuinfo->prev_cpu_nice;
			/*
			 * Assumption: nice time between sampling periods will
			 * be less than 2^32 jiffies for 32 bit sys
			 */
			cur_nice_jiffies = (unsigned long)
					cputime64_to_jiffies64(cur_nice);

			j_nightmare_cpuinfo->prev_cpu_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];
			idle_time += jiffies_to_usecs(cur_nice_jiffies);
		}
	
		if (io_is_busy && idle_time >= iowait_time)
			idle_time -= iowait_time;

		if (!wall_time || wall_time < idle_time) {
			continue;
		}

		cpu_policy = cpufreq_cpu_get(j);
		if (!cpu_policy) {
			continue;
		}

		if (wall_time == idle_time) {
			cur_load = 1;
		} else {		
			cur_load = (int)(100 * (wall_time - idle_time) / wall_time);
		}
		hotplug_history->usage[num_hist].freq[j] = cpu_policy->cur;
		hotplug_history->usage[num_hist].load[j] = cur_load;

		/* printk(KERN_ERR "LOAD CALC.: CPU[%u], load[%d], freq[%u]\n",j, cur_load, cpu_policy->cur); */

		/* calculate the scaled load across CPU */
		load_at_max_freq = (cur_load * cpu_policy->cur)/
					cpu_policy->cpuinfo.max_freq;
		cpufreq_notify_utilization(cpu_policy, load_at_max_freq);
		
		// GET MIN MAX FREQ
		min_freq = cpu_policy->min;
		max_freq = cpu_policy->max;
		if (earlysuspend) {
			min_freq = min(cpu_policy->min_suspend,min_freq);
			max_freq = min(cpu_policy->max_suspend,max_freq);
		}
		cur_load = hotplug_history->usage[num_hist].load[j];
		/* CPUs Online Scale Frequency*/
		if (cpu_policy->cur < freq_for_responsiveness) {
			inc_cpu_load = atomic_read(&nightmare_tuners_ins.inc_cpu_load_at_min_freq);
			freq_step = atomic_read(&nightmare_tuners_ins.freq_step_at_min_freq);
			freq_up_brake = atomic_read(&nightmare_tuners_ins.freq_up_brake_at_min_freq);
		} else {
			inc_cpu_load = atomic_read(&nightmare_tuners_ins.inc_cpu_load);
			freq_step = atomic_read(&nightmare_tuners_ins.freq_step);
			freq_up_brake = atomic_read(&nightmare_tuners_ins.freq_up_brake);
		}
		/* Check for frequency increase or for frequency decrease */
		if (cur_load >= inc_cpu_load) {
			/* if we cannot increment the frequency anymore, break out early */
			if (cpu_policy->cur == max_freq) {
				cpufreq_cpu_put(cpu_policy);
				continue;
			}
			inc_load = ((cur_load * freq_for_calc_incr) / 100) + ((freq_step * freq_for_calc_incr) / 100);
			inc_brake = (freq_up_brake * freq_for_calc_incr) / 100;
			if (inc_brake > inc_load) {
				cpufreq_cpu_put(cpu_policy);
				continue;
			} else {
				freq_up = cpu_policy->cur + (unsigned int)(inc_load - inc_brake);
			}			
			freq_up = nightmare_frequency_adjust(freq_up, cpu_policy->cur, min_freq, max_freq, up_sf_step);
			/* printk(KERN_ERR "UP FREQ CALC.: CPU[%u], load[%d]>=inc_cpu_load[%d], target freq[%u], cur freq[%u], min freq[%u], max_freq[%u]\n",j, cur_load, inc_cpu_load, freq_up, cpu_policy->cur, min_freq, max_freq); */
			if (freq_up != cpu_policy->cur && freq_up != prev_freq_set) {
				prev_freq_set = freq_up;
				__cpufreq_driver_target(cpu_policy, freq_up, CPUFREQ_RELATION_L);
			}
			cpufreq_cpu_put(cpu_policy);
			continue;
		} else if (cur_load < dec_cpu_load && cur_load > 0) {
			/* if we cannot reduce the frequency anymore, break out early */
			if (cpu_policy->cur == min_freq) {
				cpufreq_cpu_put(cpu_policy);
				continue;
			}

			if (cpu_policy->cur > freq_for_responsiveness_max)
				freq_step_dec = atomic_read(&nightmare_tuners_ins.freq_step_dec_at_max_freq);
			else
				freq_step_dec = atomic_read(&nightmare_tuners_ins.freq_step_dec);

			dec_load = (((100 - cur_load) * freq_for_calc_decr) / 100) + ((freq_step_dec * freq_for_calc_decr) / 100);
			if (cpu_policy->cur >= dec_load + min_freq) {
				freq_down = cpu_policy->cur - (unsigned int)dec_load;
			} else {
				freq_down = min_freq;
			}
			freq_down = nightmare_frequency_adjust(freq_down, cpu_policy->cur, min_freq, max_freq, down_sf_step);
			/* printk(KERN_ERR "DOWN FREQ CALC.: CPU[%u], load[%d]<dec_cpu_load[%d], target freq[%u], cur freq[%u], min freq[%u], max_freq[%u]\n",j, cur_load, dec_cpu_load, freq_down, cpu_policy->cur, min_freq, max_freq); */
			if (freq_down < cpu_policy->cur && freq_down != prev_freq_set) {
				prev_freq_set = freq_down;
				__cpufreq_driver_target(cpu_policy, freq_down, CPUFREQ_RELATION_L);				
			}
			cpufreq_cpu_put(cpu_policy);
			continue;
		}
		cpufreq_cpu_put(cpu_policy);
	}

	if (hotplug_enable) {
		/* Check for CPU hotplug */
		if (check_up()) {
			queue_work_on(this_nightmare_cpuinfo->cpu, dvfs_workqueue,
			      &this_nightmare_cpuinfo->up_work);
		}
		else if (check_down()) {
			queue_work_on(this_nightmare_cpuinfo->cpu, dvfs_workqueue,
			      &this_nightmare_cpuinfo->down_work);
		}
	}
	if (hotplug_history->num_hist == max_hotplug_rate)
		hotplug_history->num_hist = 0;
}

static void do_nightmare_timer(struct work_struct *work)
{
	struct cpufreq_nightmare_cpuinfo *nightmare_cpuinfo =
		container_of(work, struct cpufreq_nightmare_cpuinfo, work.work);
	int delay;

	mutex_lock(&nightmare_cpuinfo->timer_mutex);
	nightmare_check_cpu(nightmare_cpuinfo);
	/* We want all CPUs to do sampling nearly on
	 * same jiffy
	 */
	delay = usecs_to_jiffies(atomic_read(&nightmare_tuners_ins.sampling_rate));
	if (num_online_cpus() > 1) {
		delay -= jiffies % delay;
	}	
	queue_delayed_work_on(nightmare_cpuinfo->cpu, dvfs_workqueue, &nightmare_cpuinfo->work, delay);
	mutex_unlock(&nightmare_cpuinfo->timer_mutex);
}

static inline void nightmare_timer_init(struct cpufreq_nightmare_cpuinfo *nightmare_cpuinfo)
{
	INIT_DEFERRABLE_WORK(&nightmare_cpuinfo->work, do_nightmare_timer);	
	INIT_WORK(&nightmare_cpuinfo->up_work, cpu_up_work);
	INIT_WORK(&nightmare_cpuinfo->down_work, cpu_down_work);
	queue_delayed_work_on(nightmare_cpuinfo->cpu, dvfs_workqueue, &nightmare_cpuinfo->work, usecs_to_jiffies(atomic_read(&nightmare_tuners_ins.tpc_sampling_rate)));
}

static inline void nightmare_timer_exit(struct cpufreq_nightmare_cpuinfo *nightmare_cpuinfo)
{
	cancel_delayed_work_sync(&nightmare_cpuinfo->work);
	cancel_work_sync(&nightmare_cpuinfo->up_work);
	cancel_work_sync(&nightmare_cpuinfo->down_work);
}

#if !EARLYSUSPEND_HOTPLUGLOCK
static int pm_notifier_call(struct notifier_block *this,
			    unsigned long event, void *ptr)
{
	static int prev_hotplug_lock;
	bool hotplug_enable = (atomic_read(&nightmare_tuners_ins.hotplug_enable) > 0);
	switch (event) {
	case PM_SUSPEND_PREPARE:
		if (hotplug_enable) {
			prev_hotplug_lock = atomic_read(&nightmare_tuners_ins.hotplug_lock);
			atomic_set(&nightmare_tuners_ins.hotplug_lock, 1);
			apply_hotplug_lock();
			/* pr_debug("%s enter suspend\n", __func__); */
		}
		return NOTIFY_OK;
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		if (hotplug_enable) {
			atomic_set(&nightmare_tuners_ins.hotplug_lock, prev_hotplug_lock);
			if (prev_hotplug_lock)
				apply_hotplug_lock();
			prev_hotplug_lock = 0;
			/* pr_debug("%s exit suspend\n", __func__); */
		}
		return NOTIFY_OK;
	}
	return NOTIFY_DONE;
}

static struct notifier_block pm_notifier = {
	.notifier_call = pm_notifier_call,
};
#endif

static struct early_suspend early_suspend;
static void cpufreq_nightmare_early_suspend(struct early_suspend *h)
{
	int lock = atomic_read(&nightmare_tuners_ins.min_cpu_lock);
	atomic_set(&nightmare_tuners_ins.earlysuspend,1);	
	if (atomic_read(&nightmare_tuners_ins.hotplug_enable) > 0) {
		if (!lock)
			lock = 1;
		atomic_set(&nightmare_tuners_ins.hotplug_lock,lock);
		apply_hotplug_lock();
	}
}
static void cpufreq_nightmare_late_resume(struct early_suspend *h)
{
	atomic_set(&nightmare_tuners_ins.earlysuspend,0);
	if (atomic_read(&nightmare_tuners_ins.hotplug_enable) > 0) {
		atomic_set(&nightmare_tuners_ins.hotplug_lock, 0);
		apply_hotplug_lock();
	}
}

static int cpufreq_governor_nightmare(struct cpufreq_policy *policy,
				unsigned int event)
{
	unsigned int cpu = policy->cpu;
	struct cpufreq_nightmare_cpuinfo *this_nightmare_cpuinfo;
	unsigned int j;
	unsigned int min_freq = 0;
	unsigned int max_freq = 0;
	bool ignore_nice = atomic_read(&nightmare_tuners_ins.ignore_nice) > 0;
	int rc;

	this_nightmare_cpuinfo = &per_cpu(od_nightmare_cpuinfo, cpu);

	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;

		/* SET POLICY SHARED TYPE AND APPLY MASK TO ALL CPUS */
		policy->shared_type = CPUFREQ_SHARED_TYPE_ANY;
		cpumask_setall(policy->cpus);

		hotplug_history->num_hist = 0;

		mutex_lock(&nightmare_mutex);
		nightmare_enable++;

		for_each_possible_cpu(j) {
			struct cpufreq_nightmare_cpuinfo *j_nightmare_cpuinfo;
			j_nightmare_cpuinfo = &per_cpu(od_nightmare_cpuinfo, j);
			j_nightmare_cpuinfo->prev_cpu_idle = get_cpu_idle_time(j,
				&j_nightmare_cpuinfo->prev_cpu_wall);
			if (ignore_nice)
				j_nightmare_cpuinfo->prev_cpu_nice =
					kcpustat_cpu(j).cpustat[CPUTIME_NICE];
		}
		this_nightmare_cpuinfo->cpu = cpu;
		/*
		 * Start the timerschedule work, when this governor
		 * is used for first time
		 */
		if (nightmare_enable == 1) {
			rc = sysfs_create_group(cpufreq_global_kobject,
						&nightmare_attr_group);
			if (rc) {
				mutex_unlock(&nightmare_mutex);
				return rc;
			}
			atomic_set(&nightmare_tuners_ins.io_is_busy,0);
			atomic_set(&nightmare_tuners_ins.earlysuspend,0);
			atomic_set(&nightmare_tuners_ins.hotplug_lock,0);
		}
		mutex_unlock(&nightmare_mutex);

		mutex_init(&this_nightmare_cpuinfo->timer_mutex);

		nightmare_timer_init(this_nightmare_cpuinfo);

#if !EARLYSUSPEND_HOTPLUGLOCK
		register_pm_notifier(&pm_notifier);
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
		register_early_suspend(&early_suspend);
#endif
		break;

	case CPUFREQ_GOV_STOP:
#ifdef CONFIG_HAS_EARLYSUSPEND
		unregister_early_suspend(&early_suspend);
#endif

#if !EARLYSUSPEND_HOTPLUGLOCK
		unregister_pm_notifier(&pm_notifier);
#endif

		nightmare_timer_exit(this_nightmare_cpuinfo);

		mutex_destroy(&this_nightmare_cpuinfo->timer_mutex);

		mutex_lock(&nightmare_mutex);
		nightmare_enable--;

		if (!nightmare_enable) {
			sysfs_remove_group(cpufreq_global_kobject,
					   &nightmare_attr_group);
		}
		mutex_unlock(&nightmare_mutex);
		
		break;

	case CPUFREQ_GOV_LIMITS:
		mutex_lock(&this_nightmare_cpuinfo->timer_mutex);
		min_freq = policy->min;
		max_freq = policy->max;
		if (atomic_read(&nightmare_tuners_ins.earlysuspend) > 0) {
			min_freq = min(policy->min_suspend,min_freq);
			max_freq = min(policy->max_suspend,max_freq);
		}
		for_each_online_cpu(j) {
			struct cpufreq_policy *cpu_policy;
			cpu_policy = cpufreq_cpu_get(j);
			if (!cpu_policy) {
				continue;
			}
			if (max_freq < cpu_policy->cur) {
				__cpufreq_driver_target(cpu_policy,max_freq,CPUFREQ_RELATION_L);
			} else if (min_freq > cpu_policy->cur) {
				__cpufreq_driver_target(cpu_policy,min_freq,CPUFREQ_RELATION_L);
			}
			cpufreq_cpu_put(cpu_policy);
		}
		mutex_unlock(&this_nightmare_cpuinfo->timer_mutex);
		break;
	}
	return 0;
}

static int __init cpufreq_gov_nightmare_init(void)
{
	int ret;

	hotplug_history = kzalloc(sizeof(struct nightmare_cpu_usage_history), GFP_KERNEL);
	if (!hotplug_history) {
		pr_err("%s cannot create hotplug history array\n", __func__);
		ret = -ENOMEM;
		goto err_free;
	}

	dvfs_workqueue = create_workqueue("knightmare");
	if (!dvfs_workqueue) {
		pr_err("%s cannot create workqueue\n", __func__);
		ret = -ENOMEM;
		goto err_queue;
	}

	ret = cpufreq_register_governor(&cpufreq_gov_nightmare);
	if (ret)
		goto err_reg;

	early_suspend.suspend = cpufreq_nightmare_early_suspend;
	early_suspend.resume = cpufreq_nightmare_late_resume;
	early_suspend.level = EARLY_SUSPEND_LEVEL_DISABLE_FB;

	return ret;

err_reg:
	destroy_workqueue(dvfs_workqueue);
err_queue:
	kfree(hotplug_history);
err_free:
	kfree(&nightmare_tuners_ins);
	kfree(&hotplug_freq);
	return ret;
}

static void __exit cpufreq_gov_nightmare_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_nightmare);
	destroy_workqueue(dvfs_workqueue);
	kfree(hotplug_history);
	kfree(&nightmare_tuners_ins);
	kfree(&hotplug_freq);
}

MODULE_AUTHOR("ByungChang Cha <bc.cha@samsung.com> | Alucard24@XDA");
MODULE_DESCRIPTION("'cpufreq_nightmare' - A dynamic cpufreq/cpuhotplug governor");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_NIGHTMARE
fs_initcall(cpufreq_gov_nightmare_init);
#else
module_init(cpufreq_gov_nightmare_init);
#endif
module_exit(cpufreq_gov_nightmare_exit);
