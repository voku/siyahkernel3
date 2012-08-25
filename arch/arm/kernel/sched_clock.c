/*
 * sched_clock.c: support for extending counters to full 64-bit ns counter
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>

#include <asm/sched_clock.h>

<<<<<<< HEAD
static void sched_clock_poll(unsigned long wrap_ticks);
static DEFINE_TIMER(sched_clock_timer, sched_clock_poll, 0, 0);
static void (*sched_clock_update_fn)(void);
=======
struct clock_data {
	u64 epoch_ns;
	u32 epoch_cyc;
	u32 epoch_cyc_copy;
	u32 mult;
	u32 shift;
	bool suspended;
	bool needs_suspend;
};

static void sched_clock_poll(unsigned long wrap_ticks);
static DEFINE_TIMER(sched_clock_timer, sched_clock_poll, 0, 0);

static struct clock_data cd = {
	.mult	= NSEC_PER_SEC / HZ,
};

static u32 __read_mostly sched_clock_mask = 0xffffffff;

static u32 notrace jiffy_sched_clock_read(void)
{
	return (u32)(jiffies - INITIAL_JIFFIES);
}

static u32 __read_mostly (*read_sched_clock)(void) = jiffy_sched_clock_read;

static inline u64 cyc_to_ns(u64 cyc, u32 mult, u32 shift)
{
	return (cyc * mult) >> shift;
}

static unsigned long long cyc_to_sched_clock(u32 cyc, u32 mask)
{
	u64 epoch_ns;
	u32 epoch_cyc;

	if (cd.suspended)
		return cd.epoch_ns;

	/*
	 * Load the epoch_cyc and epoch_ns atomically.  We do this by
	 * ensuring that we always write epoch_cyc, epoch_ns and
	 * epoch_cyc_copy in strict order, and read them in strict order.
	 * If epoch_cyc and epoch_cyc_copy are not equal, then we're in
	 * the middle of an update, and we should repeat the load.
	 */
	do {
		epoch_cyc = cd.epoch_cyc;
		smp_rmb();
		epoch_ns = cd.epoch_ns;
		smp_rmb();
	} while (epoch_cyc != cd.epoch_cyc_copy);

	return epoch_ns + cyc_to_ns((cyc - epoch_cyc) & mask, cd.mult, cd.shift);
}

/*
 * Atomically update the sched_clock epoch.
 */
static void notrace update_sched_clock(void)
{
	unsigned long flags;
	u32 cyc;
	u64 ns;

	cyc = read_sched_clock();
	ns = cd.epoch_ns +
		cyc_to_ns((cyc - cd.epoch_cyc) & sched_clock_mask,
			  cd.mult, cd.shift);
	/*
	 * Write epoch_cyc and epoch_ns in a way that the update is
	 * detectable in cyc_to_fixed_sched_clock().
	 */
	raw_local_irq_save(flags);
	cd.epoch_cyc = cyc;
	smp_wmb();
	cd.epoch_ns = ns;
	smp_wmb();
	cd.epoch_cyc_copy = cyc;
	raw_local_irq_restore(flags);
}
>>>>>>> 6dab7ed... Merge branch 'fixes' of git://git.linaro.org/people/rmk/linux-arm

static void sched_clock_poll(unsigned long wrap_ticks)
{
	mod_timer(&sched_clock_timer, round_jiffies(jiffies + wrap_ticks));
	sched_clock_update_fn();
}

<<<<<<< HEAD
void __init init_sched_clock(struct clock_data *cd, void (*update)(void),
	unsigned int clock_bits, unsigned long rate)
=======
void __init setup_sched_clock_needs_suspend(u32 (*read)(void), int bits,
		unsigned long rate)
{
	setup_sched_clock(read, bits, rate);
	cd.needs_suspend = true;
}

void __init setup_sched_clock(u32 (*read)(void), int bits, unsigned long rate)
>>>>>>> 6dab7ed... Merge branch 'fixes' of git://git.linaro.org/people/rmk/linux-arm
{
	unsigned long r, w;
	u64 res, wrap;
	char r_unit;

	sched_clock_update_fn = update;

	/* calculate the mult/shift to convert counter ticks to ns. */
	clocks_calc_mult_shift(&cd->mult, &cd->shift, rate, NSEC_PER_SEC, 0);

	r = rate;
	if (r >= 4000000) {
		r /= 1000000;
		r_unit = 'M';
	} else {
		r /= 1000;
		r_unit = 'k';
	}

	/* calculate how many ns until we wrap */
	wrap = cyc_to_ns((1ULL << clock_bits) - 1, cd->mult, cd->shift);
	do_div(wrap, NSEC_PER_MSEC);
	w = wrap;

	/* calculate the ns resolution of this counter */
	res = cyc_to_ns(1ULL, cd->mult, cd->shift);
	pr_info("sched_clock: %u bits at %lu%cHz, resolution %lluns, wraps every %lums\n",
		clock_bits, r, r_unit, res, w);

	/*
	 * Start the timer to keep sched_clock() properly updated and
	 * sets the initial epoch.
	 */
	sched_clock_timer.data = msecs_to_jiffies(w - (w / 10));
	update();

	/*
	 * Ensure that sched_clock() starts off at 0ns
	 */
	cd->epoch_ns = 0;
}

void __init sched_clock_postinit(void)
{
	sched_clock_poll(sched_clock_timer.data);
<<<<<<< HEAD
=======
	if (cd.needs_suspend)
		cd.suspended = true;
	return 0;
}

static void sched_clock_resume(void)
{
	if (cd.needs_suspend) {
		cd.epoch_cyc = read_sched_clock();
		cd.epoch_cyc_copy = cd.epoch_cyc;
		cd.suspended = false;
	}
}

static struct syscore_ops sched_clock_ops = {
	.suspend = sched_clock_suspend,
	.resume = sched_clock_resume,
};

static int __init sched_clock_syscore_init(void)
{
	register_syscore_ops(&sched_clock_ops);
	return 0;
>>>>>>> 6dab7ed... Merge branch 'fixes' of git://git.linaro.org/people/rmk/linux-arm
}
