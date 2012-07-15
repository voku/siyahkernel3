/* include/linux/wakelock-dev.h
 *
 * Copyright (C) 2009 Google, Inc.
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

#ifndef _LINUX_WAKELOCK_DEV_H
#define _LINUX_WAKELOCK_DEV_H

#include <linux/ioctl.h>

#define WAKELOCK_IOCTL_INIT(len)	_IOC(_IOC_WRITE,'w',0,len)
#define WAKELOCK_IOCTL_LOCK		_IO('w', 1)
#define WAKELOCK_IOCTL_LOCK_TIMEOUT	_IOW('w', 2, struct timespec)
#define WAKELOCK_IOCTL_UNLOCK		_IO('w', 3)

#endif
