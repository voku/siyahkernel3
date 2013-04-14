/*
 * Author: Paul Reioux aka Faux123 <reioux@gmail.com>
 *
 * Copyright 2013 Paul Reioux
 * Copyright 2012 Paul Reioux
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
#include <linux/earlysuspend.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>

#define INTELLI_PLUG_MAJOR_VERSION	1
#define INTELLI_PLUG_MINOR_VERSION	5

#define DEF_SAMPLING_RATE		(50000)
#define DEF_SAMPLING_MS			(200)

#define DUAL_CORE_PERSISTENCE		15

#define RUN_QUEUE_THRESHOLD		38

#define CPU_DOWN_FACTOR			3

static DEFINE_MUTEX(intelli_plug_mutex);

struct delayed_work intelli_plug_work;

static unsigned int intelli_plug_active = 1;
module_param(intelli_plug_active, uint, 0644);

static unsigned int eco_mode_active = 0;
module_param(eco_mode_active, uint, 0644);

static unsigned int persist_count = 0;
static bool suspended = false;

#define NR_FSHIFT	3
static unsigned int nr_fshift = NR_FSHIFT;
module_param(nr_fshift, uint, 0644);

static unsigned int nr_run_thresholds_full[] = {
/* 	1,  2 - on-line cpus target */
	5,  UINT_MAX /* avg run threads * 2 (e.g., 9 = 2.25 threads) */
};

static unsigned int nr_run_thresholds_eco[] = {
/*  1,  2 - on-line cpus target */
    3,  UINT_MAX /* avg run threads * 2 (e.g., 9 = 2.25 threads) */
};

static unsigned int nr_run_hysteresis = 4;  /* 0.5 thread */
module_param(nr_run_hysteresis, uint, 0644);

#ifndef CONFIG_CPU_EXYNOS4210
#define RQ_AVG_TIMER_RATE	10
#else
#define RQ_AVG_TIMER_RATE	20
#endif

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

static unsigned int nr_run_last;

static unsigned int calculate_thread_stats(void)
{
	unsigned int avg_nr_run = get_nr_run_avg();
	unsigned int nr_run;
	unsigned int threshold_size;

	if (!eco_mode_active) {
		threshold_size =  ARRAY_SIZE(nr_run_thresholds_full);
		nr_run_hysteresis = 8;
	}
	else {
		threshold_size =  ARRAY_SIZE(nr_run_thresholds_eco);
		nr_run_hysteresis = 4;
	}

	for (nr_run = 1; nr_run < threshold_size; nr_run++) {
		unsigned int nr_threshold;
		if (!eco_mode_active)
			nr_threshold = nr_run_thresholds_full[nr_run - 1];
		else
			nr_threshold = nr_run_thresholds_eco[nr_run - 1];

		if (nr_run_last <= nr_run)
			nr_threshold += nr_run_hysteresis;

		if (avg_nr_run <= (nr_threshold << (FSHIFT - nr_fshift)))
			break;
	}
	nr_run_last = nr_run;

	return nr_run;
}

static void __cpuinit intelli_plug_work_fn(struct work_struct *work)
{
	unsigned int nr_run_stat;

	if (intelli_plug_active == 1) {
		nr_run_stat = calculate_thread_stats();

		if (!suspended) {
			switch (nr_run_stat) {
				case 1:
					if (persist_count > 0) {
						persist_count--;
					}
					else if (num_online_cpus() == 2) {
						cpu_down(1);
					}
					break;
				case 2:
					persist_count = DUAL_CORE_PERSISTENCE / CPU_DOWN_FACTOR;
					if (num_online_cpus() == 1)
						cpu_up(1);
					break;
				default:
					pr_err("Run Stat Error: Bad value %u\n", nr_run_stat);
					break;
			}
		}
	}
	schedule_delayed_work_on(0, &intelli_plug_work,
		msecs_to_jiffies(DEF_SAMPLING_MS));
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void intelli_plug_early_suspend(struct early_suspend *handler)
{
	int i;
	int num_of_active_cores = 2;
	
	cancel_delayed_work_sync(&intelli_plug_work);

	mutex_lock(&intelli_plug_mutex);
	suspended = true;
	mutex_unlock(&intelli_plug_mutex);

	// put rest of the cores to sleep!
	for (i=1; i<num_of_active_cores; i++) {
		if (cpu_online(i))
			cpu_down(i);
	}
}

static void __cpuinit intelli_plug_late_resume(struct early_suspend *handler)
{
	int num_of_active_cores;
	int i;

	mutex_lock(&intelli_plug_mutex);
	/* keep cores awake long enough for faster wake up */
	persist_count = DUAL_CORE_PERSISTENCE;
	suspended = false;
	mutex_unlock(&intelli_plug_mutex);

	/* wake up everyone */
	num_of_active_cores = 2;

	for (i=1; i<num_of_active_cores; i++) {
		if (!cpu_online(i))
			cpu_up(i);
	}

	schedule_delayed_work_on(0, &intelli_plug_work,
		msecs_to_jiffies(10));
}

static struct early_suspend intelli_plug_early_suspend_struct_driver = {
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 10,
	.suspend = intelli_plug_early_suspend,
	.resume = intelli_plug_late_resume,
};
#endif	/* CONFIG_HAS_EARLYSUSPEND */

int __init intelli_plug_init(void)
{
	int ret, delay;

	ret = init_rq_avg();
	if (ret) return ret;

	/* we want all CPUs to do sampling nearly on same jiffy */
	delay = usecs_to_jiffies(DEF_SAMPLING_RATE);

	if (num_online_cpus() > 1)
		delay -= jiffies % delay;

	pr_info("intelli_plug: version %d.%d by faux123\n",
		 INTELLI_PLUG_MAJOR_VERSION,
		 INTELLI_PLUG_MINOR_VERSION);

	INIT_DELAYED_WORK(&intelli_plug_work, intelli_plug_work_fn);
	schedule_delayed_work_on(0, &intelli_plug_work, delay);

#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&intelli_plug_early_suspend_struct_driver);
#endif
	return 0;
}

MODULE_AUTHOR("Paul Reioux <reioux@gmail.com>");
MODULE_DESCRIPTION("'intell_plug' - An intelligent cpu hotplug driver for "
	"Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPL");

late_initcall(intelli_plug_init);
