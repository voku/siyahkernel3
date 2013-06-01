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
#define RQ_AVG_TIMER_RATE	20
/*
 * runqueue average
 */

static atomic_t g_rqwork_flag = ATOMIC_INIT(0);

struct nightmare_runqueue_data {
	unsigned int nr_run_avg;
	unsigned int update_rate;
	int64_t last_time;
	int64_t total_time;
	struct delayed_work work;
	struct workqueue_struct *nr_run_wq;
	spinlock_t lock;
};

static struct nightmare_runqueue_data *rq_data;
static void rq_work_fn(struct work_struct *work);

static void start_rq_work(void)
{
	if (atomic_read(&g_rqwork_flag)) {
		return;
	}
	atomic_set(&g_rqwork_flag,1);

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
	if (atomic_read(&g_rqwork_flag) == 0) {
		return;
	}
	atomic_set(&g_rqwork_flag,0);

	if (rq_data->nr_run_wq)
		cancel_delayed_work(&rq_data->work);

	return;
}

static int __init init_rq_avg(void)
{
	rq_data = kzalloc(sizeof(struct nightmare_runqueue_data), GFP_KERNEL);
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

/*
 * dbs is used in this file as a shortform for demandbased switching
 * It helps to keep variable names smaller, simpler
 */

#define DEF_START_DELAY			(0)

#define MAX_HOTPLUG_RATE		(40)
#define HOTPLUG_DOWN_INDEX		(0)
#define HOTPLUG_UP_INDEX		(1)

static unsigned int hotplug_rq[4][2] = {
	{0, 100}, {100, 200}, {200, 300}, {300, 0}
};

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
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_iowait;
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_nice;
	struct cpufreq_policy *cur_policy;
	struct delayed_work work;
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

/* nightmare tuners */
static struct nightmare_tuners {
	unsigned int sampling_rate;
	bool io_is_busy;
	atomic_t hotplug_lock;
	atomic_t hotplug_enable;
	u8 max_cpu_lock;
	u8 min_cpu_lock;
	u8 cpu_up_rate;
	u8 cpu_down_rate;
	bool hotplug_compare_level;
	u8 up_avg_load;
	u8 down_avg_load;
	bool ignore_nice;
	u8 inc_cpu_load_at_min_freq;
	u8 inc_cpu_load;
	u8 dec_cpu_load;
	unsigned int freq_for_responsiveness;
	unsigned int freq_for_responsiveness_max;
	u8 freq_up_brake_at_min_freq;
	u8 freq_up_brake;
	u8 freq_step_at_min_freq;
	u8 freq_step;
	u8 freq_step_dec;
	u8 freq_step_dec_at_max_freq;
	unsigned int freq_for_calc_incr;
	unsigned int freq_for_calc_decr;
	u8 above_scaling_freq_step;
	u8 below_scaling_freq_step;
	bool dvfs_debug;
	bool earlysuspend;
} nightmare_tuners_ins = {
	.hotplug_lock = ATOMIC_INIT(0),
	.hotplug_enable = ATOMIC_INIT(0),
	.max_cpu_lock = 0,
	.min_cpu_lock = 0,
	.cpu_up_rate = 10,
	.cpu_down_rate = 5,
	.hotplug_compare_level = 1,
	.up_avg_load = 65,
	.down_avg_load = 30,
	.ignore_nice = 0,
	.inc_cpu_load_at_min_freq = 60,
	.inc_cpu_load = 70,
	.dec_cpu_load = 50,
	.freq_for_responsiveness = 400000,
	.freq_for_responsiveness_max = 1200000,
	.freq_step_at_min_freq = 20,
	.freq_step = 20,
	.freq_up_brake_at_min_freq = 30,
	.freq_up_brake = 30,
	.freq_step_dec = 10,
	.freq_step_dec_at_max_freq = 10,
	.freq_for_calc_incr = 200000,
	.freq_for_calc_decr = 200000,
	.above_scaling_freq_step = 0,
	.below_scaling_freq_step = 0,
	.dvfs_debug = 0,
	.earlysuspend = 0,
};

/*
 * CPU hotplug lock interface
 */

static void apply_hotplug_lock(void)
{
	short online, possible, lock, flag;

	/* do turn_on/off cpus */
	online = (short)num_online_cpus();
	possible = (short)num_possible_cpus();
	lock = (short)atomic_read(&nightmare_tuners_ins.hotplug_lock);
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

/*
 * History of CPU usage
 */
struct nightmare_cpu_usage {
	unsigned int freq;
	u8 load[NR_CPUS];
	unsigned int rq_avg;
};

struct nightmare_cpu_usage_history {
	struct nightmare_cpu_usage usage[MAX_HOTPLUG_RATE];
	u8 num_hist;
	u8 last_num_hist;
};

static struct nightmare_cpu_usage_history *hotplug_histories;

static inline cputime64_t get_cpu_iowait_time(unsigned int cpu,
					      cputime64_t *wall)
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
	return sprintf(buf, "%u\n", nightmare_tuners_ins.object);		\
}

show_one(sampling_rate, sampling_rate);
show_one(io_is_busy, io_is_busy);
show_one(max_cpu_lock, max_cpu_lock);
show_one(min_cpu_lock, min_cpu_lock);
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
show_one(above_scaling_freq_step, above_scaling_freq_step);
show_one(below_scaling_freq_step, below_scaling_freq_step);
show_one(dvfs_debug, dvfs_debug);


static ssize_t show_hotplug_enable(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", atomic_read(&nightmare_tuners_ins.hotplug_enable));
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

/* sampling_rate */
static ssize_t store_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	nightmare_tuners_ins.sampling_rate = max(input, 10000u);

	return count;
}

/* io_is_busy */
static ssize_t store_io_is_busy(struct kobject *a, struct attribute *b,
				const char *buf, size_t count)
{
	unsigned short input;
	int ret;

	ret = sscanf(buf, "%hu", &input);
	if (ret != 1)
		return -EINVAL;

	nightmare_tuners_ins.io_is_busy = !!input;

	return count;
}

/* hotplug_enable */
static ssize_t store_hotplug_enable(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned short input;
	int ret;

	ret = sscanf(buf, "%hu", &input);
	if (ret != 1)
		return -EINVAL;

	input = input > 0; 

	if (atomic_read(&nightmare_tuners_ins.hotplug_enable) == input)
		return count;

	if (input > 0) {
		apply_hotplug_lock();
		start_rq_work();
	} else {
		stop_rq_work();
	}
	atomic_set(&nightmare_tuners_ins.hotplug_enable, input);

	return count;
}

/* max_cpu_lock */
static ssize_t store_max_cpu_lock(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	u8 input;
	int ret;
	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > num_possible_cpus())
		input = num_possible_cpus();

	nightmare_tuners_ins.max_cpu_lock = input;

	return count;
}

/* min_cpu_lock */
static ssize_t store_min_cpu_lock(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	u8 input;
	int ret;
	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > num_possible_cpus())
		input = num_possible_cpus();

	nightmare_tuners_ins.min_cpu_lock = input;

	return count;
}

/* cpu_up_rate */
static ssize_t store_cpu_up_rate(struct kobject *a, struct attribute *b,
				 const char *buf, size_t count)
{
	u8 input;
	int ret;
	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > MAX_HOTPLUG_RATE)
		input = MAX_HOTPLUG_RATE;

	if (input == nightmare_tuners_ins.cpu_up_rate)
		return count;

	nightmare_tuners_ins.cpu_up_rate = input;

	return count;
}

/* cpu_down_rate */
static ssize_t store_cpu_down_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	u8 input;
	int ret;

	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > MAX_HOTPLUG_RATE)
		input = MAX_HOTPLUG_RATE;

	if (input == nightmare_tuners_ins.cpu_down_rate)
		return count;

	nightmare_tuners_ins.cpu_down_rate = input;
	return count;
}

/* hotplug_compare_level */
static ssize_t store_hotplug_compare_level(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned short input;
	int ret;

	ret = sscanf(buf, "%hu", &input);
	if (ret != 1)
		return -EINVAL;

	input = (input > 0);

	if (input == nightmare_tuners_ins.hotplug_compare_level) { /* nothing to do */
		return count;
	}

	nightmare_tuners_ins.hotplug_compare_level = input;

	return count;
}

/* up_avg_load */
static ssize_t store_up_avg_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	u8 input;
	int ret;

	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;
	else if (input < 10)
		input = 10;

	if (input == nightmare_tuners_ins.up_avg_load)
		return count;

	nightmare_tuners_ins.up_avg_load = input;

	return count;
}

/* down_avg_load */
static ssize_t store_down_avg_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	u8 input;
	int ret;

	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;
	
	if (input > 95)
		input = 95;
	else if (input < 5)
		input = 5;

	if (input == nightmare_tuners_ins.down_avg_load)
		return count;	

	nightmare_tuners_ins.down_avg_load = input;

	return count;
}

/* ignore_nice_load */
static ssize_t store_ignore_nice_load(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned short input;
	int ret;
	unsigned int j;

	ret = sscanf(buf, "%hu", &input);
	if (ret != 1)
		return -EINVAL;

	input = input > 0;

	if (input == nightmare_tuners_ins.ignore_nice) /* nothing to do */
		return count;

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

/* inc_cpu_load_at_min_freq */
static ssize_t store_inc_cpu_load_at_min_freq(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	u8 input;
	int ret;

	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1) {
		return -EINVAL;
	}

	input = min(input,nightmare_tuners_ins.inc_cpu_load);

	if (input == nightmare_tuners_ins.inc_cpu_load_at_min_freq)
		return count;

	nightmare_tuners_ins.inc_cpu_load_at_min_freq = input;

	return count;
}

/* inc_cpu_load */
static ssize_t store_inc_cpu_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	u8 input;
	int ret;

	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;
	else if(input < 10)
		input = 10;

	if (input == nightmare_tuners_ins.inc_cpu_load)
		return count;

	nightmare_tuners_ins.inc_cpu_load = input;

	return count;
}

/* dec_cpu_load */
static ssize_t store_dec_cpu_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	u8 input;
	int ret;

	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 95)
		input = 95;
	else if(input < 5)
		input = 5;

	if (input == nightmare_tuners_ins.dec_cpu_load)
		return count;

	nightmare_tuners_ins.dec_cpu_load = input;

	return count;
}

/* freq_for_responsiveness */
static ssize_t store_freq_for_responsiveness(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input == nightmare_tuners_ins.freq_for_responsiveness)
		return count;

	nightmare_tuners_ins.freq_for_responsiveness = input;

	return count;
}

/* freq_for_responsiveness_max */
static ssize_t store_freq_for_responsiveness_max(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input == nightmare_tuners_ins.freq_for_responsiveness_max)
		return count;

	nightmare_tuners_ins.freq_for_responsiveness_max = input;

	return count;
}

/* freq_step_at_min_freq */
static ssize_t store_freq_step_at_min_freq(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	unsigned short input;
	int ret;

	ret = sscanf(buf, "%hu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	if (input == nightmare_tuners_ins.freq_step_at_min_freq)
		return count;

	nightmare_tuners_ins.freq_step_at_min_freq = input;

	return count;
}

/* freq_step */
static ssize_t store_freq_step(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	unsigned short input;
	int ret;

	ret = sscanf(buf, "%hu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	if (input == nightmare_tuners_ins.freq_step)
		return count;

	nightmare_tuners_ins.freq_step = input;

	return count;
}

/* freq_up_brake_at_min_freq */
static ssize_t store_freq_up_brake_at_min_freq(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	u8 input;
	int ret;

	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	if (input == nightmare_tuners_ins.freq_up_brake_at_min_freq)/* nothing to do */
		return count;

	nightmare_tuners_ins.freq_up_brake_at_min_freq = input;

	return count;
}

/* freq_up_brake */
static ssize_t store_freq_up_brake(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	u8 input;
	int ret;

	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	if (input == nightmare_tuners_ins.freq_up_brake) /* nothing to do */
		return count;

	nightmare_tuners_ins.freq_up_brake = input;

	return count;
}

/* freq_step_dec */
static ssize_t store_freq_step_dec(struct kobject *a, struct attribute *b,
				       const char *buf, size_t count)
{
	u8 input;
	int ret;

	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	if (input == nightmare_tuners_ins.freq_step_dec)
		return count;

	nightmare_tuners_ins.freq_step_dec = input;

	return count;
}

/* freq_step_dec_at_max_freq */
static ssize_t store_freq_step_dec_at_max_freq(struct kobject *a, struct attribute *b,
				       const char *buf, size_t count)
{
	u8 input;
	int ret;

	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	if (input == nightmare_tuners_ins.freq_step_dec_at_max_freq)
		return count;

	nightmare_tuners_ins.freq_step_dec_at_max_freq = input;

	return count;
}

/* freq_for_calc_incr */
static ssize_t store_freq_for_calc_incr(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,400000u),50000u);

	if (input == nightmare_tuners_ins.freq_for_calc_incr)
		return count;

	nightmare_tuners_ins.freq_for_calc_incr = input;

	return count;
}

/* freq_for_calc_decr */
static ssize_t store_freq_for_calc_decr(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,400000u),125000u);

	if (input == nightmare_tuners_ins.freq_for_calc_decr)
		return count;

	nightmare_tuners_ins.freq_for_calc_decr = input;

	return count;
}

/* above_scaling_freq_step */
static ssize_t store_above_scaling_freq_step(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	u8 input;
	int ret;

	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 99)
		input = 99;

	if (input == nightmare_tuners_ins.above_scaling_freq_step)
		return count;

	nightmare_tuners_ins.above_scaling_freq_step = input;

	return count;
}

/* below_scaling_freq_step */
static ssize_t store_below_scaling_freq_step(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	u8 input;
	int ret;

	ret = sscanf(buf, "%hhu", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 99)
		input = 99;

	if (input == nightmare_tuners_ins.below_scaling_freq_step)
		return count;

	nightmare_tuners_ins.below_scaling_freq_step = input;

	return count;
}

/* dvfs_debug */
static ssize_t store_dvfs_debug(struct kobject *a, struct attribute *b,
				const char *buf, size_t count)
{
	unsigned short input;
	int ret;

	ret = sscanf(buf, "%hu", &input);
	if (ret != 1)
		return -EINVAL;

	nightmare_tuners_ins.dvfs_debug = input > 0;

	return count;
}

define_one_global_rw(sampling_rate);
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
define_one_global_rw(above_scaling_freq_step);
define_one_global_rw(below_scaling_freq_step);
define_one_global_rw(dvfs_debug);

static struct attribute *nightmare_attributes[] = {
	&sampling_rate.attr,
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
	&hotplug_rq_1_1.attr,
	&hotplug_rq_2_0.attr,
#ifndef CONFIG_CPU_EXYNOS4210
	&hotplug_rq_2_1.attr,
	&hotplug_rq_3_0.attr,
	&hotplug_rq_3_1.attr,
	&hotplug_rq_4_0.attr,
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
	&above_scaling_freq_step.attr,
	&below_scaling_freq_step.attr,
	&dvfs_debug.attr,
	NULL
};

static struct attribute_group nightmare_attr_group = {
	.attrs = nightmare_attributes,
	.name = "nightmare",
};

/************************** sysfs end ************************/

static void debug_hotplug_check(int which, int rq_avg, unsigned int freq,
			 struct nightmare_cpu_usage *usage)
{
	int cpu;
	printk(KERN_ERR "CHECK %s rq %d.%02d freq %u [", which ? "up" : "down",
	       rq_avg / 100, rq_avg % 100, freq);
	for_each_online_cpu(cpu) {
		printk(KERN_ERR "(%d, %hhu), ", cpu, usage->load[cpu]);
	}
	printk(KERN_ERR "]\n");
}

static int check_up(void)
{
	u8 num_hist = hotplug_histories->num_hist;
	struct nightmare_cpu_usage *usage;
	unsigned int freq, rq_avg;
	u8 avg_load;
	int i;
	u8 up_rate = nightmare_tuners_ins.cpu_up_rate;
	u8 up_avg_load = nightmare_tuners_ins.up_avg_load;
	bool hotplug_compare_level = nightmare_tuners_ins.hotplug_compare_level;
	unsigned int up_freq, up_rq;
	unsigned int min_freq = UINT_MAX;
	unsigned int min_rq_avg = UINT_MAX;
	u8 min_avg_load = 100;
	short online;

	if (atomic_read(&nightmare_tuners_ins.hotplug_lock) > 0)
		return 0;

	online = (short)num_online_cpus();
	up_freq = hotplug_freq[online - 1][HOTPLUG_UP_INDEX];
	up_rq = hotplug_rq[online - 1][HOTPLUG_UP_INDEX];

	if (online == (short)num_possible_cpus())
		return 0;

	if (unlikely(nightmare_tuners_ins.max_cpu_lock != 0 
		&& online >= nightmare_tuners_ins.max_cpu_lock))
		return 0;

	if (unlikely(nightmare_tuners_ins.min_cpu_lock != 0
		&& online < nightmare_tuners_ins.min_cpu_lock))
		return 1;

	if (num_hist == 0 || num_hist % up_rate)
		return 0;

	if (hotplug_compare_level) {
		usage = &hotplug_histories->usage[num_hist - 1];
		min_freq = usage->freq;
		min_rq_avg = usage->rq_avg;
		min_avg_load = usage->load[0];
		if (unlikely(nightmare_tuners_ins.dvfs_debug))
				debug_hotplug_check(1, min_rq_avg, min_freq, usage);

	} else {
		for (i = num_hist - 1; i >= num_hist - up_rate; --i) {
			usage = &hotplug_histories->usage[i];
			freq = usage->freq;
			rq_avg =  usage->rq_avg;
			avg_load = usage->load[0];
			min_freq = min(min_freq, freq);
			min_rq_avg = min(min_rq_avg, rq_avg);
			min_avg_load = min(min_avg_load, avg_load);
			if (unlikely(nightmare_tuners_ins.dvfs_debug))
				debug_hotplug_check(1, rq_avg, freq, usage);
		}
	}

	if (min_avg_load < up_avg_load)
		return 0;

	if (min_freq >= up_freq && min_rq_avg > up_rq) {
		printk(KERN_ERR "[HOTPLUG IN] %s %d>=%d && %d>%d\n",
			__func__, min_freq, up_freq, min_rq_avg, up_rq);
		hotplug_histories->num_hist = 0;
		return 1;
	}
	return 0;
}

static int check_down(void)
{
	u8 num_hist = hotplug_histories->num_hist;
	struct nightmare_cpu_usage *usage;
	unsigned int freq, rq_avg;
	u8 avg_load;
	int i;
	u8 down_rate = nightmare_tuners_ins.cpu_down_rate;
	u8 down_avg_load = nightmare_tuners_ins.down_avg_load;
	bool hotplug_compare_level = nightmare_tuners_ins.hotplug_compare_level;
	unsigned int down_freq, down_rq;
	unsigned int max_freq = 0;
	unsigned int max_rq_avg = 0;
	u8 max_avg_load = 0;
	short online;

	if (atomic_read(&nightmare_tuners_ins.hotplug_lock) > 0)
		return 0;

	online = (short)num_online_cpus();
	down_freq = hotplug_freq[online - 1][HOTPLUG_DOWN_INDEX];
	down_rq = hotplug_rq[online - 1][HOTPLUG_DOWN_INDEX];

	if (online == 1)
		return 0;

	if (unlikely(nightmare_tuners_ins.max_cpu_lock != 0
		&& online > nightmare_tuners_ins.max_cpu_lock))
		return 1;

	if (unlikely(nightmare_tuners_ins.min_cpu_lock != 0
		&& online <= nightmare_tuners_ins.min_cpu_lock))
		return 0;

	if (num_hist == 0 || num_hist % down_rate)
		return 0;

	if (hotplug_compare_level) {
		usage = &hotplug_histories->usage[num_hist - 1];
		max_freq = usage->freq;
		max_rq_avg = usage->rq_avg;
		max_avg_load = usage->load[1];
		if (unlikely(nightmare_tuners_ins.dvfs_debug))
				debug_hotplug_check(0, max_rq_avg, max_freq, usage);
	} else {
		for (i = num_hist - 1; i >= num_hist - down_rate; --i) {
			usage = &hotplug_histories->usage[i];
			freq = usage->freq;
			rq_avg =  usage->rq_avg;
			avg_load = usage->load[1];
			max_freq = max(max_freq, freq);
			max_rq_avg = max(max_rq_avg, rq_avg);
			max_avg_load = max(max_avg_load, avg_load);
			if (unlikely(nightmare_tuners_ins.dvfs_debug))
				debug_hotplug_check(0, rq_avg, freq, usage);
		}
	}

	if ((max_freq <= down_freq && max_rq_avg <= down_rq) 
		|| (max_avg_load < down_avg_load)) {
		printk(KERN_ERR "[HOTPLUG OUT] %s %d<=%d && %d<%d\n",
			__func__, max_freq, down_freq, max_rq_avg, down_rq);
		hotplug_histories->num_hist = 0;
		return 1;
	}

	return 0;
}

static void nightmare_check_cpu(struct cpufreq_nightmare_cpuinfo *this_nightmare_cpuinfo)
{
	struct cpufreq_policy *policy;
	unsigned int j;
	u8 num_hist = hotplug_histories->num_hist;
	u8 max_hotplug_rate = max(nightmare_tuners_ins.cpu_up_rate,nightmare_tuners_ins.cpu_down_rate);
	/* add total_load, avg_load to get average load */
	unsigned int rq_avg = 0;
	bool hotplug_enable = atomic_read(&nightmare_tuners_ins.hotplug_enable) > 0;

	policy = this_nightmare_cpuinfo->cur_policy;
	hotplug_histories->usage[num_hist].freq = policy->cur;

	if (hotplug_enable) {
		hotplug_histories->usage[num_hist].rq_avg = get_nr_run_avg();
		/* add total_load, avg_load to get average load */
		rq_avg = hotplug_histories->usage[num_hist].rq_avg;
	}

	/* get last num_hist used */
	hotplug_histories->last_num_hist = num_hist;
	++hotplug_histories->num_hist;

	for_each_cpu(j, policy->cpus) {
		struct cpufreq_nightmare_cpuinfo *j_nightmare_cpuinfo;
		cputime64_t cur_wall_time, cur_idle_time, cur_iowait_time;
		cputime64_t prev_wall_time, prev_idle_time, prev_iowait_time;
		unsigned int idle_time, wall_time, iowait_time;
		/* Extrapolated load of this CPU */
		unsigned int load_at_max_freq = 0;
		/* Current load across this CPU */
		u8 cur_load = 0;

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

		if (unlikely(nightmare_tuners_ins.ignore_nice)) {
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

		if (!wall_time || wall_time <= idle_time) {
			hotplug_histories->usage[num_hist].load[j] = 0;
			continue;
		}

		cur_load = (u8)(100 * (wall_time - idle_time) / wall_time);

		if (cpu_online(j)) {
			hotplug_histories->usage[num_hist].load[j] = cur_load;
		} else {
			hotplug_histories->usage[num_hist].load[j] = 0;
		}
		/* calculate the scaled load across CPU */
		load_at_max_freq = (cur_load * policy->cur)/
					policy->cpuinfo.max_freq;
		cpufreq_notify_utilization(policy, load_at_max_freq);
	}

	if (hotplug_enable) {
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
	}
	if (hotplug_histories->num_hist == max_hotplug_rate)
		hotplug_histories->num_hist = 0;
}

static unsigned int nightmare_frequency_adjust(unsigned int next_freq, unsigned int cur_freq, unsigned int min_freq, unsigned int max_freq, u8 scaling_freq_step)
{
	unsigned int adjust_freq = 0;

	if (next_freq >= max_freq)
		return max_freq;
	else if (next_freq <= min_freq)
		return min_freq;

	adjust_freq = (next_freq / 100000) * 100000;

	/* Avoid to manage freq with above_scaling_freq_step or below_scaling_freq_step */
	if (adjust_freq == cur_freq)
		return cur_freq;

	if (next_freq % 100000 > (unsigned int)(scaling_freq_step * 1000))
		adjust_freq += 100000;

	return adjust_freq;
		
}

static void nightmare_check_frequency(struct cpufreq_nightmare_cpuinfo *this_nightmare_cpuinfo)
{
	int j;
	u8 num_hist = hotplug_histories->last_num_hist;
	bool earlysuspend = nightmare_tuners_ins.earlysuspend;

	for_each_online_cpu(j) {
		struct cpufreq_policy *policy;
		u8 cur_load = 0;
		u8 inc_cpu_load = 0;
		u8 freq_step = 0;
		u8 freq_up_brake = 0;
		u8 freq_step_dec = 0;
		unsigned int inc_load=0;
		unsigned int inc_brake=0;
		unsigned int freq_up = 0;
		unsigned int dec_load = 0;
		unsigned int freq_down = 0;
		unsigned int min_freq = 0;
		unsigned int max_freq = 0;

		policy = cpufreq_cpu_get(j);
		if (!policy) {
			continue;
		}
		if (earlysuspend) {
			min_freq = min(policy->min_suspend,policy->min);
			max_freq = min(policy->max_suspend,policy->max);
		} else {
			min_freq = policy->min;
			max_freq = policy->max;
		}

		cur_load = hotplug_histories->usage[num_hist].load[j];
		/* CPUs Online Scale Frequency*/
		if (policy->cur < nightmare_tuners_ins.freq_for_responsiveness) {
			inc_cpu_load = nightmare_tuners_ins.inc_cpu_load_at_min_freq;
			freq_step = nightmare_tuners_ins.freq_step_at_min_freq;
			freq_up_brake = nightmare_tuners_ins.freq_up_brake_at_min_freq;
		} else {
			inc_cpu_load = nightmare_tuners_ins.inc_cpu_load;
			freq_step = nightmare_tuners_ins.freq_step;
			freq_up_brake = nightmare_tuners_ins.freq_up_brake;
		}
		if (policy->cur > nightmare_tuners_ins.freq_for_responsiveness_max) {
			freq_step_dec = nightmare_tuners_ins.freq_step_dec_at_max_freq;
		} else {
			freq_step_dec = nightmare_tuners_ins.freq_step_dec;
		}
		/* Check for frequency increase or for frequency decrease */
		if (cur_load >= inc_cpu_load) {
			/* if we cannot increment the frequency anymore, break out early */
			if (policy->cur == max_freq) {
				cpufreq_cpu_put(policy);
				continue;
			}
			inc_load = (((unsigned int)cur_load * nightmare_tuners_ins.freq_for_calc_incr) / 100) + (((unsigned int)freq_step * nightmare_tuners_ins.freq_for_calc_incr) / 100);
			inc_brake = ((unsigned int)freq_up_brake * nightmare_tuners_ins.freq_for_calc_incr) / 100;
			if (unlikely(inc_brake > inc_load)) {
				cpufreq_cpu_put(policy);
				continue;
			} else {
				freq_up = policy->cur + (inc_load - inc_brake);
			}			
			freq_up = nightmare_frequency_adjust(freq_up, policy->cur, min_freq, max_freq, nightmare_tuners_ins.above_scaling_freq_step);
			if (freq_up != policy->cur) {
				__cpufreq_driver_target(policy, freq_up, CPUFREQ_RELATION_L);
			}
			cpufreq_cpu_put(policy);
			continue;
		} else if (cur_load < nightmare_tuners_ins.dec_cpu_load) {
			/* if we cannot reduce the frequency anymore, break out early */
			if (policy->cur == min_freq) {
				cpufreq_cpu_put(policy);
				continue;
			}
			dec_load = (((100 - (unsigned int)cur_load) * nightmare_tuners_ins.freq_for_calc_decr) / 100) + (((unsigned int)freq_step_dec * nightmare_tuners_ins.freq_for_calc_decr) / 100);
			if (policy->cur >= dec_load + min_freq) {
				freq_down = policy->cur - dec_load;
			} else {
				freq_down = min_freq;
			}
			freq_down = nightmare_frequency_adjust(freq_down, policy->cur, min_freq, max_freq, nightmare_tuners_ins.below_scaling_freq_step);

			if (freq_down < policy->cur) {
				__cpufreq_driver_target(policy, freq_down, CPUFREQ_RELATION_L);
			}
			cpufreq_cpu_put(policy);
			continue;
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
	delay = usecs_to_jiffies(nightmare_tuners_ins.sampling_rate);

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

#if !EARLYSUSPEND_HOTPLUGLOCK
static int pm_notifier_call(struct notifier_block *this,
			    unsigned long event, void *ptr)
{
	static unsigned short prev_hotplug_lock;
	bool hotplug_enable = (atomic_read(&nightmare_tuners_ins.hotplug_enable) > 0);
	switch (event) {
	case PM_SUSPEND_PREPARE:
		if (hotplug_enable) {
			prev_hotplug_lock = atomic_read(&nightmare_tuners_ins.hotplug_lock);
			atomic_set(&nightmare_tuners_ins.hotplug_lock, 1);
			apply_hotplug_lock();
			pr_debug("%s enter suspend\n", __func__);
		}
		return NOTIFY_OK;
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		if (hotplug_enable) {
			atomic_set(&nightmare_tuners_ins.hotplug_lock, prev_hotplug_lock);
			if (prev_hotplug_lock)
				apply_hotplug_lock();
			prev_hotplug_lock = 0;
			pr_debug("%s exit suspend\n", __func__);
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
	if (atomic_read(&nightmare_tuners_ins.hotplug_enable) > 0) {
		atomic_set(&nightmare_tuners_ins.hotplug_lock,(nightmare_tuners_ins.min_cpu_lock) ? nightmare_tuners_ins.min_cpu_lock : 1);
		nightmare_tuners_ins.earlysuspend = 1;
		apply_hotplug_lock();
		stop_rq_work();
	} else {
		nightmare_tuners_ins.earlysuspend = 1;
	}
}
static void cpufreq_nightmare_late_resume(struct early_suspend *h)
{
	if (atomic_read(&nightmare_tuners_ins.hotplug_enable) > 0) {
		atomic_set(&nightmare_tuners_ins.hotplug_lock, 0);
		nightmare_tuners_ins.earlysuspend = 0;
		apply_hotplug_lock();
		start_rq_work();
	} else {
		nightmare_tuners_ins.earlysuspend = 0;
	}
}

static int cpufreq_governor_nightmare(struct cpufreq_policy *policy,
				unsigned int event)
{
	unsigned int cpu = policy->cpu;
	struct cpufreq_nightmare_cpuinfo *this_nightmare_cpuinfo;
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

		hotplug_histories->num_hist = 0;
		hotplug_histories->last_num_hist = 0;

		start_rq_work();

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
			nightmare_tuners_ins.sampling_rate = 60000;
			nightmare_tuners_ins.io_is_busy = 0;
			/* 
				set default hotplug_enable var when nightmare is starting.
				Default = 1
			*/
			nightmare_tuners_ins.earlysuspend = 0;
			atomic_set(&nightmare_tuners_ins.hotplug_lock, 0);			
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

		stop_rq_work();

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
		for_each_online_cpu(j) {
			struct cpufreq_policy *cpu_policy;
			unsigned int min_freq = 0;
			unsigned int max_freq = 0;

			cpu_policy = cpufreq_cpu_get(j);
			if (!cpu_policy) {
				continue;
			}

			if (nightmare_tuners_ins.earlysuspend) {
				min_freq = min(policy->min_suspend,policy->min);
				max_freq = min(policy->max_suspend,policy->max);
			} else {
				min_freq = policy->min;
				max_freq = policy->max;
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

	ret = init_rq_avg();
	if (ret)
		return ret;

	hotplug_histories = kzalloc(sizeof(struct nightmare_cpu_usage_history), GFP_KERNEL);
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
	kfree(rq_data);
	return ret;
}

static void __exit cpufreq_gov_nightmare_exit(void)
{
	unsigned int i;

	cpufreq_unregister_governor(&cpufreq_gov_nightmare);
	for_each_possible_cpu(i) {
		struct cpufreq_nightmare_cpuinfo *this_nightmare_cpuinfo =
			&per_cpu(od_nightmare_cpuinfo, i);
		mutex_destroy(&this_nightmare_cpuinfo->timer_mutex);
	}
	kfree(hotplug_histories);
	kfree(rq_data);
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
