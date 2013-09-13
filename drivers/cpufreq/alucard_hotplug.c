/*
 * Author: Alucard_24@XDA
 *
 * Copyright 2012 Alucard_24@XDA
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
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/tick.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>

static DEFINE_MUTEX(alucard_hotplug_mutex);

static struct delayed_work alucard_hotplug_work;

struct hotplug_cpuinfo {
	unsigned long prev_cpu_user;
	unsigned long prev_cpu_system;
	unsigned long prev_cpu_others;
	unsigned long prev_cpu_idle;
	unsigned long prev_cpu_iowait;
	spinlock_t lock;
};

static DEFINE_PER_CPU(struct hotplug_cpuinfo, od_hotplug_cpuinfo);

static unsigned int alucard_hotplug_enable;

static struct hotplug_tuners {
	atomic_t hotplug_sampling_rate;
	atomic_t hotplug_enable;
	atomic_t cpu_up_rate;
	atomic_t cpu_down_rate;
	atomic_t maxcoreslimit;
#ifndef CONFIG_CPU_EXYNOS4210
	atomic_t maxcoreslimitsleep;
#endif
} hotplug_tuners_ins = {
	.hotplug_sampling_rate = ATOMIC_INIT(60000),
	.hotplug_enable = ATOMIC_INIT(0),
	.cpu_up_rate = ATOMIC_INIT(5),
	.cpu_down_rate = ATOMIC_INIT(10),
	.maxcoreslimit = ATOMIC_INIT(NR_CPUS),
#ifndef CONFIG_CPU_EXYNOS4210
	.maxcoreslimitsleep = ATOMIC_INIT(1),
#endif
};

#define MAX_HOTPLUG_RATE		(40)
#define DOWN_INDEX		(0)
#define UP_INDEX		(1)
/*#define DEF_SAMPLING_MS			(200)*/

static atomic_t hotplugging_rate = ATOMIC_INIT(0);

#ifdef CONFIG_CPU_EXYNOS4210
static atomic_t hotplug_freq[2][2] = {
	{ATOMIC_INIT(0), ATOMIC_INIT(800000)},
	{ATOMIC_INIT(500000), ATOMIC_INIT(0)}
};
static atomic_t hotplug_load[2][2] = {
	{ATOMIC_INIT(0), ATOMIC_INIT(65)},
	{ATOMIC_INIT(30), ATOMIC_INIT(0)}
};
#else
static atomic_t hotplug_freq[4][2] = {
	{ATOMIC_INIT(0), ATOMIC_INIT(702000)},
	{ATOMIC_INIT(486000), ATOMIC_INIT(702000)},
	{ATOMIC_INIT(486000), ATOMIC_INIT(702000)},
	{ATOMIC_INIT(486000), ATOMIC_INIT(0)}
};
static atomic_t hotplug_load[4][2] = {
	{ATOMIC_INIT(0), ATOMIC_INIT(65)},
	{ATOMIC_INIT(30), ATOMIC_INIT(65)},
	{ATOMIC_INIT(30), ATOMIC_INIT(65)},
	{ATOMIC_INIT(30), ATOMIC_INIT(0)}
};
#endif

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%d\n", atomic_read(&hotplug_tuners_ins.object));		\
}
show_one(hotplug_sampling_rate, hotplug_sampling_rate);
show_one(hotplug_enable, hotplug_enable);
show_one(cpu_up_rate, cpu_up_rate);
show_one(cpu_down_rate, cpu_down_rate);
show_one(maxcoreslimit, maxcoreslimit);
#ifndef CONFIG_CPU_EXYNOS4210
show_one(maxcoreslimitsleep, maxcoreslimitsleep);
#endif

#define show_hotplug_param(file_name, num_core, up_down)		\
static ssize_t show_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%d\n", atomic_read(&file_name[num_core - 1][up_down]));	\
}

#define store_hotplug_param(file_name, num_core, up_down)		\
static ssize_t store_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr,				\
	const char *buf, size_t count)					\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%d", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	if (input == atomic_read(&file_name[num_core - 1][up_down])) {		\
		return count;	\
	}	\
	atomic_set(&file_name[num_core - 1][up_down], input);			\
	return count;							\
}

/* hotplug freq */
show_hotplug_param(hotplug_freq, 1, 1);
show_hotplug_param(hotplug_freq, 2, 0);
#if NR_CPUS >= 4
show_hotplug_param(hotplug_freq, 2, 1);
show_hotplug_param(hotplug_freq, 3, 0);
show_hotplug_param(hotplug_freq, 3, 1);
show_hotplug_param(hotplug_freq, 4, 0);
#endif
/* hotplug load */
show_hotplug_param(hotplug_load, 1, 1);
show_hotplug_param(hotplug_load, 2, 0);
#if NR_CPUS >= 4
show_hotplug_param(hotplug_load, 2, 1);
show_hotplug_param(hotplug_load, 3, 0);
show_hotplug_param(hotplug_load, 3, 1);
show_hotplug_param(hotplug_load, 4, 0);
#endif

/* hotplug freq */
store_hotplug_param(hotplug_freq, 1, 1);
store_hotplug_param(hotplug_freq, 2, 0);
#if NR_CPUS >= 4
store_hotplug_param(hotplug_freq, 2, 1);
store_hotplug_param(hotplug_freq, 3, 0);
store_hotplug_param(hotplug_freq, 3, 1);
store_hotplug_param(hotplug_freq, 4, 0);
#endif
/* hotplug load */
store_hotplug_param(hotplug_load, 1, 1);
store_hotplug_param(hotplug_load, 2, 0);
#if NR_CPUS >= 4
store_hotplug_param(hotplug_load, 2, 1);
store_hotplug_param(hotplug_load, 3, 0);
store_hotplug_param(hotplug_load, 3, 1);
store_hotplug_param(hotplug_load, 4, 0);
#endif
define_one_global_rw(hotplug_freq_1_1);
define_one_global_rw(hotplug_freq_2_0);
#if NR_CPUS >= 4
define_one_global_rw(hotplug_freq_2_1);
define_one_global_rw(hotplug_freq_3_0);
define_one_global_rw(hotplug_freq_3_1);
define_one_global_rw(hotplug_freq_4_0);
#endif
define_one_global_rw(hotplug_load_1_1);
define_one_global_rw(hotplug_load_2_0);
#if NR_CPUS >= 4
define_one_global_rw(hotplug_load_2_1);
define_one_global_rw(hotplug_load_3_0);
define_one_global_rw(hotplug_load_3_1);
define_one_global_rw(hotplug_load_4_0);
#endif

#ifndef CONFIG_CPU_EXYNOS4210
extern bool apget_if_suspended(void);
#endif

/* hotplug_sampling_rate */
static ssize_t store_hotplug_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input,10000);
	
	if (input == atomic_read(&hotplug_tuners_ins.hotplug_sampling_rate))
		return count;

	atomic_set(&hotplug_tuners_ins.hotplug_sampling_rate,input);

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

	if (atomic_read(&hotplug_tuners_ins.hotplug_enable) == input)
		return count;

	atomic_set(&hotplug_tuners_ins.hotplug_enable, input);

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

	if (input == atomic_read(&hotplug_tuners_ins.cpu_up_rate))
		return count;

	atomic_set(&hotplug_tuners_ins.cpu_up_rate,input);

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

	if (input == atomic_read(&hotplug_tuners_ins.cpu_down_rate))
		return count;

	atomic_set(&hotplug_tuners_ins.cpu_down_rate,input);
	return count;
}

/* maxcoreslimit */
static ssize_t store_maxcoreslimit(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input > NR_CPUS ? NR_CPUS : input, 1); 

	if (atomic_read(&hotplug_tuners_ins.maxcoreslimit) == input)
		return count;

	atomic_set(&hotplug_tuners_ins.maxcoreslimit, input);

	return count;
}
#ifndef CONFIG_CPU_EXYNOS4210
/* maxcoreslimitsleep */
static ssize_t store_maxcoreslimitsleep(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input > NR_CPUS ? NR_CPUS : input, 1); 

	if (atomic_read(&hotplug_tuners_ins.maxcoreslimitsleep) == input)
		return count;

	atomic_set(&hotplug_tuners_ins.maxcoreslimitsleep, input);

	return count;
}
#endif
define_one_global_rw(hotplug_sampling_rate);
define_one_global_rw(hotplug_enable);
define_one_global_rw(cpu_up_rate);
define_one_global_rw(cpu_down_rate);
define_one_global_rw(maxcoreslimit);
#ifndef CONFIG_CPU_EXYNOS4210
define_one_global_rw(maxcoreslimitsleep);
#endif

static struct attribute *hotplug_attributes[] = {
	&hotplug_sampling_rate.attr,
	&hotplug_enable.attr,
	&hotplug_freq_1_1.attr,
	&hotplug_freq_2_0.attr,
#if NR_CPUS >= 4
	&hotplug_freq_2_1.attr,
	&hotplug_freq_3_0.attr,
	&hotplug_freq_3_1.attr,
	&hotplug_freq_4_0.attr,
#endif
	&hotplug_load_1_1.attr,
	&hotplug_load_2_0.attr,
#if NR_CPUS >= 4
	&hotplug_load_2_1.attr,
	&hotplug_load_3_0.attr,
	&hotplug_load_3_1.attr,
	&hotplug_load_4_0.attr,
#endif
	&cpu_up_rate.attr,
	&cpu_down_rate.attr,
	&maxcoreslimit.attr,
#ifndef CONFIG_CPU_EXYNOS4210
	&maxcoreslimitsleep.attr,
#endif
	NULL
};

static struct attribute_group hotplug_attr_group = {
	.attrs = hotplug_attributes,
	.name = "alucard_hotplug",
};

static void __cpuinit hotplug_work_fn(struct work_struct *work)
{
	int delay;
#ifndef CONFIG_CPU_EXYNOS4210
	bool earlysuspend = apget_if_suspended();
#endif
	int up_rate = atomic_read(&hotplug_tuners_ins.cpu_up_rate);
	int down_rate = atomic_read(&hotplug_tuners_ins.cpu_down_rate);
#ifdef CONFIG_CPU_EXYNOS4210
	int maxcoreslimit = atomic_read(&hotplug_tuners_ins.maxcoreslimit);
#else
	int maxcoreslimit = !earlysuspend ? atomic_read(&hotplug_tuners_ins.maxcoreslimit) : atomic_read(&hotplug_tuners_ins.maxcoreslimitsleep);
#endif
	bool hotplug_enable = atomic_read(&hotplug_tuners_ins.hotplug_enable) > 0;
#ifdef CONFIG_CPU_EXYNOS4210
	unsigned int cur_freq[NR_CPUS] = {0, 0};
	int cur_load[NR_CPUS] = {-1, -1};
#else
	unsigned int cur_freq[NR_CPUS] = {0, 0, 0, 0};
	int cur_load[NR_CPUS] = {-1, -1, -1, -1};
#endif
	unsigned int j;
	unsigned int cpu;
	unsigned long flags;

	for_each_online_cpu(j) {
		struct hotplug_cpuinfo *j_hotplug_cpuinfo = &per_cpu(od_hotplug_cpuinfo, j);
		struct cpufreq_policy cpu_policy;
		unsigned long cur_user_time, cur_system_time, cur_others_time, cur_idle_time, cur_iowait_time;
		unsigned int busy_time, idle_time;

		spin_lock_irqsave(&j_hotplug_cpuinfo->lock, flags);
		cur_user_time = (__force unsigned long)(kcpustat_cpu(j).cpustat[CPUTIME_USER]);
		cur_system_time = (__force unsigned long)(kcpustat_cpu(j).cpustat[CPUTIME_SYSTEM]);
		cur_others_time = (__force unsigned long)(kcpustat_cpu(j).cpustat[CPUTIME_IRQ] + kcpustat_cpu(j).cpustat[CPUTIME_SOFTIRQ]
																		+ kcpustat_cpu(j).cpustat[CPUTIME_STEAL] + kcpustat_cpu(j).cpustat[CPUTIME_NICE]);

		cur_idle_time = (__force unsigned long)(kcpustat_cpu(j).cpustat[CPUTIME_IDLE]);
		cur_iowait_time = (__force unsigned long)(kcpustat_cpu(j).cpustat[CPUTIME_IOWAIT]);
		spin_unlock_irqrestore(&j_hotplug_cpuinfo->lock, flags);

		busy_time = (unsigned int)
				((cur_user_time - j_hotplug_cpuinfo->prev_cpu_user) +
				 (cur_system_time - j_hotplug_cpuinfo->prev_cpu_system) +
				 (cur_others_time - j_hotplug_cpuinfo->prev_cpu_others));
		j_hotplug_cpuinfo->prev_cpu_user = cur_user_time;
		j_hotplug_cpuinfo->prev_cpu_system = cur_system_time;
		j_hotplug_cpuinfo->prev_cpu_others = cur_others_time;

		idle_time = (unsigned int)
				((cur_idle_time - j_hotplug_cpuinfo->prev_cpu_idle) + 
				 (cur_iowait_time - j_hotplug_cpuinfo->prev_cpu_iowait));
		j_hotplug_cpuinfo->prev_cpu_idle = cur_idle_time;
		j_hotplug_cpuinfo->prev_cpu_iowait = cur_iowait_time;

		cpufreq_get_policy(&cpu_policy, j);
		/*printk(KERN_ERR "TIMER CPU[%u], wall[%u], idle[%u]\n",j, busy_time + idle_time, idle_time);*/
		if (!cpu_policy.cur || busy_time + idle_time == 0) { /*if busy_time and idle_time are 0, evaluate cpu load next time*/
			hotplug_enable = false;
			continue;
		}

		cur_load[j] = busy_time ? (100 * busy_time) / (busy_time + idle_time) : 1;/*if busy_time is 0 cpu_load is equal to 1*/
		cur_freq[j] = cpu_policy.cur;
	}

	/* set hotplugging_rate used */
	atomic_inc(&hotplugging_rate);

	if (hotplug_enable) {
		/*Check for CPU hotplug*/
		if (atomic_read(&hotplugging_rate) % up_rate == 0) {
			for_each_cpu_not(cpu, cpu_online_mask) {
				if (cpu > 0 && cpu <= maxcoreslimit - 1) {
					if (cur_load[cpu - 1] >= atomic_read(&hotplug_load[cpu - 1][UP_INDEX])
						&& cur_freq[cpu - 1] >= atomic_read(&hotplug_freq[cpu - 1][UP_INDEX])) {
						cpu_up(cpu);
						break;
					}
				}
			}
		}
		if (atomic_read(&hotplugging_rate) % down_rate == 0) {
			maxcoreslimit = (maxcoreslimit == NR_CPUS ? 0 : maxcoreslimit - 1);
			for_each_online_cpu(cpu) {
				if (cpu > maxcoreslimit) {
					if (cur_load[cpu] > -1
						&& (cur_load[cpu] < atomic_read(&hotplug_load[cpu][DOWN_INDEX])
							|| cur_freq[cpu] <= atomic_read(&hotplug_freq[cpu][DOWN_INDEX]))) {
								cpu_down(cpu);
								break;
					}
				}
			}
		}
	}

	if (atomic_read(&hotplugging_rate) == max(up_rate, down_rate)) {
		atomic_set(&hotplugging_rate, 0);
	}

	delay = usecs_to_jiffies(atomic_read(&hotplug_tuners_ins.hotplug_sampling_rate));
	if (num_online_cpus() > 1)
		delay -= jiffies % delay;

	schedule_delayed_work_on(0, &alucard_hotplug_work, delay);
	/*schedule_delayed_work_on(0, &alucard_hotplug_work,
	msecs_to_jiffies(DEF_SAMPLING_MS));*/
}

int __init alucard_hotplug_init(void)
{
	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay;
	unsigned int j;
	int rc;

	alucard_hotplug_enable++;

	for_each_possible_cpu(j) {
		struct hotplug_cpuinfo *j_hotplug_cpuinfo = &per_cpu(od_hotplug_cpuinfo, j);
		unsigned long flags;
		spin_lock_init(&j_hotplug_cpuinfo->lock);
		spin_lock_irqsave(&j_hotplug_cpuinfo->lock, flags);
		j_hotplug_cpuinfo->prev_cpu_user = (__force unsigned long)(kcpustat_cpu(j).cpustat[CPUTIME_USER]);
		j_hotplug_cpuinfo->prev_cpu_system = (__force unsigned long)(kcpustat_cpu(j).cpustat[CPUTIME_SYSTEM]);
		j_hotplug_cpuinfo->prev_cpu_others = (__force unsigned long)(kcpustat_cpu(j).cpustat[CPUTIME_IRQ] + kcpustat_cpu(j).cpustat[CPUTIME_SOFTIRQ]
																		+ kcpustat_cpu(j).cpustat[CPUTIME_STEAL] + kcpustat_cpu(j).cpustat[CPUTIME_NICE]);

		j_hotplug_cpuinfo->prev_cpu_idle = (__force unsigned long)(kcpustat_cpu(j).cpustat[CPUTIME_IDLE]);
		j_hotplug_cpuinfo->prev_cpu_iowait = (__force unsigned long)(kcpustat_cpu(j).cpustat[CPUTIME_IOWAIT]);
		spin_unlock_irqrestore(&j_hotplug_cpuinfo->lock, flags);
	}

	if (alucard_hotplug_enable == 1) {
		rc = sysfs_create_group(cpufreq_global_kobject,
							&hotplug_attr_group);
		if (rc)
			return rc;
	}

	delay = usecs_to_jiffies(atomic_read(&hotplug_tuners_ins.hotplug_sampling_rate));
	if (num_online_cpus() > 1)
		delay -= jiffies % delay;

	INIT_DELAYED_WORK(&alucard_hotplug_work, hotplug_work_fn);
	schedule_delayed_work_on(0, &alucard_hotplug_work, delay);

	return 0;
}

MODULE_AUTHOR("Alucard_24@XDA");
MODULE_DESCRIPTION("'alucard_hotplug' - A cpu hotplug driver for "
	"capable processors");
MODULE_LICENSE("GPL");

late_initcall(alucard_hotplug_init);
