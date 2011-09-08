/*
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Common Header for EXYNOS machines
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __ARCH_ARM_MACH_EXYNOS_COMMON_H
#define __ARCH_ARM_MACH_EXYNOS_COMMON_H

struct map_desc;

extern struct smp_operations exynos_smp_ops;

extern void exynos_cpu_die(unsigned int cpu);

#endif /* __ARCH_ARM_MACH_EXYNOS_COMMON_H */
