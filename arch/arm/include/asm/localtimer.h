/*
 *  arch/arm/include/asm/localtimer.h
 *
 *  Copyright (C) 2004-2005 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __ASM_ARM_LOCALTIMER_H
#define __ASM_ARM_LOCALTIMER_H

#include <linux/interrupt.h>

struct clock_event_device;

#ifdef CONFIG_LOCAL_TIMERS

#ifdef CONFIG_HAVE_ARM_TWD

#include "smp_twd.h"

#define local_timer_stop(c)	twd_timer_stop((c))

#else

/*
 * Stop the local timer
 */
void local_timer_stop(struct clock_event_device *);

#endif

/*
 * Setup a local timer interrupt for a CPU.
 */
int local_timer_setup(struct clock_event_device *);

#else

static inline int local_timer_setup(struct clock_event_device *evt)
{
	return -ENXIO;
}

static inline void local_timer_stop(struct clock_event_device *evt)
{
}
#endif

#endif
