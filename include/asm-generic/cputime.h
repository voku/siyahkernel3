#ifndef _ASM_GENERIC_CPUTIME_H
#define _ASM_GENERIC_CPUTIME_H

#include <linux/time.h>
#include <linux/jiffies.h>

typedef u64 cputime64_t;

#define cputime64_zero (0ULL)
#define cputime64_add(__a, __b)     ((__a) + (__b))
#define cputime64_sub(__a, __b)     ((__a) - (__b))

#include <asm-generic/cputime_jiffies.h>

#endif
