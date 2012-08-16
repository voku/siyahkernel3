#ifdef __uClinux__
#include "system_no.h"
#include <linux/bug.h>
#else
#include "system_mm.h"
#endif
