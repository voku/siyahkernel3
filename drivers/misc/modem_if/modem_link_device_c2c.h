/*
 * Copyright (C) 2010 Samsung Electronics.
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

#ifndef __MODEM_LINK_DEVICE_C2C_H__
#define __MODEM_LINK_DEVICE_C2C_H__

#include "modem_link_device_memory.h"
#include <mach/c2c.h>

/*
	magic_code +
	access_enable +
	fmt_tx_head + fmt_tx_tail + fmt_tx_buff +
	raw_tx_head + raw_tx_tail + raw_tx_buff +
	fmt_rx_head + fmt_rx_tail + fmt_rx_buff +
	raw_rx_head + raw_rx_tail + raw_rx_buff +
	mbx_cp2ap +
	mbx_ap2cp
 =	2 +
	2 +
	4 + 4 + 1332 +
	4 + 4 + (4564+2088956) +
	4 + 4 + 1332 +
	4 + 4 + (9124+2088956) +
	2 +
	2
 =	4 MB
*/
#define C2C_4M_FMT_TX_BUFF_SZ	1332
#define C2C_4M_RAW_TX_BUFF_SZ	(4564+2088956)
#define C2C_4M_FMT_RX_BUFF_SZ	1332
#define C2C_4M_RAW_RX_BUFF_SZ	(9124+2088956)

struct c2c_ipc_4m_map {
	u16 magic;
	u16 access;

	u32 fmt_tx_head;
	u32 fmt_tx_tail;
	u8  fmt_tx_buff[C2C_4M_FMT_TX_BUFF_SZ];

	u32 raw_tx_head;
	u32 raw_tx_tail;
	u8  raw_tx_buff[C2C_4M_RAW_TX_BUFF_SZ];

	u32 fmt_rx_head;
	u32 fmt_rx_tail;
	u8  fmt_rx_buff[C2C_4M_FMT_RX_BUFF_SZ];

	u32 raw_rx_head;
	u32 raw_rx_tail;
	u8  raw_rx_buff[C2C_4M_RAW_RX_BUFF_SZ];
} __packed;

struct shmem_link_device;

struct shmem_link_device {
	struct link_device ld;

	enum shmem_type type;

	/* SHM (SHARED MEMORY) address and size */
	unsigned long start;	/* physical "start" address of SHM */
	u32 size;
	u8 __iomem *base;	/* virtual address of the "start" */

	/* SHM IRQ GPIO# */
	unsigned gpio_ap_wakeup;
	int irq_ap_wakeup;
	unsigned gpio_ap_status;

	unsigned gpio_cp_wakeup;
	unsigned gpio_cp_status;

	/* Physical configuration -> logical configuration */
	struct memif_boot_map bt_map;
	struct memif_dload_map dl_map;
	struct memif_uload_map ul_map;

	/* IPC device map */
	struct shmem_ipc_map ipc_map;

	/* Pointers (aliases) to IPC device map */
	u16 __iomem *magic;
	u16 __iomem *access;
	struct shmem_ipc_device *dev[MAX_IPC_DEV];
	u16 __iomem *mbx2ap;
	u16 __iomem *mbx2cp;

	/* Wakelock for SHM device */
	struct wake_lock wlock;
	char wlock_name[MIF_MAX_NAME_LEN];

	/* This must be set just one time at 1st CP boot succeess. */
	bool setup_done;

	/* for UDL */
	struct tasklet_struct ul_tsk;
	struct tasklet_struct dl_tsk;
	struct completion udl_cmpl;

	/*
	** for CP crash dump
	*/
	bool forced_cp_crash;
	struct timer_list crash_ack_timer;
	struct timer_list crash_timer;
	struct completion crash_cmpl;
	/* If this field is wanted to be used, it must be initialized only in
	 * the "ld->dump_start" method.
	 */
	struct delayed_work crash_dwork;
	/* Count of CP crash dump packets received */
	int crash_rcvd;

	/* for locking TX process */
	spinlock_t tx_lock[MAX_IPC_DEV];

	/* for retransmission under SHM flow control after TXQ full state */
	atomic_t res_required[MAX_IPC_DEV];
	struct completion req_ack_cmpl[MAX_IPC_DEV];

	/* for efficient RX process */
	struct tasklet_struct rx_tsk;
	struct io_device *iod[MAX_IPC_DEV];

	/* for wake-up/sleep control */
	atomic_t accessing;

	/* Multi-purpose miscellaneous buffer */
	u8 *buff;

	/* Alias to SHM IRQ handler */
	irqreturn_t (*irq_handler)(int irq, void *data);

	/* for logging SHM status */
	struct mem_stat_queue stat_list;

	/* for SHM dump */
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	char dump_path[MIF_MAX_PATH_LEN];
	char trace_path[MIF_MAX_PATH_LEN];
	struct trace_queue dump_list;
	struct trace_queue trace_list;
	struct delayed_work dump_dwork;
	struct delayed_work trace_dwork;
#endif
};

/* converts from struct link_device* to struct xxx_link_device* */
#define to_shmem_link_device(linkdev) \
		container_of(linkdev, struct shmem_link_device, ld)
#endif

