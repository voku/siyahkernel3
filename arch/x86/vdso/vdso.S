#include <linux/init.h>

__INITDATA

	.globl vdso_start, vdso_end
vdso_start:
	.incbin "arch/x86/vdso/vdso.so"
vdso_end:
	.align PAGE_SIZE /* extra data here leaks to userspace. */

__FINIT
