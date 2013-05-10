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

#ifndef CONFIG_INTELLI_PLUG
#define AUTO_HOTPLUGGING 1
#else
#define AUTO_HOTPLUGGING 0
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#if AUTO_HOTPLUGGING
#define EARLYSUSPEND_HOTPLUGLOCK 1
#define RQ_AVG_TIMER_RATE	20
/*
 * runqueue average
 */
struct runqueue_data {
	unsigned int nr_run_avg;
	unsigned int update_rate;
	int64_t last_time;
	int64_t total_time;
	struct delayed_work work;
	struct workqueue_struct *nr_run_wq;
	spinlock_t lock;
};

static struct runqueue_data *rq_data;
static void rq_work_fn(struct work_struct *work);

static void start_rq_work(void)
{
	rq_data->nr_run_avg = 0;
	rq_data->last_time = 0;
	rq_data->total_time = 0;
	if (rq_data->nr_run_wq == NULL)
		rq_data->nr_run_wq =
			create_singlethread_workqueue("nr_run_avg");

	queue_delayed_work(rq_data->nr_run_wq, &rq_data->work,
			   msecs_to_jiffies(rq_data->update_rate));
	return;
}

static void stop_rq_work(void)
{
	if (rq_data->nr_run_wq)
		cancel_delayed_work(&rq_data->work);
	return;
}

static int __init init_rq_avg(void)
{
	rq_data = kzalloc(sizeof(struct runqueue_data), GFP_KERNEL);
	if (rq_data == NULL) {
		pr_err("%s cannot allocate memory\n", __func__);
		return -ENOMEM;
	}
	spin_lock_init(&rq_data->lock);
	rq_data->update_rate = RQ_AVG_TIMER_RATE;
	INIT_DEFERRABLE_WORK(&rq_data->work, rq_work_fn);

	return 0;
}

static void rq_work_fn(struct work_struct *work)
{
	int64_t time_diff = 0;
	int64_t nr_run = 0;
	unsigned long flags = 0;
	int64_t cur_time = ktime_to_ns(ktime_get());

	spin_lock_irqsave(&rq_data->lock, flags);

	if (rq_data->last_time == 0)
		rq_data->last_time = cur_time;
	if (rq_data->nr_run_avg == 0)
		rq_data->total_time = 0;

	nr_run = nr_running() * 100;
	time_diff = cur_time - rq_data->last_time;
	do_div(time_diff, 1000 * 1000);

	if (time_diff != 0 && rq_data->total_time != 0) {
		nr_run = (nr_run * time_diff) +
			(rq_data->nr_run_avg * rq_data->total_time);
		do_div(nr_run, rq_data->total_time + time_diff);
	}
	rq_data->nr_run_avg = nr_run;
	rq_data->total_time += time_diff;
	rq_data->last_time = cur_time;

	if (rq_data->update_rate != 0)
		queue_delayed_work(rq_data->nr_run_wq, &rq_data->work,
				   msecs_to_jiffies(rq_data->update_rate));

	spin_unlock_irqrestore(&rq_data->lock, flags);
}

static unsigned int get_nr_run_avg(void)
{
	unsigned int nr_run_avg;
	unsigned long flags = 0;

	spin_lock_irqsave(&rq_data->lock, flags);
	nr_run_avg = rq_data->nr_run_avg;
	rq_data->nr_run_avg = 0;
	spin_unlock_irqrestore(&rq_data->lock, flags);

	return nr_run_avg;
}
#endif

/*
 * dbs is used in this file as a shortform for demandbased switching
 * It helps to keep variable names smaller, simpler
 */

#define DEF_SAMPLING_UP_FACTOR		(1)
#define MAX_SAMPLING_UP_FACTOR		(100000)
#define DEF_SAMPLING_DOWN_FACTOR	(1)
#define MAX_SAMPLING_DOWN_FACTOR	(100000)
#define DEF_FREQ_STEP_DEC		(10)

#define DEF_SAMPLING_RATE		(60000)
#define MIN_SAMPLING_RATE		(10000)
#define MAX_HOTPLUG_RATE		(40u)
#define DEF_UP_NR_CPUS			(1)

#define DEF_FREQ_STEP			(20)
#define DEF_START_DELAY			(0)
#define FREQ_FOR_RESPONSIVENESS		(400000)
#define MAX_FREQ_FOR_CALC_INCR		(400000)
#define DEF_FREQ_FOR_CALC_INCR		(200000)
#define MIN_FREQ_FOR_CALC_INCR		(50000)
#define MAX_FREQ_FOR_CALC_DECR		(400000)
#define DEF_FREQ_FOR_CALC_DECR		(200000)
#define MIN_FREQ_FOR_CALC_DECR		(125000)
/* CPU freq will be increased if measured load > inc_cpu_load;*/
#define DEF_INC_CPU_LOAD 		(70)
#define INC_CPU_LOAD_AT_MIN_FREQ	(60)
/* CPU freq will be decreased if measured load < dec_cpu_load;*/
#define DEF_DEC_CPU_LOAD 		(50)
#define DEF_FREQ_UP_BRAKE		(30u)


#if AUTO_HOTPLUGGING
#define DEF_MAX_CPU_LOCK		(0)
#define DEF_MIN_CPU_LOCK		(0)
#define DEF_CPU_UP_RATE			(10)
#define DEF_CPU_DOWN_RATE		(5)
#define HOTPLUG_DOWN_INDEX		(0)
#define HOTPLUG_UP_INDEX		(1)
#define UP_AVG_LOAD			(65u)
#define DOWN_AVG_LOAD			(50u)
#define DEF_HOTPLUG_COMPARE_LEVEL	(0u)
#endif


#if AUTO_HOTPLUGGING
#ifdef CONFIG_MACH_MIDAS
static int hotplug_rq[4][2] = {
	{0, 100}, {100, 200}, {200, 300}, {300, 0}
};

static int hotplug_freq[4][2] = {
	{0, 500000},
	{200000, 500000},
	{200000, 500000},
	{200000, 0}
};
#else
static int hotplug_rq[4][2] = {
	{0, 100}, {100, 200}, {200, 300}, {300, 0}
};

static int hotplug_freq[4][2] = {
	{0, 500000},
	{200000, 500000},
	{200000, 500000},
	{200000, 0}
};
#endif
#endif

static unsigned int min_sampling_rate;
static int earlysuspend = -1;

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

/* Sampling types */
enum {DBS_NORMAL_SAMPLE, DBS_SUB_SAMPLE};

struct cpufreq_nightmare_cpuinfo {
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_iowait;
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_nice;
	struct cpufreq_policy *cur_policy;
	struct delayed_work work;
	struct cpufreq_frequency_table *freq_table;
	unsigned int rate_mult;
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

static struct nightmare_tuners {
	unsigned int sampling_rate;
	unsigned int freq_step_dec;
	unsigned int ignore_nice;
	unsigned int sampling_down_factor;
	unsigned int io_is_busy;
	/* nightmare tuners */
	unsigned int freq_step;
#if AUTO_HOTPLUGGING
	unsigned int cpu_up_rate;
	unsigned int cpu_down_rate;
	unsigned int max_cpu_lock;
	unsigned int min_cpu_lock;
	atomic_t hotplug_lock;
	unsigned int up_avg_load;
	unsigned int down_avg_load;
	unsigned int hotplug_compare_level;
#endif
	unsigned int up_nr_cpus;
	unsigned int dvfs_debug;
	unsigned int max_freq;
	unsigned int min_freq;
	unsigned int inc_cpu_load_at_min_freq;
	unsigned int freq_for_responsiveness;
	unsigned int freq_for_calc_incr;
	unsigned int freq_for_calc_decr;
	unsigned int inc_cpu_load;
	unsigned int dec_cpu_load;
	unsigned int sampling_up_factor;
	unsigned int freq_up_brake;
} nightmare_tuners_ins = {
	.sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR,
	.freq_step_dec = DEF_FREQ_STEP_DEC,
	.ignore_nice = 0,
	.freq_step = DEF_FREQ_STEP,
#if AUTO_HOTPLUGGING
	.cpu_up_rate = DEF_CPU_UP_RATE,
	.cpu_down_rate = DEF_CPU_DOWN_RATE,
	.max_cpu_lock = DEF_MAX_CPU_LOCK,
	.min_cpu_lock = DEF_MIN_CPU_LOCK,
	.hotplug_lock = ATOMIC_INIT(0),
	.up_avg_load = UP_AVG_LOAD,
	.down_avg_load = DOWN_AVG_LOAD,
	.hotplug_compare_level = DEF_HOTPLUG_COMPARE_LEVEL,
#endif
	.up_nr_cpus = DEF_UP_NR_CPUS,
	.dvfs_debug = 0,
	.inc_cpu_load_at_min_freq = INC_CPU_LOAD_AT_MIN_FREQ,
	.freq_for_responsiveness = FREQ_FOR_RESPONSIVENESS,
	.freq_for_calc_incr = DEF_FREQ_FOR_CALC_INCR,
	.freq_for_calc_decr = DEF_FREQ_FOR_CALC_DECR,
	.inc_cpu_load = DEF_INC_CPU_LOAD,
	.dec_cpu_load = DEF_DEC_CPU_LOAD,
	.sampling_up_factor = DEF_SAMPLING_UP_FACTOR,
	.freq_up_brake = DEF_FREQ_UP_BRAKE,
};

/*
 * CPU hotplug lock interface
 */

#if AUTO_HOTPLUGGING
static atomic_t g_hotplug_count = ATOMIC_INIT(0);
static atomic_t g_hotplug_lock = ATOMIC_INIT(0);

static void apply_hotplug_lock(void)
{
	int online, possible, lock, flag;
	struct cpufreq_nightmare_cpuinfo *nightmare_cpuinfo;

	/* do turn_on/off cpus */
	nightmare_cpuinfo = &per_cpu(od_nightmare_cpuinfo, 0); /* from CPU0 */
	online = num_online_cpus();
	possible = num_possible_cpus();
	lock = atomic_read(&g_hotplug_lock);
	flag = lock - online;

	if (lock == 0 || flag == 0)
		return;

	pr_debug("%s online %d possible %d lock %d flag %d %d\n",
		 __func__, online, possible, lock, flag, (int)abs(flag));

	if (flag > 0) {
		cpu_up(1);
	} else {
		cpu_down(1);
	}
}

int cpufreq_nightmare_cpu_lock(int num_core)
{
	int prev_lock;

	if (num_core < 1 || num_core > num_possible_cpus())
		return -EINVAL;

	prev_lock = atomic_read(&g_hotplug_lock);

	if (prev_lock != 0 && prev_lock < num_core)
		return -EINVAL;
	else if (prev_lock == num_core)
		atomic_inc(&g_hotplug_count);

	atomic_set(&g_hotplug_lock, num_core);
	atomic_set(&g_hotplug_count, 1);
	apply_hotplug_lock();

	return 0;
}

int cpufreq_nightmare_cpu_unlock(int num_core)
{
	int prev_lock = atomic_read(&g_hotplug_lock);

	if (prev_lock < num_core)
		return 0;
	else if (prev_lock == num_core)
		atomic_dec(&g_hotplug_count);

	if (atomic_read(&g_hotplug_count) == 0)
		atomic_set(&g_hotplug_lock, 0);

	return 0;
}

void cpufreq_nightmare_min_cpu_lock(unsigned int num_core)
{
	int online, flag;
	struct cpufreq_nightmare_cpuinfo *nightmare_cpuinfo;

	nightmare_tuners_ins.min_cpu_lock = min(num_core, num_possible_cpus());

	nightmare_cpuinfo = &per_cpu(od_nightmare_cpuinfo, 0); /* from CPU0 */
	online = num_online_cpus();
	flag = (int)num_core - online;
	if (flag <= 0)
		return;
	cpu_up(1);
}

void cpufreq_nightmare_min_cpu_unlock(void)
{
	int online, lock, flag;
	struct cpufreq_nightmare_cpuinfo *nightmare_cpuinfo;

	nightmare_tuners_ins.min_cpu_lock = 0;

	nightmare_cpuinfo = &per_cpu(od_nightmare_cpuinfo, 0); /* from CPU0 */
	online = num_online_cpus();
	lock = atomic_read(&g_hotplug_lock);
	if (lock == 0)
		return;
	flag = lock - online;
	if (flag >= 0)
		return;
	cpu_down(1);
}
#endif

/*
 * History of CPU usage
 */
struct cpu_usage {
	unsigned int freq;
	int load[NR_CPUS];
#if AUTO_HOTPLUGGING
	unsigned int rq_avg;
#endif
	unsigned int avg_load;
};

struct cpu_usage_history {
	struct cpu_usage usage[MAX_HOTPLUG_RATE];
	unsigned int num_hist;
	unsigned int last_num_hist;
};

struct cpu_usage_history *hotplug_histories;

static inline u64 get_cpu_idle_time_jiffy(unsigned int cpu, u64 *wall)
{
	u64 idle_time;
	u64 cur_wall_time;
	u64 busy_time;

	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());

	busy_time  = kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];

	idle_time = cur_wall_time - busy_time;
	if (wall)
		*wall = jiffies_to_usecs(cur_wall_time);

	return jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu, cputime64_t *wall)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, NULL);

	if (idle_time == -1ULL)
		return get_cpu_idle_time_jiffy(cpu, wall);
	else
		idle_time += get_cpu_iowait_time_us(cpu, wall);

	return idle_time;
}

static inline cputime64_t get_cpu_iowait_time(unsigned int cpu,
					      cputime64_t *wall)
{
	u64 iowait_time = get_cpu_iowait_time_us(cpu, wall);

	if (iowait_time == -1ULL)
		return 0;

	return iowait_time;
}

/************************** sysfs interface ************************/

static ssize_t show_sampling_rate_min(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", min_sampling_rate);
}

define_one_global_ro(sampling_rate_min);

/* cpufreq_nightmare Governor Tunables */
#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", nightmare_tuners_ins.object);		\
}
show_one(sampling_rate, sampling_rate);
show_one(io_is_busy, io_is_busy);
show_one(sampling_down_factor, sampling_down_factor);
show_one(ignore_nice_load, ignore_nice);
show_one(freq_step_dec, freq_step_dec);
show_one(freq_step, freq_step);
#if AUTO_HOTPLUGGING
show_one(cpu_up_rate, cpu_up_rate);
show_one(cpu_down_rate, cpu_down_rate);
show_one(up_avg_load, up_avg_load);
show_one(max_cpu_lock, max_cpu_lock);
show_one(min_cpu_lock, min_cpu_lock);
show_one(down_avg_load, down_avg_load);
show_one(hotplug_compare_level,hotplug_compare_level);
#endif
show_one(up_nr_cpus, up_nr_cpus);
show_one(dvfs_debug, dvfs_debug);
show_one(inc_cpu_load_at_min_freq, inc_cpu_load_at_min_freq);
show_one(freq_for_responsiveness, freq_for_responsiveness);
show_one(freq_for_calc_incr, freq_for_calc_incr);
show_one(freq_for_calc_decr, freq_for_calc_decr);
show_one(inc_cpu_load, inc_cpu_load);
show_one(dec_cpu_load, dec_cpu_load);
show_one(sampling_up_factor, sampling_up_factor);
show_one(freq_up_brake, freq_up_brake);

#if AUTO_HOTPLUGGING
static ssize_t show_hotplug_lock(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", atomic_read(&g_hotplug_lock));
}

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

show_hotplug_param(hotplug_rq, 1, 1);
show_hotplug_param(hotplug_rq, 2, 0);
#ifndef CONFIG_CPU_EXYNOS4210
show_hotplug_param(hotplug_rq, 2, 1);
show_hotplug_param(hotplug_rq, 3, 0);
show_hotplug_param(hotplug_rq, 3, 1);
show_hotplug_param(hotplug_rq, 4, 0);
#endif

store_hotplug_param(hotplug_freq, 1, 1);
store_hotplug_param(hotplug_freq, 2, 0);
#ifndef CONFIG_CPU_EXYNOS4210
store_hotplug_param(hotplug_freq, 2, 1);
store_hotplug_param(hotplug_freq, 3, 0);
store_hotplug_param(hotplug_freq, 3, 1);
store_hotplug_param(hotplug_freq, 4, 0);
#endif

store_hotplug_param(hotplug_rq, 1, 1);
store_hotplug_param(hotplug_rq, 2, 0);
#ifndef CONFIG_CPU_EXYNOS4210
store_hotplug_param(hotplug_rq, 2, 1);
store_hotplug_param(hotplug_rq, 3, 0);
store_hotplug_param(hotplug_rq, 3, 1);
store_hotplug_param(hotplug_rq, 4, 0);
#endif

define_one_global_rw(hotplug_freq_1_1);
define_one_global_rw(hotplug_freq_2_0);
#ifndef CONFIG_CPU_EXYNOS4210
define_one_global_rw(hotplug_freq_2_1);
define_one_global_rw(hotplug_freq_3_0);
define_one_global_rw(hotplug_freq_3_1);
define_one_global_rw(hotplug_freq_4_0);
#endif

define_one_global_rw(hotplug_rq_1_1);
define_one_global_rw(hotplug_rq_2_0);
#ifndef CONFIG_CPU_EXYNOS4210
define_one_global_rw(hotplug_rq_2_1);
define_one_global_rw(hotplug_rq_3_0);
define_one_global_rw(hotplug_rq_3_1);
define_one_global_rw(hotplug_rq_4_0);
#endif
#endif

static ssize_t store_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	nightmare_tuners_ins.sampling_rate = max(input, min_sampling_rate);
	return count;
}

static ssize_t store_io_is_busy(struct kobject *a, struct attribute *b,
				const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	nightmare_tuners_ins.io_is_busy = !!input;
	return count;
}

static ssize_t store_sampling_down_factor(struct kobject *a,
					  struct attribute *b,
					  const char *buf, size_t count)
{
	unsigned int input, j;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_SAMPLING_DOWN_FACTOR || input < 1)
		return -EINVAL;
	nightmare_tuners_ins.sampling_down_factor = input;

	return count;
}

static ssize_t store_ignore_nice_load(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	unsigned int j;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	if (input == nightmare_tuners_ins.ignore_nice) { /* nothing to do */
		return count;
	}
	nightmare_tuners_ins.ignore_nice = input;

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct cpufreq_nightmare_cpuinfo *nightmare_cpuinfo;
		nightmare_cpuinfo = &per_cpu(od_nightmare_cpuinfo, j);
		nightmare_cpuinfo->prev_cpu_idle =
			get_cpu_idle_time(j, &nightmare_cpuinfo->prev_cpu_wall);
		if (nightmare_tuners_ins.ignore_nice)
			nightmare_cpuinfo->prev_cpu_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];
	}
	return count;
}

static ssize_t store_freq_step_dec(struct kobject *a, struct attribute *b,
				       const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	nightmare_tuners_ins.freq_step_dec = min(input, 100u);
	return count;
}

static ssize_t store_freq_step(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	nightmare_tuners_ins.freq_step = min(input, 100u);
	return count;
}

#if AUTO_HOTPLUGGING
static ssize_t store_cpu_up_rate(struct kobject *a, struct attribute *b,
				 const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	nightmare_tuners_ins.cpu_up_rate = min(input, MAX_HOTPLUG_RATE);
	return count;
}

static ssize_t store_cpu_down_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	nightmare_tuners_ins.cpu_down_rate = min(input, MAX_HOTPLUG_RATE);
	return count;
}

static ssize_t store_max_cpu_lock(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	nightmare_tuners_ins.max_cpu_lock = min(input, num_possible_cpus());
	return count;
}

static ssize_t store_min_cpu_lock(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	if (input == 0)
		cpufreq_nightmare_min_cpu_unlock();
	else
		cpufreq_nightmare_min_cpu_lock(input);
	return count;
}

static ssize_t store_hotplug_lock(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	int prev_lock;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	input = min(input, num_possible_cpus());
	prev_lock = atomic_read(&nightmare_tuners_ins.hotplug_lock);

	if (prev_lock)
		cpufreq_nightmare_cpu_unlock(prev_lock);

	if (input == 0) {
		atomic_set(&g_hotplug_lock, 0);
		atomic_set(&g_hotplug_count, 0);
		atomic_set(&nightmare_tuners_ins.hotplug_lock, 0);
		return count;
	}

	ret = cpufreq_nightmare_cpu_lock(input);
	if (ret) {
		printk(KERN_ERR "[HOTPLUG] already locked with smaller value %d < %d\n",
			atomic_read(&g_hotplug_lock), input);
		return ret;
	}

	atomic_set(&nightmare_tuners_ins.hotplug_lock, input);

	return count;
}
/* up_avg_load */
static ssize_t store_up_avg_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	nightmare_tuners_ins.up_avg_load = max(min(input,100u),10u);
	return count;
}

/* down_avg_load */
static ssize_t store_down_avg_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	nightmare_tuners_ins.down_avg_load = max(min(input,95u),5u);
	return count;
}

/* hotplug_compare_level */
static ssize_t store_hotplug_compare_level(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1 || input < 0 || input > 1)
		return -EINVAL;

	if (input == nightmare_tuners_ins.hotplug_compare_level) { /* nothing to do */
		return count;
	}

	nightmare_tuners_ins.hotplug_compare_level = input;

	return count;
}
#endif

static ssize_t store_up_nr_cpus(struct kobject *a, struct attribute *b,
				const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	nightmare_tuners_ins.up_nr_cpus = min(input, num_possible_cpus());
	return count;
}

static ssize_t store_dvfs_debug(struct kobject *a, struct attribute *b,
				const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	nightmare_tuners_ins.dvfs_debug = input > 0;
	return count;
}

static ssize_t store_inc_cpu_load_at_min_freq(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100) {
		return -EINVAL;
	}
	nightmare_tuners_ins.inc_cpu_load_at_min_freq = min(input,nightmare_tuners_ins.inc_cpu_load);
	return count;
}

static ssize_t store_freq_for_responsiveness(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	nightmare_tuners_ins.freq_for_responsiveness = input;
	return count;
}

static ssize_t store_freq_for_calc_incr(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	if (input > MAX_FREQ_FOR_CALC_INCR)
		input = MAX_FREQ_FOR_CALC_INCR;
	else if (input < MIN_FREQ_FOR_CALC_INCR)
		input = MIN_FREQ_FOR_CALC_INCR;

	nightmare_tuners_ins.freq_for_calc_incr = input;
	return count;
}

static ssize_t store_freq_for_calc_decr(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	if (input > MAX_FREQ_FOR_CALC_DECR)
		input = MAX_FREQ_FOR_CALC_DECR;
	else if (input < MIN_FREQ_FOR_CALC_DECR)
		input = MIN_FREQ_FOR_CALC_DECR;

	nightmare_tuners_ins.freq_for_calc_decr = input;
	return count;
}

/* inc_cpu_load */
static ssize_t store_inc_cpu_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	nightmare_tuners_ins.inc_cpu_load = max(min(input,100u),10u);
	return count;
}

/* dec_cpu_load */
static ssize_t store_dec_cpu_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	nightmare_tuners_ins.dec_cpu_load = max(min(input,95u),5u);
	return count;
}

/* sampling_up_factor */
static ssize_t store_sampling_up_factor(struct kobject *a,
					  struct attribute *b,
					  const char *buf, size_t count)
{
	unsigned int input, j;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_SAMPLING_UP_FACTOR || input < 1)
		return -EINVAL;
	nightmare_tuners_ins.sampling_up_factor = input;
	
	return count;
}

/* freq_up_brake */
static ssize_t store_freq_up_brake(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1 || input < 0 || input > 100)
		return -EINVAL;

	if (input == nightmare_tuners_ins.freq_up_brake) { /* nothing to do */
		return count;
	}

	nightmare_tuners_ins.freq_up_brake = input;

	return count;
}


define_one_global_rw(sampling_rate);
define_one_global_rw(io_is_busy);
define_one_global_rw(sampling_down_factor);
define_one_global_rw(ignore_nice_load);
define_one_global_rw(freq_step_dec);
define_one_global_rw(freq_step);
#if AUTO_HOTPLUGGING
define_one_global_rw(cpu_up_rate);
define_one_global_rw(cpu_down_rate);
define_one_global_rw(max_cpu_lock);
define_one_global_rw(min_cpu_lock);
define_one_global_rw(hotplug_lock);
define_one_global_rw(up_avg_load);
define_one_global_rw(down_avg_load);
define_one_global_rw(hotplug_compare_level);
#endif
define_one_global_rw(up_nr_cpus);
define_one_global_rw(dvfs_debug);
define_one_global_rw(inc_cpu_load_at_min_freq);
define_one_global_rw(freq_for_responsiveness);
define_one_global_rw(freq_for_calc_incr);
define_one_global_rw(freq_for_calc_decr);
define_one_global_rw(inc_cpu_load);
define_one_global_rw(dec_cpu_load);
define_one_global_rw(sampling_up_factor);
define_one_global_rw(freq_up_brake);

static struct attribute *nightmare_attributes[] = {
	&sampling_rate_min.attr,
	&sampling_rate.attr,
	&sampling_down_factor.attr,
	&ignore_nice_load.attr,
	&io_is_busy.attr,
	&freq_step_dec.attr,
	&freq_step.attr,
#if AUTO_HOTPLUGGING
	&cpu_up_rate.attr,
	&cpu_down_rate.attr,
	&max_cpu_lock.attr,
	&min_cpu_lock.attr,
	&hotplug_lock.attr,
	&hotplug_freq_1_1.attr,
	&hotplug_freq_2_0.attr,
#ifndef CONFIG_CPU_EXYNOS4210
	&hotplug_freq_2_1.attr,
	&hotplug_freq_3_0.attr,
	&hotplug_freq_3_1.attr,
	&hotplug_freq_4_0.attr,
#endif
	&hotplug_rq_1_1.attr,
	&hotplug_rq_2_0.attr,
#ifndef CONFIG_CPU_EXYNOS4210
	&hotplug_rq_2_1.attr,
	&hotplug_rq_3_0.attr,
	&hotplug_rq_3_1.attr,
	&hotplug_rq_4_0.attr,
#endif
	&up_avg_load.attr,
	&down_avg_load.attr,
	&hotplug_compare_level.attr,
#endif
	&up_nr_cpus.attr,
	/* priority: hotplug_lock > max_cpu_lock > min_cpu_lock
	   Exception: hotplug_lock on early_suspend uses min_cpu_lock */
	&dvfs_debug.attr,
	&inc_cpu_load_at_min_freq.attr,
	&freq_for_responsiveness.attr,
	&freq_for_calc_incr.attr,
	&freq_for_calc_decr.attr,
	&inc_cpu_load.attr,
	&dec_cpu_load.attr,
	&sampling_up_factor.attr,
	&freq_up_brake.attr,
	NULL
};

static struct attribute_group nightmare_attr_group = {
	.attrs = nightmare_attributes,
	.name = "nightmare",
};

/************************** sysfs end ************************/

#if AUTO_HOTPLUGGING
static void cpu_up_work(struct work_struct *work)
{
	int cpu;
	int online = num_online_cpus();
	int nr_up = nightmare_tuners_ins.up_nr_cpus;
	int min_cpu_lock = nightmare_tuners_ins.min_cpu_lock;
	int hotplug_lock = atomic_read(&g_hotplug_lock);

	if (hotplug_lock && min_cpu_lock)
		nr_up = max(hotplug_lock, min_cpu_lock) - online;
	else if (hotplug_lock)
		nr_up = hotplug_lock - online;
	else if (min_cpu_lock)
		nr_up = max(nr_up, min_cpu_lock - online);

	if (online == 1) {
		printk(KERN_ERR "CPU_UP 3\n");
		cpu_up(num_possible_cpus() - 1);
		nr_up -= 1;
	}

	for_each_cpu_not(cpu, cpu_online_mask) {
		if (nr_up-- == 0)
			break;
		if (cpu == 0)
			continue;
		printk(KERN_ERR "CPU_UP %d\n", cpu);
		cpu_up(cpu);
	}
}

static void cpu_down_work(struct work_struct *work)
{
	int cpu;
	int online = num_online_cpus();
	int nr_down = 1;
	int hotplug_lock = atomic_read(&g_hotplug_lock);

	if (hotplug_lock)
		nr_down = online - hotplug_lock;

	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
		printk(KERN_ERR "CPU_DOWN %d\n", cpu);
		cpu_down(cpu);
		if (--nr_down == 0)
			break;
	}
}
#endif

static void debug_hotplug_check(int which, int rq_avg, int freq,
			 struct cpu_usage *usage)
{
	int cpu;
	printk(KERN_ERR "CHECK %s rq %d.%02d freq %d [", which ? "up" : "down",
	       rq_avg / 100, rq_avg % 100, freq);
	for_each_online_cpu(cpu) {
		printk(KERN_ERR "(%d, %d), ", cpu, usage->load[cpu]);
	}
	printk(KERN_ERR "]\n");
}

#if AUTO_HOTPLUGGING
static int check_up(void)
{
	int num_hist = hotplug_histories->num_hist;
	struct cpu_usage *usage;
	int freq, rq_avg;
	int avg_load;
	int i;
	int up_rate = nightmare_tuners_ins.cpu_up_rate;
	unsigned int up_avg_load = nightmare_tuners_ins.up_avg_load;
	unsigned int hotplug_compare_level = nightmare_tuners_ins.hotplug_compare_level;
	int up_freq, up_rq;
	int min_freq = INT_MAX;
	int min_rq_avg = INT_MAX;
	int min_avg_load = INT_MAX;
	int online;
	int hotplug_lock = atomic_read(&g_hotplug_lock);

	if (hotplug_lock > 0)
		return 0;

	online = num_online_cpus();
	up_freq = hotplug_freq[online - 1][HOTPLUG_UP_INDEX];
	up_rq = hotplug_rq[online - 1][HOTPLUG_UP_INDEX];

	if (online == num_possible_cpus())
		return 0;

	if (nightmare_tuners_ins.max_cpu_lock != 0
		&& online >= nightmare_tuners_ins.max_cpu_lock)
		return 0;

	if (nightmare_tuners_ins.min_cpu_lock != 0
		&& online < nightmare_tuners_ins.min_cpu_lock)
		return 1;

	if (num_hist == 0 || num_hist % up_rate)
		return 0;

	if (hotplug_compare_level == 0) {
		for (i = num_hist - 1; i >= num_hist - up_rate; --i) {
			usage = &hotplug_histories->usage[i];

			freq = usage->freq;
			rq_avg =  usage->rq_avg;
			avg_load = usage->avg_load;

			min_freq = min(min_freq, freq);
			min_rq_avg = min(min_rq_avg, rq_avg);
			min_avg_load = min(min_avg_load, avg_load);

			if (nightmare_tuners_ins.dvfs_debug)
				debug_hotplug_check(1, rq_avg, freq, usage);
		}
	} else {
		usage = &hotplug_histories->usage[num_hist - 1];
		min_freq = usage->freq;
		min_rq_avg = usage->rq_avg;
		min_avg_load = usage->avg_load;
		if (nightmare_tuners_ins.dvfs_debug)
				debug_hotplug_check(1, min_rq_avg, min_freq, usage);
	}

	if (min_freq >= up_freq && min_rq_avg > up_rq) {
		if (online >= 1) {
			if (min_avg_load < up_avg_load)
				return 0;
		}
		printk(KERN_ERR "[HOTPLUG IN] %s %d>=%d && %d>%d\n",
			__func__, min_freq, up_freq, min_rq_avg, up_rq);
		hotplug_histories->num_hist = 0;
		return 1;
	}
	return 0;
}

static int check_down(void)
{
	int num_hist = hotplug_histories->num_hist;
	struct cpu_usage *usage;
	int freq, rq_avg;
	int avg_load;
	int i;
	int down_rate = nightmare_tuners_ins.cpu_down_rate;
	unsigned int down_avg_load = nightmare_tuners_ins.down_avg_load;
	unsigned int hotplug_compare_level = nightmare_tuners_ins.hotplug_compare_level;
	int down_freq, down_rq;
	int max_freq = 0;
	int max_rq_avg = 0;
	int max_avg_load = 0;
	int online;
	int hotplug_lock = atomic_read(&g_hotplug_lock);

	if (hotplug_lock > 0)
		return 0;

	online = num_online_cpus();
	down_freq = hotplug_freq[online - 1][HOTPLUG_DOWN_INDEX];
	down_rq = hotplug_rq[online - 1][HOTPLUG_DOWN_INDEX];

	if (online == 1)
		return 0;

	if (nightmare_tuners_ins.max_cpu_lock != 0
		&& online > nightmare_tuners_ins.max_cpu_lock)
		return 1;

	if (nightmare_tuners_ins.min_cpu_lock != 0
		&& online <= nightmare_tuners_ins.min_cpu_lock)
		return 0;

	if (num_hist == 0 || num_hist % down_rate)
		return 0;

	if (hotplug_compare_level == 0) {
		for (i = num_hist - 1; i >= num_hist - down_rate; --i) {
			usage = &hotplug_histories->usage[i];

			freq = usage->freq;
			rq_avg =  usage->rq_avg;
			avg_load = usage->avg_load;

			max_freq = max(max_freq, freq);
			max_rq_avg = max(max_rq_avg, rq_avg);
			max_avg_load = max(max_avg_load, avg_load);

			if (nightmare_tuners_ins.dvfs_debug)
				debug_hotplug_check(0, rq_avg, freq, usage);
		}
	} else {
		usage = &hotplug_histories->usage[num_hist - 1];
		max_freq = usage->freq;
		max_rq_avg = usage->rq_avg;
		max_avg_load = usage->avg_load;
		if (nightmare_tuners_ins.dvfs_debug)
				debug_hotplug_check(0, max_rq_avg, max_freq, usage);
	}

	if ((max_freq <= down_freq && max_rq_avg <= down_rq) || (online >= 2 && max_avg_load < down_avg_load)) {
		printk(KERN_ERR "[HOTPLUG OUT] %s %d<=%d && %d<%d\n",
			__func__, max_freq, down_freq, max_rq_avg, down_rq);
		hotplug_histories->num_hist = 0;
		return 1;
	}

	return 0;
}
#endif

static void nightmare_check_cpu(struct cpufreq_nightmare_cpuinfo *this_nightmare_cpuinfo)
{
	/* Extrapolated load of this CPU */
	unsigned int load_at_max_freq = 0;
	/* Current load across this CPU */
	unsigned int cur_load = 0;

	struct cpufreq_policy *policy;
	unsigned int j;
	int num_hist = hotplug_histories->num_hist;
#if AUTO_HOTPLUGGING
	int max_hotplug_rate = max(nightmare_tuners_ins.cpu_up_rate,nightmare_tuners_ins.cpu_down_rate);
#else
	int max_hotplug_rate = 20;
#endif
	/* add total_load, avg_load to get average load */
	unsigned int total_load = 0;
	unsigned int avg_load = 0;
#if AUTO_HOTPLUGGING
	int rq_avg = 0;
#endif

	policy = this_nightmare_cpuinfo->cur_policy;

	hotplug_histories->usage[num_hist].freq = policy->cur;
#if AUTO_HOTPLUGGING
	hotplug_histories->usage[num_hist].rq_avg = get_nr_run_avg();

	/* add total_load, avg_load to get average load */
	rq_avg = hotplug_histories->usage[num_hist].rq_avg;
#endif
	/* get last num_hist used */
	hotplug_histories->last_num_hist = num_hist;
	++hotplug_histories->num_hist;

	for_each_cpu(j, policy->cpus) {
		struct cpufreq_nightmare_cpuinfo *j_nightmare_cpuinfo;
		cputime64_t cur_wall_time, cur_idle_time, cur_iowait_time;
		cputime64_t prev_wall_time, prev_idle_time, prev_iowait_time;
		unsigned int idle_time, wall_time, iowait_time;

		j_nightmare_cpuinfo = &per_cpu(od_nightmare_cpuinfo, j);
		
		prev_wall_time = j_nightmare_cpuinfo->prev_cpu_wall;
		prev_idle_time = j_nightmare_cpuinfo->prev_cpu_idle;
		prev_iowait_time = j_nightmare_cpuinfo->prev_cpu_iowait;

		cur_idle_time = get_cpu_idle_time(j, &cur_wall_time);
		cur_iowait_time = get_cpu_iowait_time(j, &cur_wall_time);

		wall_time = (unsigned int)
				(cur_wall_time - prev_wall_time);
		j_nightmare_cpuinfo->prev_cpu_wall = cur_wall_time;

		idle_time = (unsigned int)
				(cur_idle_time - prev_idle_time);
		j_nightmare_cpuinfo->prev_cpu_idle = cur_idle_time;

		iowait_time = (unsigned int)
				(cur_iowait_time - prev_iowait_time);
		j_nightmare_cpuinfo->prev_cpu_iowait = cur_iowait_time;

		if (nightmare_tuners_ins.ignore_nice) {
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
	
		if (nightmare_tuners_ins.io_is_busy && idle_time >= iowait_time)
			idle_time -= iowait_time;

		if (unlikely(!wall_time || wall_time < idle_time)) {
			hotplug_histories->usage[num_hist].load[j] = -1;
			continue;
		}

		cur_load = 100 * (wall_time - idle_time) / wall_time;

		if (cpu_online(j)) {
			total_load += cur_load;
			hotplug_histories->usage[num_hist].load[j] = cur_load;
		} else {
			hotplug_histories->usage[num_hist].load[j] = -1;
		}
		/* calculate the scaled load across CPU */
		load_at_max_freq = (cur_load * policy->cur)/
					policy->cpuinfo.max_freq;

		cpufreq_notify_utilization(policy, load_at_max_freq);
	}
	/* calculate the average load across all related CPUs */
	avg_load = total_load / num_online_cpus();
	hotplug_histories->usage[num_hist].avg_load = avg_load;	

#if AUTO_HOTPLUGGING
	/* Check for CPU hotplug */
	if (check_up()) {
		mutex_unlock(&this_nightmare_cpuinfo->timer_mutex);
		cpu_up(1);
		mutex_lock(&this_nightmare_cpuinfo->timer_mutex);
	}
	else if (check_down()) {
		mutex_unlock(&this_nightmare_cpuinfo->timer_mutex);
		cpu_down(1);
		mutex_lock(&this_nightmare_cpuinfo->timer_mutex);
	}
#endif
	if (hotplug_histories->num_hist == max_hotplug_rate)
		hotplug_histories->num_hist = 0;
}

static void nightmare_check_frequency(struct cpufreq_nightmare_cpuinfo *this_nightmare_cpuinfo)
{
	int j;
	int num_hist = hotplug_histories->last_num_hist;
	int inc_cpu_load = nightmare_tuners_ins.inc_cpu_load;
	int dec_cpu_load = nightmare_tuners_ins.dec_cpu_load;
	unsigned int freq_step = nightmare_tuners_ins.freq_step;
	unsigned int freq_up_brake = nightmare_tuners_ins.freq_up_brake;
	unsigned int freq_step_dec = nightmare_tuners_ins.freq_step_dec;
	unsigned int inc_load=0;
	unsigned int inc_brake=0;
	unsigned int freq_up = 0;
	unsigned int dec_load = 0;
	unsigned int freq_down = 0;
	unsigned int freq_for_calc_incr = nightmare_tuners_ins.freq_for_calc_incr;
	unsigned int freq_for_calc_decr = nightmare_tuners_ins.freq_for_calc_decr;

	for_each_online_cpu(j) {
		struct cpufreq_policy *policy;
		int cur_load = -1;

		cur_load = hotplug_histories->usage[num_hist].load[j];

		policy = cpufreq_cpu_get(j);
		if (!policy) {
			continue;
		}

		/* CPUs Online Scale Frequency*/
		if (policy->cur < nightmare_tuners_ins.freq_for_responsiveness) {
			inc_cpu_load = nightmare_tuners_ins.inc_cpu_load_at_min_freq;
		} else {
			inc_cpu_load = nightmare_tuners_ins.inc_cpu_load;
		}

		/* Check for frequency increase or for frequency decrease */
		if (cur_load >= inc_cpu_load) {
			this_nightmare_cpuinfo->rate_mult = nightmare_tuners_ins.sampling_up_factor;

			/* if we cannot increment the frequency anymore, break out early */
			if (policy->cur == policy->max) {
				cpufreq_cpu_put(policy);
				continue;
			}

			inc_load = ((cur_load * freq_for_calc_incr) / 100) + ((freq_step * freq_for_calc_incr) / 100);
			inc_brake = (freq_up_brake * freq_for_calc_incr) / 100;

			if (inc_brake > inc_load) {
				cpufreq_cpu_put(policy);
				continue;
			} else {
				freq_up = min(policy->cur + (inc_load - inc_brake),policy->max);
			}			

			if (earlysuspend >= 0 && freq_up > policy->max_suspend) {
				freq_up = policy->max_suspend;
			}

			if (freq_up != policy->cur && freq_up <= policy->max) {
				__cpufreq_driver_target(policy, freq_up, CPUFREQ_RELATION_L);
			}

		} else if (cur_load < dec_cpu_load && cur_load > -1) {
			this_nightmare_cpuinfo->rate_mult = nightmare_tuners_ins.sampling_down_factor;

			/* if we cannot reduce the frequency anymore, break out early */
			if (policy->cur == policy->min) {
				cpufreq_cpu_put(policy);
				continue;
			}
	
			dec_load = (((100 - cur_load) * freq_for_calc_decr) / 100) + ((freq_step_dec * freq_for_calc_decr) / 100);

			if (policy->cur > dec_load + policy->min) {
				freq_down = policy->cur - dec_load;
			} else {
				freq_down = policy->min;
			}

			if (earlysuspend >= 0 && freq_down < policy->min_suspend) {
				freq_down = policy->min_suspend;
			}

			if (freq_down != policy->cur) {
				__cpufreq_driver_target(policy, freq_down, CPUFREQ_RELATION_L);
			}
		}
		cpufreq_cpu_put(policy);
	}
	return;
}

static void do_nightmare_timer(struct work_struct *work)
{
	struct cpufreq_nightmare_cpuinfo *nightmare_cpuinfo =
		container_of(work, struct cpufreq_nightmare_cpuinfo, work.work);
	unsigned int cpu = nightmare_cpuinfo->cpu;
	int delay;

	mutex_lock(&nightmare_cpuinfo->timer_mutex);

	nightmare_check_cpu(nightmare_cpuinfo);
	nightmare_check_frequency(nightmare_cpuinfo);
	/* We want all CPUs to do sampling nearly on
	 * same jiffy
	 */
	delay = usecs_to_jiffies(nightmare_tuners_ins.sampling_rate * (nightmare_cpuinfo->rate_mult < 1 ? 1 : nightmare_cpuinfo->rate_mult));

	if (num_online_cpus() > 1)
		delay -= jiffies % delay;

	schedule_delayed_work_on(cpu, &nightmare_cpuinfo->work, delay);
	mutex_unlock(&nightmare_cpuinfo->timer_mutex);
}

static inline void nightmare_timer_init(struct cpufreq_nightmare_cpuinfo *nightmare_cpuinfo)
{
	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(DEF_START_DELAY * 1000 * 1000
				     + nightmare_tuners_ins.sampling_rate);

	if (num_online_cpus() > 1)
		delay -= jiffies % delay;


	INIT_DEFERRABLE_WORK(&nightmare_cpuinfo->work, do_nightmare_timer);
	schedule_delayed_work_on(nightmare_cpuinfo->cpu, &nightmare_cpuinfo->work, delay + 2 * HZ);
}

static inline void nightmare_timer_exit(struct cpufreq_nightmare_cpuinfo *nightmare_cpuinfo)
{
	cancel_delayed_work_sync(&nightmare_cpuinfo->work);
}

#if AUTO_HOTPLUGGING
static int pm_notifier_call(struct notifier_block *this,
			    unsigned long event, void *ptr)
{
	static unsigned int prev_hotplug_lock;
	switch (event) {
	case PM_SUSPEND_PREPARE:
		prev_hotplug_lock = atomic_read(&g_hotplug_lock);
		atomic_set(&g_hotplug_lock, 1);
		apply_hotplug_lock();
		pr_debug("%s enter suspend\n", __func__);
		return NOTIFY_OK;
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		atomic_set(&g_hotplug_lock, prev_hotplug_lock);
		if (prev_hotplug_lock)
			apply_hotplug_lock();
		prev_hotplug_lock = 0;
		pr_debug("%s exit suspend\n", __func__);
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
#if AUTO_HOTPLUGGING
	earlysuspend = atomic_read(&g_hotplug_lock);
	atomic_set(&g_hotplug_lock,(nightmare_tuners_ins.min_cpu_lock) ? nightmare_tuners_ins.min_cpu_lock : 1);
	apply_hotplug_lock();
	stop_rq_work();
#else
	earlysuspend = 1;
#endif
}
static void cpufreq_nightmare_late_resume(struct early_suspend *h)
{
#if AUTO_HOTPLUGGING
	atomic_set(&g_hotplug_lock, earlysuspend);
	earlysuspend = -1;
	apply_hotplug_lock();
	start_rq_work();
#else
	earlysuspend = -1;
#endif
}

static int cpufreq_governor_nightmare(struct cpufreq_policy *policy,
				unsigned int event)
{
	unsigned int cpu = policy->cpu;
	struct cpufreq_nightmare_cpuinfo *this_nightmare_cpuinfo;
	struct cpufreq_frequency_table *freq_table;
	unsigned int j;
	int rc;

	this_nightmare_cpuinfo = &per_cpu(od_nightmare_cpuinfo, cpu);

	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;

		/* SET POLICY SHARED TYPE AND APPLY MASK TO ALL CPUS */
		policy->shared_type = CPUFREQ_SHARED_TYPE_ANY;
		cpumask_setall(policy->cpus);

		nightmare_tuners_ins.max_freq = policy->max;
		nightmare_tuners_ins.min_freq = policy->min;
		hotplug_histories->num_hist = 0;
		hotplug_histories->last_num_hist = 0;
#if AUTO_HOTPLUGGING
		start_rq_work();
#endif
		mutex_lock(&nightmare_mutex);

		nightmare_enable++;
		for_each_cpu(j, policy->cpus) {
			struct cpufreq_nightmare_cpuinfo *j_nightmare_cpuinfo;
			j_nightmare_cpuinfo = &per_cpu(od_nightmare_cpuinfo, j);
			j_nightmare_cpuinfo->cur_policy = policy;

			j_nightmare_cpuinfo->prev_cpu_idle = get_cpu_idle_time(j,
				&j_nightmare_cpuinfo->prev_cpu_wall);
			if (nightmare_tuners_ins.ignore_nice)
				j_nightmare_cpuinfo->prev_cpu_nice =
					kcpustat_cpu(j).cpustat[CPUTIME_NICE];
		}
		this_nightmare_cpuinfo->cpu = cpu;
		this_nightmare_cpuinfo->rate_mult = 1;
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

			min_sampling_rate = MIN_SAMPLING_RATE;
			nightmare_tuners_ins.sampling_rate = DEF_SAMPLING_RATE;
			nightmare_tuners_ins.io_is_busy = 0;
		}
		mutex_unlock(&nightmare_mutex);

		mutex_init(&this_nightmare_cpuinfo->timer_mutex);
		nightmare_timer_init(this_nightmare_cpuinfo);

#if AUTO_HOTPLUGGING
#if !EARLYSUSPEND_HOTPLUGLOCK
		register_pm_notifier(&pm_notifier);
#endif
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
		register_early_suspend(&early_suspend);
#endif
		break;

	case CPUFREQ_GOV_STOP:
#ifdef CONFIG_HAS_EARLYSUSPEND
		unregister_early_suspend(&early_suspend);
#endif
#if AUTO_HOTPLUGGING
#if !EARLYSUSPEND_HOTPLUGLOCK
		unregister_pm_notifier(&pm_notifier);
#endif
#endif

		nightmare_timer_exit(this_nightmare_cpuinfo);

		mutex_lock(&nightmare_mutex);
		mutex_destroy(&this_nightmare_cpuinfo->timer_mutex);

		nightmare_enable--;
		mutex_unlock(&nightmare_mutex);

#if AUTO_HOTPLUGGING
		stop_rq_work();
#endif
		if (!nightmare_enable)
			sysfs_remove_group(cpufreq_global_kobject,
					   &nightmare_attr_group);

		break;

	case CPUFREQ_GOV_LIMITS:
		mutex_lock(&this_nightmare_cpuinfo->timer_mutex);

		for_each_online_cpu(j) {
			struct cpufreq_policy *cpu_policy;

			cpu_policy = cpufreq_cpu_get(j);
			if (!cpu_policy)
				continue;

			if (policy->max < cpu_policy->cur)
				__cpufreq_driver_target(cpu_policy,policy->max,CPUFREQ_RELATION_H);
			else if (policy->min > cpu_policy->cur)
				__cpufreq_driver_target(cpu_policy,policy->min,CPUFREQ_RELATION_L);

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

#if AUTO_HOTPLUGGING
	ret = init_rq_avg();
	if (ret)
		return ret;
#endif

	hotplug_histories = kzalloc(sizeof(struct cpu_usage_history), GFP_KERNEL);
	if (!hotplug_histories) {
		pr_err("%s cannot create hotplug history array\n", __func__);
		ret = -ENOMEM;
		goto err_hist;
	}

	ret = cpufreq_register_governor(&cpufreq_gov_nightmare);
	if (ret)
		goto err_queue;

	early_suspend.suspend = cpufreq_nightmare_early_suspend;
	early_suspend.resume = cpufreq_nightmare_late_resume;
	early_suspend.level = EARLY_SUSPEND_LEVEL_DISABLE_FB;

	return ret;

err_queue:
	kfree(hotplug_histories);
err_hist:
#if AUTO_HOTPLUGGING
	kfree(rq_data);
#endif
	return ret;
}

static void __exit cpufreq_gov_nightmare_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_nightmare);
	kfree(hotplug_histories);
#if AUTO_HOTPLUGGING
	kfree(rq_data);
#endif
}

MODULE_AUTHOR("ByungChang Cha <bc.cha@samsung.com>");
MODULE_DESCRIPTION("'cpufreq_nightmare' - A dynamic cpufreq/cpuhotplug governor");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_NIGHTMARE
fs_initcall(cpufreq_gov_nightmare_init);
#else
module_init(cpufreq_gov_nightmare_init);
#endif
module_exit(cpufreq_gov_nightmare_exit);
