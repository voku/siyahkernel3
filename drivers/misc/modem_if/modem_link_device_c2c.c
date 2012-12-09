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

#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/time.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/wakelock.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/if_arp.h>
#include <linux/platform_device.h>
#include <linux/kallsyms.h>
#include <linux/suspend.h>
#include <linux/platform_data/modem.h>
#include <plat/gpio-cfg.h>
#include <mach/gpio.h>
#include <mach/c2c.h>

#include "modem_prj.h"
#include "modem_link_device_c2c.h"
#include "modem_utils.h"

static void handle_cp_crash(struct shmem_link_device *shmd);
static void trigger_force_cp_crash(struct shmem_link_device *shmd);

/**
 * set_circ_pointer
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 * @dir: direction of communication (TX or RX)
 * @ptr: type of the queue pointer (HEAD or TAIL)
 * @addr: address of the queue pointer
 * @val: value to be written to the queue pointer
 *
 * Writes a value to a pointer in a circular queue with verification.
 */
static inline void set_circ_pointer(struct shmem_link_device *shmd, int id,
				int dir, int ptr, void __iomem *addr, u32 val)
{
	struct link_device *ld = &shmd->ld;
	int cnt = 0;
	u32 saved = 0;

	iowrite32(val, addr);

	while (1) {
		/* Check the value written to the address */
		saved = ioread32(addr);
		if (likely(saved == val))
			break;

		cnt++;
		mif_err("%s: ERR! %s_%s.%s saved(%d) != val(%d), count %d\n",
			ld->name, get_dev_name(id), circ_dir(dir),
			circ_ptr(ptr), saved, val, cnt);
		if (cnt >= MAX_RETRY_CNT) {
			trigger_force_cp_crash(shmd);
			break;
		}

		udelay(100);

		/* Write the value again */
		iowrite32(val, addr);
	}
}

/**
 * register_isr
 * @irq: IRQ number for an interrupt
 * @isr: function pointer to an interrupt service routine
 * @flags: set of interrupt flags
 * @name: name of the interrupt
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Registers the ISR for the IRQ number.
 */
static int register_isr(unsigned int irq, irqreturn_t (*isr)(int, void*),
			unsigned long flags, const char *name,
			struct shmem_link_device *shmd)
{
	int ret;

	ret = request_irq(irq, isr, flags, name, shmd);
	if (ret) {
		mif_err("%s: ERR! request_irq fail (err %d)\n", name, ret);
		return ret;
	}

	ret = enable_irq_wake(irq);
	if (ret)
		mif_err("%s: ERR! enable_irq_wake fail (err %d)\n", name, ret);

	mif_err("%s (#%d) handler registered\n", name, irq);

	return 0;
}

/**
 * recv_int2ap
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Returns the value of the CP-to-AP interrupt register.
 */
static inline u16 recv_int2ap(struct shmem_link_device *shmd)
{
	if (shmd->mbx2ap)
		return *shmd->mbx2ap;
	return (u16)c2c_read_interrupt();
}

/**
 * clear_int2ap
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Clear the value of the CP-to-AP interrupt register.
 */
static inline void clear_int2ap(struct shmem_link_device *shmd)
{
	c2c_reset_interrupt();
}

/**
 * send_int2cp
 * @shmd: pointer to an instance of shmem_link_device structure
 * @mask: value to be written to the AP-to-CP interrupt register
 */
static inline void send_int2cp(struct shmem_link_device *shmd, u16 mask)
{
	if (shmd->mbx2cp)
		*shmd->mbx2cp = mask;
	c2c_send_interrupt(mask);
}

/**
 * read_int2cp
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Returns the value of the AP-to-CP interrupt register.
 */
static inline u16 read_int2cp(struct shmem_link_device *shmd)
{
	if (shmd->mbx2cp)
		return ioread16(shmd->mbx2cp);
	else
		return 0;
}

/**
 * get_magic
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Returns the value of the "magic code" field.
 */
static inline u16 get_magic(struct shmem_link_device *shmd)
{
	return ioread16(shmd->magic);
}

/**
 * set_magic
 * @shmd: pointer to an instance of shmem_link_device structure
 * @val: value to be written to the "magic code" field
 */
static inline void set_magic(struct shmem_link_device *shmd, u16 val)
{
	iowrite16(val, shmd->magic);
}

/**
 * get_access
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Returns the value of the "access enable" field.
 */
static inline u16 get_access(struct shmem_link_device *shmd)
{
	return ioread16(shmd->access);
}

/**
 * set_access
 * @shmd: pointer to an instance of shmem_link_device structure
 * @val: value to be written to the "access enable" field
 */
static inline void set_access(struct shmem_link_device *shmd, u16 val)
{
	iowrite16(val, shmd->access);
}

/**
 * get_txq_head
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Returns the value of a head (in) pointer in a TX queue.
 */
static inline u32 get_txq_head(struct shmem_link_device *shmd, int id)
{
	return ioread32(shmd->dev[id]->txq.head);
}

/**
 * get_txq_tail
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Returns the value of a tail (out) pointer in a TX queue.
 *
 * It is useless for an AP to read a tail pointer in a TX queue twice to verify
 * whether or not the value in the pointer is valid, because it can already have
 * been updated by a CP after the first access from the AP.
 */
static inline u32 get_txq_tail(struct shmem_link_device *shmd, int id)
{
	return ioread32(shmd->dev[id]->txq.tail);
}

/**
 * set_txq_head
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 * @in: value to be written to the head pointer in a TXQ
 */
static inline void set_txq_head(struct shmem_link_device *shmd, int id, u32 in)
{
	set_circ_pointer(shmd, id, TX, HEAD, shmd->dev[id]->txq.head, in);
}

/**
 * set_txq_tail
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 * @out: value to be written to the tail pointer in a TXQ
 */
static inline void set_txq_tail(struct shmem_link_device *shmd, int id, u32 out)
{
	set_circ_pointer(shmd, id, TX, TAIL, shmd->dev[id]->txq.tail, out);
}

/**
 * get_txq_buff
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Returns the start address of the buffer in a TXQ.
 */
static inline u8 *get_txq_buff(struct shmem_link_device *shmd, int id)
{
	return shmd->dev[id]->txq.buff;
}

/**
 * get_txq_buff_size
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Returns the size of the buffer in a TXQ.
 */
static inline u32 get_txq_buff_size(struct shmem_link_device *shmd, int id)
{
	return shmd->dev[id]->txq.size;
}

/**
 * get_rxq_head
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Returns the value of a head (in) pointer in an RX queue.
 *
 * It is useless for an AP to read a head pointer in an RX queue twice to verify
 * whether or not the value in the pointer is valid, because it can already have
 * been updated by a CP after the first access from the AP.
 */
static inline u32 get_rxq_head(struct shmem_link_device *shmd, int id)
{
	return ioread32(shmd->dev[id]->rxq.head);
}

/**
 * get_rxq_tail
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Returns the value of a tail (in) pointer in an RX queue.
 */
static inline u32 get_rxq_tail(struct shmem_link_device *shmd, int id)
{
	return ioread32(shmd->dev[id]->rxq.tail);
}

/**
 * set_rxq_head
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 * @in: value to be written to the head pointer in an RXQ
 */
static inline void set_rxq_head(struct shmem_link_device *shmd, int id, u32 in)
{
	set_circ_pointer(shmd, id, RX, HEAD, shmd->dev[id]->rxq.head, in);
}

/**
 * set_rxq_tail
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 * @out: value to be written to the tail pointer in an RXQ
 */
static inline void set_rxq_tail(struct shmem_link_device *shmd, int id, u32 out)
{
	set_circ_pointer(shmd, id, RX, TAIL, shmd->dev[id]->rxq.tail, out);
}

/**
 * get_rxq_buff
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Returns the start address of the buffer in an RXQ.
 */
static inline u8 *get_rxq_buff(struct shmem_link_device *shmd, int id)
{
	return shmd->dev[id]->rxq.buff;
}

/**
 * get_rxq_buff_size
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Returns the size of the buffer in an RXQ.
 */
static inline u32 get_rxq_buff_size(struct shmem_link_device *shmd, int id)
{
	return shmd->dev[id]->rxq.size;
}

/**
 * get_mask_req_ack
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Returns the REQ_ACK mask value for the IPC device.
 */
static inline u16 get_mask_req_ack(struct shmem_link_device *shmd, int id)
{
	return shmd->dev[id]->mask_req_ack;
}

/**
 * get_mask_res_ack
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Returns the RES_ACK mask value for the IPC device.
 */
static inline u16 get_mask_res_ack(struct shmem_link_device *shmd, int id)
{
	return shmd->dev[id]->mask_res_ack;
}

/**
 * get_mask_send
 * @shmd: pointer to an instance of shmem_link_device structure
 * @id: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Returns the SEND mask value for the IPC device.
 */
static inline u16 get_mask_send(struct shmem_link_device *shmd, int id)
{
	return shmd->dev[id]->mask_send;
}

/**
 * reset_txq_circ
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Empties a TXQ by resetting the head (in) pointer with the value in the tail
 * (out) pointer.
 */
static inline void reset_txq_circ(struct shmem_link_device *shmd, int dev)
{
	struct link_device *ld = &shmd->ld;
	u32 head = get_txq_head(shmd, dev);
	u32 tail = get_txq_tail(shmd, dev);

	mif_err("%s: %s_TXQ: HEAD[%u] <== TAIL[%u]\n",
		ld->name, get_dev_name(dev), head, tail);

	set_txq_head(shmd, dev, tail);
}

/**
 * reset_rxq_circ
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Empties an RXQ by resetting the tail (out) pointer with the value in the head
 * (in) pointer.
 */
static inline void reset_rxq_circ(struct shmem_link_device *shmd, int dev)
{
	struct link_device *ld = &shmd->ld;
	u32 head = get_rxq_head(shmd, dev);
	u32 tail = get_rxq_tail(shmd, dev);

	mif_err("%s: %s_RXQ: TAIL[%u] <== HEAD[%u]\n",
		ld->name, get_dev_name(dev), tail, head);

	set_rxq_tail(shmd, dev, head);
}

/**
 * get_shmem_status
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dir: direction of communication (TX or RX)
 * @stat: pointer to an instance of mem_status structure
 *
 * Takes a snapshot of the current status of a SHM.
 */
static void get_shmem_status(struct shmem_link_device *shmd,
			enum circ_dir_type dir, struct mem_status *stat)
{
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	getnstimeofday(&stat->ts);
#endif

	stat->dir = dir;
	stat->magic = get_magic(shmd);
	stat->access = get_access(shmd);
	stat->head[IPC_FMT][TX] = get_txq_head(shmd, IPC_FMT);
	stat->tail[IPC_FMT][TX] = get_txq_tail(shmd, IPC_FMT);
	stat->head[IPC_FMT][RX] = get_rxq_head(shmd, IPC_FMT);
	stat->tail[IPC_FMT][RX] = get_rxq_tail(shmd, IPC_FMT);
	stat->head[IPC_RAW][TX] = get_txq_head(shmd, IPC_RAW);
	stat->tail[IPC_RAW][TX] = get_txq_tail(shmd, IPC_RAW);
	stat->head[IPC_RAW][RX] = get_rxq_head(shmd, IPC_RAW);
	stat->tail[IPC_RAW][RX] = get_rxq_tail(shmd, IPC_RAW);
	stat->int2ap = recv_int2ap(shmd);
	stat->int2cp = read_int2cp(shmd);
}

#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
/**
 * print_circ_status
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 * @mst: pointer to an instance of mem_status structure
 *
 * Prints a snapshot of the status of a SHM.
 */
static void print_circ_status(struct shmem_link_device *shmd, int dev,
			struct mem_status *mst)
{
	struct link_device *ld = &shmd->ld;
	struct utc_time utc;

	if (dev > IPC_RAW)
		return;

	ts2utc(&mst->ts, &utc);
	pr_info("%s: [%02d:%02d:%02d.%06d] "
		"[%s] <%s> %s | TXQ{in:%u out:%u} RXQ{in:%u out:%u} "
		"INTR{RX:0x%X TX:0x%X}\n",
		MIF_TAG, utc.hour, utc.min, utc.sec, ns2us(mst->ts.tv_nsec),
		get_dir_str(mst->dir), ld->name, get_dev_name(dev),
		mst->head[dev][TX], mst->tail[dev][TX],
		mst->head[dev][RX], mst->tail[dev][RX],
		mst->int2ap, mst->int2cp);
}

/**
 * print_shmem_status
 * @shmd: pointer to an instance of shmem_link_device structure
 * @mst: pointer to an instance of mem_status structure
 *
 * Prints a snapshot of the status of a SHM.
 */
static void print_shmem_status(struct shmem_link_device *shmd,
			struct mem_status *mst)
{
	struct link_device *ld = &shmd->ld;
	int us = ns2us(mst->ts.tv_nsec);
	struct utc_time utc;

	ts2utc(&mst->ts, &utc);
	pr_info("%s: %s: [%02d:%02d:%02d.%06d] "
		"[%s] ACC{%X %d} "
		"FMT{TI:%u TO:%u RI:%u RO:%u} "
		"RAW{TI:%u TO:%u RI:%u RO:%u} "
		"INTR{RX:0x%X TX:0x%X}\n",
		MIF_TAG, ld->name, utc.hour, utc.min, utc.sec, us,
		get_dir_str(mst->dir), mst->magic, mst->access,
		mst->head[IPC_FMT][TX], mst->tail[IPC_FMT][TX],
		mst->head[IPC_FMT][RX], mst->tail[IPC_FMT][RX],
		mst->head[IPC_RAW][TX], mst->tail[IPC_RAW][TX],
		mst->head[IPC_RAW][RX], mst->tail[IPC_RAW][RX],
		mst->int2ap, mst->int2cp);
}

/**
 * save_shmem_dump_work
 * @work: pointer to an instance of work_struct structure
 *
 * Performs actual file operation for saving a SHM dump.
 */
static void save_shmem_dump_work(struct work_struct *work)
{
	struct shmem_link_device *shmd;
	struct link_device *ld;
	struct trace_queue *trq;
	struct trace_data *trd;
	struct mem_stat_queue *msq;
	struct mem_status *mst;
	struct file *fp;
	struct timespec *ts;
	u8 *dump;
	int rcvd;
	char *path;
	struct utc_time utc;
	int us;

	shmd = container_of(work, struct shmem_link_device, dump_dwork.work);
	ld = &shmd->ld;
	trq = &shmd->dump_list;
	msq = &shmd->stat_list;
	path = shmd->dump_path;

	while (1) {
		trd = trq_get_data_slot(trq);
		if (!trd)
			break;

		ts = &trd->ts;
		dump = trd->data;
		rcvd = trd->size;

		ts2utc(ts, &utc);
		snprintf(path, MIF_MAX_PATH_LEN,
			"%s/%s_dump_%d%02d%02d-%02d%02d%02d.hex",
			MIF_LOG_DIR, ld->name, utc.year, utc.mon, utc.day,
			utc.hour, utc.min, utc.sec);

		fp = mif_open_file(path);
		if (fp) {
			mif_save_file(fp, dump, rcvd);
			mif_close_file(fp);
		} else {
			mif_err("%s: ERR! %s open fail\n", ld->name, path);
			mif_print_dump(dump, rcvd, 16);
		}

		kfree(dump);
	}

	dump = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!dump)
		return;

	snprintf(path, MIF_MAX_PATH_LEN,
		"%s/%s_history_%d%02d%02d-%02d%02d%02d.lst",
		MIF_LOG_DIR, ld->name, utc.year, utc.mon, utc.day,
		utc.hour, utc.min, utc.sec);
	fp = mif_open_file(path);
	if (!fp)
		mif_err("%s: ERR! %s open fail\n", ld->name, path);

	while (1) {
		mst = msq_get_data_slot(msq);
		if (!mst)
			break;

		if (fp) {
			memset(dump, 0, PAGE_SIZE);
			ts2utc(&mst->ts, &utc);
			us = ns2us(mst->ts.tv_nsec);
			snprintf(dump, PAGE_SIZE, "[%02d:%02d:%02d.%06d] "
				"[%s] ACC{%X %d} "
				"FMT{TI:%u TO:%u RI:%u RO:%u} "
				"RAW{TI:%u TO:%u RI:%u RO:%u} "
				"INTR{RX:0x%X TX:0x%X}\n",
				utc.hour, utc.min, utc.sec, us,
				get_dir_str(mst->dir), mst->magic, mst->access,
				mst->head[IPC_FMT][TX], mst->tail[IPC_FMT][TX],
				mst->head[IPC_FMT][RX], mst->tail[IPC_FMT][RX],
				mst->head[IPC_RAW][TX], mst->tail[IPC_RAW][TX],
				mst->head[IPC_RAW][RX], mst->tail[IPC_RAW][RX],
				mst->int2ap, mst->int2cp);
			mif_save_file(fp, dump, strlen(dump));
		} else {
			print_shmem_status(shmd, mst);
		}
	}

	if (fp)
		mif_close_file(fp);

	kfree(dump);
}

/**
 * save_shmem_dump
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Saves SHM dumps.
 *
 * Actual file operation (save) will be performed by save_shmem_dump_work() that
 * is invoked by a delayed work.
 */
static void save_shmem_dump(struct shmem_link_device *shmd)
{
	queue_delayed_work(system_nrt_wq, &shmd->dump_dwork, 0);
}

/**
 * capture_shmem_dump
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Captures current SHM dump.
 *
 */
static void capture_shmem_dump(struct shmem_link_device *shmd)
{
	struct link_device *ld = &shmd->ld;
	struct trace_data *trd;
	u8 *buff;
	struct timespec ts;

	buff = kzalloc(shmd->size, GFP_ATOMIC);
	if (!buff) {
		mif_err("%s: ERR! kzalloc fail\n", ld->name);
		return;
	}

	getnstimeofday(&ts);

	memcpy(buff, shmd->base, shmd->size);

	trd = trq_get_free_slot(&shmd->dump_list);
	if (!trd) {
		mif_err("%s: ERR! trq_get_free_slot fail\n", ld->name);
		mif_print_dump(buff, shmd->size, 16);
		kfree(buff);
		return;
	}

	memcpy(&trd->ts, &ts, sizeof(struct timespec));
	trd->data = buff;
	trd->size = shmd->size;
}

/**
 * print_ipc_trace
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 * @stat: pointer to an instance of memif_circ_status structure
 * @ts: pointer to an instance of timespec structure
 * @buff: start address of a buffer into which RX IPC messages were copied
 * @rcvd: size of data in the buffer
 *
 * Prints IPC messages in a local memory buffer to a kernel log.
 */
static void print_ipc_trace(struct shmem_link_device *shmd, int dev,
			struct memif_circ_status *stat, struct timespec *ts,
			u8 *buff, u32 rcvd)
{
	struct link_device *ld = &shmd->ld;
	struct utc_time utc;

	ts2utc(ts, &utc);

	pr_info("%s: [%d-%02d-%02d %02d:%02d:%02d.%03d] "
		"%s %s_RXQ {IN:%u OUT:%u LEN:%d}\n",
		MIF_TAG, utc.year, utc.mon, utc.day, utc.hour, utc.min, utc.sec,
		utc.msec, ld->name, get_dev_name(dev), stat->in, stat->out,
		stat->size);

	mif_print_dump(buff, rcvd, 4);

	return;
}

/**
 * save_ipc_trace_work
 * @work: pointer to an instance of work_struct structure
 *
 * Performs actual file operation for saving RX IPC trace.
 */
static void save_ipc_trace_work(struct work_struct *work)
{
	struct shmem_link_device *shmd;
	struct link_device *ld;
	struct trace_queue *trq;
	struct trace_data *trd;
	struct memif_circ_status *stat;
	struct file *fp;
	struct timespec *ts;
	int dev;
	u8 *dump;
	int rcvd;
	u8 *buff;
	char *path;
	struct utc_time utc;

	shmd = container_of(work, struct shmem_link_device, trace_dwork.work);
	ld = &shmd->ld;
	trq = &shmd->trace_list;
	path = shmd->trace_path;

	buff = kzalloc(shmd->size << 3, GFP_KERNEL);
	if (!buff) {
		while (1) {
			trd = trq_get_data_slot(trq);
			if (!trd)
				break;

			ts = &trd->ts;
			dev = trd->dev;
			stat = &trd->stat;
			dump = trd->data;
			rcvd = trd->size;
			print_ipc_trace(shmd, dev, stat, ts, dump, rcvd);

			kfree(dump);
		}
		return;
	}

	while (1) {
		trd = trq_get_data_slot(trq);
		if (!trd)
			break;

		ts = &trd->ts;
		dev = trd->dev;
		stat = &trd->stat;
		dump = trd->data;
		rcvd = trd->size;

		ts2utc(ts, &utc);
		snprintf(path, MIF_MAX_PATH_LEN,
			"%s/%s_%s_%d%02d%02d-%02d%02d%02d.lst",
			MIF_LOG_DIR, ld->name, get_dev_name(dev),
			utc.year, utc.mon, utc.day, utc.hour, utc.min, utc.sec);

		fp = mif_open_file(path);
		if (fp) {
			int len;

			snprintf(buff, MIF_MAX_PATH_LEN,
				"[%d-%02d-%02d %02d:%02d:%02d.%03d] "
				"%s %s_RXQ {IN:%u OUT:%u LEN:%d}\n",
				utc.year, utc.mon, utc.day, utc.hour, utc.min,
				utc.sec, utc.msec, ld->name, get_dev_name(dev),
				stat->in, stat->out, stat->size);
			len = strlen(buff);
			mif_dump2format4(dump, rcvd, (buff + len), NULL);
			strcat(buff, "\n");
			len = strlen(buff);

			mif_save_file(fp, buff, len);

			memset(buff, 0, len);
			mif_close_file(fp);
		} else {
			mif_err("%s: %s open fail\n", ld->name, path);
			print_ipc_trace(shmd, dev, stat, ts, dump, rcvd);
		}

		kfree(dump);
	}

	kfree(buff);
}

/**
 * save_ipc_trace
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 * @stat: pointer to an instance of memif_circ_status structure
 *
 * Saves IPC messages currently in an RX circular queue.
 *
 * Actual file operation (save) will be performed by save_ipc_trace_work() that
 * is invoked by a delayed work.
 */
static void save_ipc_trace(struct shmem_link_device *shmd, int dev,
			struct memif_circ_status *stat)
{
	struct link_device *ld = &shmd->ld;
	struct trace_data *trd;
	u8 *buff;
	struct timespec ts;

	getnstimeofday(&ts);

	buff = kzalloc(stat->size, GFP_ATOMIC);
	if (!buff) {
		mif_err("%s: %s: ERR! kzalloc fail\n",
			ld->name, get_dev_name(dev));

		/* Copy IPC messages from a SHM RXQ to a local buffer */
		buff = shmd->buff;
		memset(buff, 0, shmd->size);
		circ_read(buff, stat->buff, stat->qsize, stat->out, stat->size);

		/* Print IPC messages in the local buffer to a kernel log */
		print_ipc_trace(shmd, dev, stat, &ts, buff, stat->size);

		return;
	}

	circ_read(buff, stat->buff, stat->qsize, stat->out, stat->size);

	trd = trq_get_free_slot(&shmd->trace_list);
	if (!trd) {
		mif_err("%s: %s: ERR! trq_get_free_slot fail\n",
			ld->name, get_dev_name(dev));
		print_ipc_trace(shmd, dev, stat, &ts, buff, stat->size);
		kfree(buff);
		return;
	}

	memcpy(&trd->ts, &ts, sizeof(struct timespec));
	trd->dev = dev;
	memcpy(&trd->stat, stat, sizeof(struct memif_circ_status));
	trd->data = buff;
	trd->size = stat->size;

	queue_delayed_work(system_nrt_wq, &shmd->trace_dwork, 0);
}
#endif

/**
 * check_magic_access
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Returns 0 if the "magic code" and "access enable" values are valid, otherwise
 * returns -EACCES.
 */
static int check_magic_access(struct shmem_link_device *shmd)
{
	struct link_device *ld = &shmd->ld;
	int i;
	u16 magic = get_magic(shmd);
	u16 access = get_access(shmd);

	/* Returns 0 if the "magic code" and "access enable" are valid */
	if (likely(magic == DPRAM_MAGIC_CODE && access == 1))
		return 0;

	/* Retry up to 100 times with 100 us delay per each retry */
	for (i = 1; i <= 100; i++) {
		mif_err("%s: magic:%X access:%X -> retry:%d\n",
			ld->name, magic, access, i);
		udelay(100);

		magic = get_magic(shmd);
		access = get_access(shmd);
		if (likely(magic == DPRAM_MAGIC_CODE && access == 1))
			return 0;
	}

	mif_err("%s: !CRISIS! magic:%X access:%X\n", ld->name, magic, access);
	return -EACCES;
}

/**
 * ipc_active
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Returns whether or not IPC via the shmem_link_device instance is possible.
 */
static bool ipc_active(struct shmem_link_device *shmd)
{
	struct link_device *ld = &shmd->ld;

	/* Check link mode */
	if (ld->mode != LINK_MODE_IPC) {
		mif_err("%s: <called by %pf> ERR! ld->mode != LINK_MODE_IPC\n",
			ld->name, CALLER);
		return false;
	}

	/* Check "magic code" and "access enable" values */
	if (check_magic_access(shmd) < 0) {
		mif_err("%s: <called by %pf> ERR! check_magic_access fail\n",
			ld->name, CALLER);
		return false;
	}

	return true;
}

static inline int c2c_wake_cp_up(struct shmem_link_device *shmd)
{
	int cnt = 0;

	gpio_set_value(shmd->gpio_cp_wakeup, 1);

	while (!gpio_get_value(shmd->gpio_cp_status) || c2c_read_link()) {
		if (cnt++ > 10) {
			if (in_irq())
				mif_err("ERR! gpio_cp_status == 0 in IRQ\n");
			else
				mif_err("ERR! gpio_cp_status == 0\n");
			return -EACCES;
		}

		mif_info("gpio_cp_status == 0 (cnt %d)\n", cnt);
		if (in_interrupt())
			udelay(1000);
		else
			usleep_range(1000, 2000);
	}

	return 0;
}

static inline void c2c_allow_cp_sleep(struct shmem_link_device *shmd)
{
	gpio_set_value(shmd->gpio_cp_wakeup, 0);
}

/**
 * wake_cp_up
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Wakes up a CP if it can sleep and increases the "accessing" counter in the
 * shmem_link_device instance.
 *
 * CAUTION!!! allow_cp_sleep() MUST be invoked after wake_cp_up() success
 * to decrease the "accessing" counter.
 */
static int wake_cp_up(struct shmem_link_device *shmd)
{
	struct link_device *ld = &shmd->ld;

	if (shmd->type == C2C_SHMEM) {
		if (c2c_wake_cp_up(shmd) < 0) {
			mif_err("%s: <called by %pf> ERR! wake_cp_up fail\n",
				ld->name, CALLER);
			return -EACCES;
		}

		atomic_inc(&shmd->accessing);
	}

	return 0;
}

/**
 * allow_cp_sleep
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Decreases the "accessing" counter in the shmem_link_device instance if it can
 * sleep and allows a CP to sleep only if the value of "accessing" counter is
 * less than or equal to 0.
 *
 * MUST be invoked after wake_cp_up() success to decrease the "accessing"
 * counter.
 */
static void allow_cp_sleep(struct shmem_link_device *shmd)
{
	struct link_device *ld = &shmd->ld;

	if (shmd->type == C2C_SHMEM) {
		if (atomic_dec_return(&shmd->accessing) <= 0) {
			c2c_allow_cp_sleep(shmd);
			atomic_set(&shmd->accessing, 0);
			mif_err("%s: CP sleep possible\n", ld->name);
		}
	}
}

/**
 * handle_no_cp_crash_ack
 * @arg: pointer to an instance of shmem_link_device structure
 *
 * Invokes handle_cp_crash() to enter the CRASH_EXIT state if there was no
 * CRASH_ACK from a CP in FORCE_CRASH_ACK_TIMEOUT.
 */
static void handle_no_cp_crash_ack(unsigned long arg)
{
	struct shmem_link_device *shmd = (struct shmem_link_device *)arg;
	struct link_device *ld = &shmd->ld;

	mif_err("%s: ERR! No CRASH_EXIT ACK from CP\n", ld->mc->name);

	if (!wake_lock_active(&shmd->wlock))
		wake_lock(&shmd->wlock);

	handle_cp_crash(shmd);
}

/**
 * trigger_force_cp_crash
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Triggers an enforced CP crash.
 */
static void trigger_force_cp_crash(struct shmem_link_device *shmd)
{
	struct link_device *ld = &shmd->ld;

	shmd->forced_cp_crash = true;

	if (ld->mode == LINK_MODE_ULOAD) {
		mif_err("%s: CP crash is already in progress\n", ld->mc->name);
		return;
	}

	ld->mode = LINK_MODE_ULOAD;

	mif_err("%s: <called by %pf>\n", ld->name, CALLER);

	wake_cp_up(shmd);
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	/* Take a SHM dump */
	capture_shmem_dump(shmd);
#endif

	/* Send CRASH_EXIT command to a CP */
	send_int2cp(shmd, INT_CMD(INT_CMD_CRASH_EXIT));
	get_shmem_status(shmd, TX, msq_get_free_slot(&shmd->stat_list));

	/* If there is no CRASH_ACK from a CP in FORCE_CRASH_ACK_TIMEOUT,
	   handle_no_cp_crash_ack() will be executed. */
	mif_add_timer(&shmd->crash_ack_timer, FORCE_CRASH_ACK_TIMEOUT,
			handle_no_cp_crash_ack, (unsigned long)shmd);

	return;
}

/**
 * udl_command_handler
 * @shmd: pointer to an instance of shmem_link_device structure
 * @cmd: SHM upload/download command from a CP
 *
 * Processes a command for upload/download from a CP.
 */
static void udl_command_handler(struct shmem_link_device *shmd, u16 cmd)
{
	struct link_device *ld = &shmd->ld;

	if (cmd & UDL_RESULT_FAIL) {
		mif_err("%s: ERR! Command failed: %04x\n", ld->name, cmd);
		return;
	}

	switch (UDL_CMD_MASK(cmd)) {
	case UDL_CMD_RECV_READY:
		mif_err("%s: [CP->AP] UDL_CMD_RECV_READY\n", ld->name);
		send_int2cp(shmd, CMD_IMG_START_REQ);
		break;

	default:
		complete_all(&shmd->udl_cmpl);
	}
}

/**
 * cmd_req_active_handler
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Handles the REQ_ACTIVE command from a CP.
 */
static void cmd_req_active_handler(struct shmem_link_device *shmd)
{
	send_int2cp(shmd, INT_CMD(INT_CMD_RES_ACTIVE));
}

/**
 * cmd_crash_reset_handler
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Handles the CRASH_RESET command from a CP.
 */
static void cmd_crash_reset_handler(struct shmem_link_device *shmd)
{
	struct link_device *ld = &shmd->ld;
	struct io_device *iod = NULL;
	int i;

	ld->mode = LINK_MODE_ULOAD;

	if (!wake_lock_active(&shmd->wlock))
		wake_lock(&shmd->wlock);

	/* Stop network interfaces */
	mif_netif_stop(ld);

	/* Purge the skb_txq in every IPC device (IPC_FMT, IPC_RAW, etc.) */
	for (i = 0; i < ld->max_ipc_dev; i++)
		skb_queue_purge(ld->skb_txq[i]);

	mif_err("%s: Recv 0xC7 (CRASH_RESET)\n", ld->name);

	/* Change the modem state to STATE_CRASH_RESET for the FMT IO device */
	iod = link_get_iod_with_format(ld, IPC_FMT);
	iod->modem_state_changed(iod, STATE_CRASH_RESET);

	/* Change the modem state to STATE_CRASH_RESET for the BOOT IO device */
	iod = link_get_iod_with_format(ld, IPC_BOOT);
	iod->modem_state_changed(iod, STATE_CRASH_RESET);
}

/**
 * handle_cp_crash
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Actual handler for the CRASH_EXIT command from a CP.
 */
static void handle_cp_crash(struct shmem_link_device *shmd)
{
	struct link_device *ld = &shmd->ld;
	struct io_device *iod;
	int i;

	if (shmd->forced_cp_crash)
		shmd->forced_cp_crash = false;

	/* Stop network interfaces */
	mif_netif_stop(ld);

	/* Purge the skb_txq in every IPC device (IPC_FMT, IPC_RAW, etc.) */
	for (i = 0; i < ld->max_ipc_dev; i++)
		skb_queue_purge(ld->skb_txq[i]);

#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	save_shmem_dump(shmd);
#endif

	/* Change the modem state to STATE_CRASH_EXIT for the FMT IO device */
	iod = link_get_iod_with_format(ld, IPC_FMT);
	if (iod)
		iod->modem_state_changed(iod, STATE_CRASH_EXIT);

	/* Change the modem state to STATE_CRASH_EXIT for the BOOT IO device */
	iod = link_get_iod_with_format(ld, IPC_BOOT);
	if (iod)
		iod->modem_state_changed(iod, STATE_CRASH_EXIT);
}

/**
 * cmd_crash_exit_handler
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Handles the CRASH_EXIT command from a CP.
 */
static void cmd_crash_exit_handler(struct shmem_link_device *shmd)
{
	struct link_device *ld = &shmd->ld;

	mif_err("%s: Recv 0xC9 (CRASH_EXIT)\n", ld->name);
	return;

	ld->mode = LINK_MODE_ULOAD;

	if (!wake_lock_active(&shmd->wlock))
		wake_lock(&shmd->wlock);

	del_timer(&shmd->crash_ack_timer);

	wake_cp_up(shmd);
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	if (!shmd->forced_cp_crash)
		capture_shmem_dump(shmd);
#endif

	mif_err("%s: Recv 0xC9 (CRASH_EXIT)\n", ld->name);

	handle_cp_crash(shmd);
}

/**
 * reset_shmem_ipc
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Reset SHM with IPC map.
 */
static void reset_shmem_ipc(struct shmem_link_device *shmd)
{
	int i;
	struct link_device *ld = &shmd->ld;

	set_access(shmd, 0);

	/* Clear pointers in every circular queue */
	for (i = 0; i < ld->max_ipc_dev; i++) {
		set_txq_head(shmd, i, 0);
		set_txq_tail(shmd, i, 0);
		set_rxq_head(shmd, i, 0);
		set_rxq_tail(shmd, i, 0);
	}

	for (i = 0; i < ld->max_ipc_dev; i++)
		atomic_set(&shmd->res_required[i], 0);

	atomic_set(&shmd->accessing, 0);

	if (wake_lock_active(&shmd->wlock))
		wake_unlock(&shmd->wlock);

	set_magic(shmd, DPRAM_MAGIC_CODE);
	set_access(shmd, 1);
}

/**
 * init_shmem_ipc
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Initializes IPC via SHM.
 */
static int init_shmem_ipc(struct shmem_link_device *shmd)
{
	int i;
	struct link_device *ld = &shmd->ld;

	if (ld->mode == LINK_MODE_IPC &&
	    get_magic(shmd) == DPRAM_MAGIC_CODE &&
	    get_access(shmd) == 1) {
		mif_err("%s: IPC already initialized\n", ld->name);
		return 0;
	}

	if (!shmd->setup_done) {
		/* Initialize variables for efficient TX/RX processing */
		for (i = 0; i < ld->max_ipc_dev; i++)
			shmd->iod[i] = link_get_iod_with_format(ld, i);
		shmd->iod[IPC_RAW] = link_get_iod_with_format(ld,
							IPC_MULTI_RAW);
	}

	reset_shmem_ipc(shmd);

	if (get_magic(shmd) != DPRAM_MAGIC_CODE || get_access(shmd) != 1)
		return -EACCES;

	ld->mode = LINK_MODE_IPC;

	return 0;
}

/**
 * cmd_phone_start_handler
 * @shmd: pointer to an instance of shmem_link_device structure
 *
 * Handles the PHONE_START command from a CP.
 */
static void cmd_phone_start_handler(struct shmem_link_device *shmd)
{
	int err;
	struct link_device *ld = &shmd->ld;
	struct io_device *iod;

	mif_err("%s: Recv 0xC8 (CP_START)\n", ld->name);

	iod = link_get_iod_with_format(ld, IPC_FMT);
	if (!iod) {
		mif_err("%s: ERR! no iod\n", ld->name);
		return;
	}

	err = init_shmem_ipc(shmd);
	if (err)
		return;

	if (!shmd->setup_done)
		shmd->setup_done = true;

	if (iod->mc->phone_state != STATE_ONLINE)
		iod->modem_state_changed(iod, STATE_ONLINE);

	mif_err("%s: Send 0xC2 (INIT_END)\n", ld->name);
	send_int2cp(shmd, INT_CMD(INT_CMD_INIT_END));
}

/**
 * command_handler: processes a SHM command from a CP
 * @shmd: pointer to an instance of shmem_link_device structure
 * @cmd: SHM command from a CP
 */
static void command_handler(struct shmem_link_device *shmd, u16 cmd)
{
	struct link_device *ld = &shmd->ld;

	switch (INT_CMD_MASK(cmd)) {
	case INT_CMD_REQ_ACTIVE:
		cmd_req_active_handler(shmd);
		break;

	case INT_CMD_CRASH_RESET:
		cmd_crash_reset_handler(shmd);
		break;

	case INT_CMD_CRASH_EXIT:
		cmd_crash_exit_handler(shmd);
		break;

	case INT_CMD_PHONE_START:
		cmd_phone_start_handler(shmd);
		complete_all(&ld->init_cmpl);
		break;

	case INT_CMD_NV_REBUILDING:
		mif_err("%s: NV_REBUILDING\n", ld->name);
		break;

	case INT_CMD_PIF_INIT_DONE:
		complete_all(&ld->pif_cmpl);
		break;

	case INT_CMD_SILENT_NV_REBUILDING:
		mif_err("%s: SILENT_NV_REBUILDING\n", ld->name);
		break;

	case INT_CMD_NORMAL_POWER_OFF:
		/*ToDo:*/
		/*kernel_sec_set_cp_ack()*/;
		break;

	case INT_CMD_REQ_TIME_SYNC:
	case INT_CMD_CP_DEEP_SLEEP:
	case INT_CMD_EMER_DOWN:
		break;

	default:
		mif_err("%s: unknown command 0x%04X\n", ld->name, cmd);
	}
}

/**
 * ipc_rx_task
 * @data: pointer to an instance of shmem_link_device structure
 *
 * Invokes the recv method in the io_device instance to perform receiving IPC
 * messages from each mif_rxb.
 */
static void ipc_rx_task(unsigned long data)
{
	struct shmem_link_device *shmd = (struct shmem_link_device *)data;
	struct link_device *ld = &shmd->ld;
	struct io_device *iod;
	struct sk_buff *skb;
	int i;

	for (i = 0; i < ld->max_ipc_dev; i++) {
		iod = shmd->iod[i];
		while (1) {
			skb = skb_dequeue(ld->skb_rxq[i]);
			if (!skb)
				break;
			iod->recv_skb(iod, ld, skb);
		}
	}
}

/**
 * get_rxq_rcvd
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 * @mst: pointer to an instance of mem_status structure
 * OUT @mcst: pointer to an instance of memif_circ_status structure
 *
 * Stores {start address of the buffer in a RXQ, size of the buffer, in & out
 * pointer values, size of received data} into the 'stat' instance.
 *
 * Returns an error code.
 */
static int get_rxq_rcvd(struct shmem_link_device *shmd, int dev,
			struct mem_status *mst, struct memif_circ_status *mcst)
{
	struct link_device *ld = &shmd->ld;

	mcst->buff = get_rxq_buff(shmd, dev);
	mcst->qsize = get_rxq_buff_size(shmd, dev);
	mcst->in = mst->head[dev][RX];
	mcst->out = mst->tail[dev][RX];
	mcst->size = circ_get_usage(mcst->qsize, mcst->in, mcst->out);

	if (circ_valid(mcst->qsize, mcst->in, mcst->out)) {
		mif_err("%s: %s_RXQ qsize[%u] in[%u] out[%u] rcvd[%u]\n",
			ld->name, get_dev_name(dev), mcst->qsize, mcst->in,
			mcst->out, mcst->size);
		return 0;
	} else {
		mif_err("%s: ERR! %s_RXQ invalid (qsize[%d] in[%d] out[%d])\n",
			ld->name, get_dev_name(dev), mcst->qsize, mcst->in,
			mcst->out);
		return -EIO;
	}
}

/**
 * recv_ipc_with_skb: receives SIPC5 messages from an RXQ with skb
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 * @mst: pointer to an instance of mem_status structure
 *
 * Returns
 *   ret < 0  : error
 *   ret == 0 : ILLEGAL status
 *   ret > 0  : valid data
 *
 * Must be invoked only when there is data in the corresponding RXQ.
 *
 * Requires a recv_skb method in the io_device instance, so this function must
 * be used for only SIPC5.
 */
static int recv_ipc_with_skb(struct shmem_link_device *shmd, int dev,
			struct mem_status *mst)
{
	struct link_device *ld = &shmd->ld;
	struct sk_buff *skb;
	/**
	 * variables for the status of the circular queue
	 */
	u8 __iomem *src;
	struct memif_circ_status mcst;
	/**
	 * variables for RX processing
	 */
	int qsize;	/* size of the queue			*/
	int rcvd;	/* size of data in the RXQ or error	*/
	int rest;	/* size of the rest data		*/
	int idx;	/* index to the start of current frame	*/
	u8 *frm;	/* pointer to current frame		*/
	u8 *dst;	/* pointer to the destination buffer	*/
	int len;	/* length of current frame		*/
	int tot;	/* total length including padding data	*/

	/* Get data size in the RXQ and in/out pointer values */
	rcvd = get_rxq_rcvd(shmd, dev, mst, &mcst);
	if (unlikely(rcvd < 0)) {
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
		trigger_force_cp_crash(shmd);
#endif
		goto exit;
	}
	rcvd = mcst.size;

	src = mcst.buff;
	qsize = mcst.qsize;
	rest = mcst.size;
	idx = mcst.out;

	while (rest > 0) {
		/* Calculate the start of an SIPC5 frame */
		frm = src + idx;

		/* Check the SIPC5 frame */
		len = sipc5_check_frame_in_dev(ld, dev, frm, rest);
		if (len <= 0) {
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
			save_ipc_trace(shmd, dev, &mcst);
			trigger_force_cp_crash(shmd);
#endif
			rcvd = -EBADMSG;
			goto exit;
		}

		/* Calculate total length of the frame (data + padding) */
		tot = len + sipc5_calc_padding_size(len);

		/* Allocate an skb */
		skb = dev_alloc_skb(tot);
		if (!skb) {
			mif_err("%s: ERR! %s dev_alloc_skb fail\n",
				ld->name, get_dev_name(dev));
			rcvd = -ENOMEM;
			goto exit;
		}

		/* Read the frame from the RXQ */
		dst = skb_put(skb, tot);
		circ_read(dst, src, qsize, idx, tot);

#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
		/* Take a log for debugging */
		if (unlikely(dev == IPC_FMT)) {
			char str[MIF_MAX_STR_LEN];
			snprintf(str, MIF_MAX_STR_LEN, "%s: CP2MIF",
				ld->mc->name);
			pr_ipc(str, skb->data, (skb->len > 20 ? 20 : skb->len));
		}
#endif

#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
		/* Verify data copied to the skb */
		if (ld->aligned && memcmp16_to_io(frm, dst, 4)) {
			mif_err("%s: memcmp16_to_io fail\n", ld->name);
			trigger_force_cp_crash(shmd);
			rcvd = -EIO;
			goto exit;
		}
#endif

		/* Remove padding in the skb */
		skb_trim(skb, len);

		/* Store the frame to the corresponding skb_rxq */
		skb_queue_tail(ld->skb_rxq[dev], skb);

		/* Calculate new idx value */
		rest -= tot;
		idx += tot;
		if (unlikely(idx >= qsize))
			idx -= qsize;
	}

exit:
	/* Update tail (out) pointer to empty out the RXQ */
	set_rxq_tail(shmd, dev, mcst.in);

	return rcvd;
}

/**
 * recv_ipc_msg: receives IPC messages from every RXQ
 * @shmd: pointer to an instance of shmem_link_device structure
 * @stat: pointer to an instance of mem_status structure
 *
 * 1) Receives all IPC message frames currently in every IPC RXQ.
 * 2) Sends RES_ACK responses if there are REQ_ACK requests from a CP.
 * 3) Completes all threads waiting for the corresponding RES_ACK from a CP if
 *    there is any RES_ACK response.
 */
static void recv_ipc_msg(struct shmem_link_device *shmd,
			struct mem_status *stat)
{
	struct link_device *ld = &shmd->ld;
	int i = 0;
	int ret = 0;
	u16 mask = 0;
	u16 intr = stat->int2ap;

	if (!ipc_active(shmd))
		return;

	/* Read data from every RXQ */
	for (i = 0; i < ld->max_ipc_dev; i++) {
		/* Invoke an RX function only when there is data in the RXQ */
		if (unlikely(stat->head[i][RX] == stat->tail[i][RX])) {
			mif_err("%s: %s_RXQ is empty\n",
				ld->name, get_dev_name(i));
		} else {
			ret = recv_ipc_with_skb(shmd, i, stat);
			if (ret < 0)
				reset_rxq_circ(shmd, i);
		}
	}

	/* Schedule soft IRQ for RX */
	tasklet_hi_schedule(&shmd->rx_tsk);

	/* Check and process REQ_ACK (at this time, in == out) */
	if (unlikely(intr & INT_MASK_REQ_ACK_SET)) {
		for (i = 0; i < ld->max_ipc_dev; i++) {
			if (intr & get_mask_req_ack(shmd, i)) {
				mif_err("%s: set %s_RES_ACK\n",
					ld->name, get_dev_name(i));
				mask |= get_mask_res_ack(shmd, i);
			}
		}

		send_int2cp(shmd, INT_NON_CMD(mask));
	}

	/* Check and process RES_ACK */
	if (unlikely(intr & INT_MASK_RES_ACK_SET)) {
		for (i = 0; i < ld->max_ipc_dev; i++) {
			if (intr & get_mask_res_ack(shmd, i)) {
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
				mif_err("%s: recv %s_RES_ACK\n",
					ld->name, get_dev_name(i));
				print_circ_status(shmd, i, stat);
#endif
				complete(&shmd->req_ack_cmpl[i]);
			}
		}
	}
}

/**
 * cmd_msg_handler: processes a SHM command or receives IPC messages
 * @shmd: pointer to an instance of shmem_link_device structure
 * @stat: pointer to an instance of mem_status structure
 *
 * Invokes command_handler for a SHM command or recv_ipc_msg for IPC messages.
 */
static inline void cmd_msg_handler(struct shmem_link_device *shmd,
			struct mem_status *stat)
{
	struct mem_status *mst = msq_get_free_slot(&shmd->stat_list);
	u16 intr = stat->int2ap;

	memcpy(mst, stat, sizeof(struct mem_status));

	if (unlikely(INT_CMD_VALID(intr)))
		command_handler(shmd, intr);
	else
		recv_ipc_msg(shmd, stat);
}

/**
 * intr_handler: processes an interrupt from a CP
 * @shmd: pointer to an instance of shmem_link_device structure
 * @stat: pointer to an instance of mem_status structure
 *
 * Call flow for normal interrupt handling:
 *   cmd_msg_handler -> command_handler -> cmd_xxx_handler
 *   cmd_msg_handler -> recv_ipc_msg -> recv_ipc_with_skb ...
 */
static inline void intr_handler(struct shmem_link_device *shmd,
			struct mem_status *stat)
{
	char *name = shmd->ld.name;
	u16 intr = stat->int2ap;

	if (unlikely(intr == INT_POWERSAFE_FAIL)) {
		mif_err("%s: intr == INT_POWERSAFE_FAIL\n", name);
		return;
	}

	if (unlikely(EXT_UDL_CMD(intr))) {
		if (likely(EXT_INT_VALID(intr))) {
			if (UDL_CMD_VALID(intr))
				udl_command_handler(shmd, intr);
			else
				mif_err("%s: ERR! invalid intr 0x%04X\n",
					name, intr);
		} else {
			mif_err("%s: ERR! invalid intr 0x%04X\n", name, intr);
		}

		return;
	}

	if (likely(INT_VALID(intr)))
		cmd_msg_handler(shmd, stat);
	else
		mif_err("%s: ERR! invalid intr 0x%04X\n", name, intr);
}

/**
 * c2c_irq_handler: interrupt handler for a C2C interrupt
 * @data: pointer to a data
 *
 * 1) Reads the interrupt value
 * 2) Performs interrupt handling
 */
static void c2c_irq_handler(void *data)
{
	struct shmem_link_device *shmd = (struct shmem_link_device *)data;
	struct link_device *ld = (struct link_device *)&shmd->ld;
	struct mem_status stat;
	mif_err("%s: +++\n", ld->name);

	if (unlikely(ld->mode == LINK_MODE_OFFLINE))
		goto exit;

	if (wake_cp_up(shmd) < 0) {
		trigger_force_cp_crash(shmd);
		goto exit;
	}

	get_shmem_status(shmd, RX, &stat);
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	print_shmem_status(shmd, &stat);
#endif

	intr_handler(shmd, &stat);

	clear_int2ap(shmd);

	allow_cp_sleep(shmd);

exit:
	mif_err("%s: ---\n", ld->name);
	return;
}

/**
 * ap_wakeup_irq_handler: interrupt handler for a wakeup interrupt
 * @irq: IRQ number
 * @data: pointer to a data
 *
 * 1) Reads the interrupt value
 * 2) Performs interrupt handling
 */
static irqreturn_t ap_wakeup_irq_handler(int irq, void *data)
{
	struct shmem_link_device *shmd = (struct shmem_link_device *)data;
	struct link_device *ld = (struct link_device *)&shmd->ld;
	int c2c_wakeup = gpio_get_value(shmd->gpio_ap_wakeup);
	mif_err("+++\n");

	if (unlikely(ld->mode == LINK_MODE_OFFLINE))
		goto exit;

	if (wake_cp_up(shmd) < 0) {
		trigger_force_cp_crash(shmd);
		goto exit;
	}

	if (c2c_wakeup) {
		irq_set_irq_type(shmd->gpio_ap_wakeup, IRQ_TYPE_EDGE_FALLING);
		wake_lock(&shmd->wlock);
	} else {
		irq_set_irq_type(shmd->gpio_ap_wakeup, IRQ_TYPE_EDGE_RISING);
		wake_unlock(&shmd->wlock);
	}

	allow_cp_sleep(shmd);

exit:
	mif_err("---\n");
	return IRQ_HANDLED;
}

/**
 * get_txq_space
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 * OUT @stat: pointer to an instance of memif_circ_status structure
 *
 * Stores {start address of the buffer in a TXQ, size of the buffer, in & out
 * pointer values, size of free space} into the 'stat' instance.
 *
 * Returns the size of free space in the buffer or an error code.
 */
static int get_txq_space(struct shmem_link_device *shmd, int dev,
			struct memif_circ_status *stat)
{
	struct link_device *ld = &shmd->ld;
	int cnt = 0;
	u32 qsize;
	u32 head;
	u32 tail;
	int space;

	while (1) {
		qsize = get_txq_buff_size(shmd, dev);
		head = get_txq_head(shmd, dev);
		tail = get_txq_tail(shmd, dev);
		space = circ_get_space(qsize, head, tail);

		mif_err("%s: %s_TXQ{qsize:%u in:%u out:%u space:%u}\n",
			ld->name, get_dev_name(dev), qsize, head, tail, space);

		if (circ_valid(qsize, head, tail))
			break;

		cnt++;
		mif_err("%s: ERR! invalid %s_TXQ{qsize:%d in:%d out:%d "
			"space:%d}, count %d\n",
			ld->name, get_dev_name(dev), qsize, head, tail,
			space, cnt);
		if (cnt >= MAX_RETRY_CNT) {
			space = -EIO;
			break;
		}

		udelay(100);
	}

	stat->buff = get_txq_buff(shmd, dev);
	stat->qsize = qsize;
	stat->in = head;
	stat->out = tail;
	stat->size = space;

	return space;
}

/**
 * write_ipc_to_txq
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 * @stat: pointer to an instance of memif_circ_status structure
 * @skb: pointer to an instance of sk_buff structure
 *
 * Must be invoked only when there is enough space in the TXQ.
 */
static void write_ipc_to_txq(struct shmem_link_device *shmd, int dev,
			struct memif_circ_status *stat, struct sk_buff *skb)
{
	struct link_device *ld = &shmd->ld;
	u8 __iomem *buff = stat->buff;
	u32 qsize = stat->qsize;
	u32 in = stat->in;
	u8 *src = skb->data;
	u32 len = skb->len;
	u32 inp;

	/* Write data to the TXQ */
	circ_write(buff, src, qsize, in, len);

	/* Update new head (in) pointer */
	inp = in + len;
	if (inp >= qsize)
		inp -= qsize;
	set_txq_head(shmd, dev, inp);

	/* Take a log for debugging */
	if (dev == IPC_FMT) {
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
		char tag[MIF_MAX_STR_LEN];
		snprintf(tag, MIF_MAX_STR_LEN, "%s: MIF2CP", ld->mc->name);
		pr_ipc(tag, src, (len > 20 ? 20 : len));
#endif
	}

#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	/* Verify data written to the TXQ */
	if (ld->aligned && memcmp16_to_io((buff + in), src, 4)) {
		mif_err("%s: memcmp16_to_io fail\n", ld->name);
		trigger_force_cp_crash(shmd);
	}
#endif
}

/**
 * xmit_ipc_msg
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * Tries to transmit IPC messages in the skb_txq of @dev as many as possible.
 *
 * Returns total length of IPC messages transmitted or an error code.
 */
static int xmit_ipc_msg(struct shmem_link_device *shmd, int dev)
{
	struct link_device *ld = &shmd->ld;
	struct sk_buff_head *txq = ld->skb_txq[dev];
	struct sk_buff *skb;
	unsigned long flags;
	struct memif_circ_status stat;
	int space;
	int copied = 0;

	/* Acquire the spin lock for a TXQ */
	spin_lock_irqsave(&shmd->tx_lock[dev], flags);

	while (1) {
		/* Get the size of free space in the TXQ */
		space = get_txq_space(shmd, dev, &stat);
		if (unlikely(space < 0)) {
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
			/* Trigger a enforced CP crash */
			trigger_force_cp_crash(shmd);
#endif
			/* Empty out the TXQ */
			reset_txq_circ(shmd, dev);
			copied = -EIO;
			break;
		}

		skb = skb_dequeue(txq);
		if (unlikely(!skb))
			break;

		/* Check the free space size comparing with skb->len */
		if (unlikely(space < skb->len)) {
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
			struct mem_status mst;
#endif
			/* Set res_required flag for the "dev" */
			atomic_set(&shmd->res_required[dev], 1);

			/* Take the skb back to the skb_txq */
			skb_queue_head(txq, skb);

			mif_err("%s: <called by %pf> NOSPC in %s_TXQ"
				"{qsize:%u in:%u out:%u}, free:%u < len:%u\n",
				ld->name, CALLER, get_dev_name(dev),
				stat.qsize, stat.in, stat.out, space, skb->len);
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
			get_shmem_status(shmd, TX, &mst);
			print_circ_status(shmd, dev, &mst);
#endif
			copied = -ENOSPC;
			break;
		}

		/* TX only when there is enough space in the TXQ */
		write_ipc_to_txq(shmd, dev, &stat, skb);
		copied += skb->len;
		dev_kfree_skb_any(skb);
	}

	/* Release the spin lock */
	spin_unlock_irqrestore(&shmd->tx_lock[dev], flags);

	return copied;
}

/**
 * wait_for_res_ack
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * 1) Sends an REQ_ACK interrupt for @dev to CP.
 * 2) Waits for the corresponding RES_ACK for @dev from CP.
 *
 * Returns the return value from wait_for_completion_interruptible_timeout().
 */
static int wait_for_res_ack(struct shmem_link_device *shmd, int dev)
{
	struct link_device *ld = &shmd->ld;
	struct completion *cmpl = &shmd->req_ack_cmpl[dev];
	unsigned long timeout = msecs_to_jiffies(RES_ACK_WAIT_TIMEOUT);
	int ret;
	u16 mask;

#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	mif_err("%s: send %s_REQ_ACK\n", ld->name, get_dev_name(dev));
#endif

	mask = get_mask_req_ack(shmd, dev);
	send_int2cp(shmd, INT_NON_CMD(mask));

	ret = wait_for_completion_interruptible_timeout(cmpl, timeout);
	/* ret == 0 on timeout, ret < 0 if interrupted */
	if (ret < 0) {
		mif_err("%s: %s: wait_for_completion interrupted! (ret %d)\n",
			ld->name, get_dev_name(dev), ret);
		goto exit;
	}

	if (ret == 0) {
		struct mem_status mst;
		get_shmem_status(shmd, TX, &mst);

		mif_err("%s: wait_for_completion TIMEOUT! (no %s_RES_ACK)\n",
			ld->name, get_dev_name(dev));

		/*
		** The TXQ must be checked whether or not it is empty, because
		** an interrupt mask can be overwritten by the next interrupt.
		*/
		if (mst.head[dev][TX] == mst.tail[dev][TX]) {
			ret = get_txq_buff_size(shmd, dev);
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
			mif_err("%s: %s_TXQ has been emptied\n",
				ld->name, get_dev_name(dev));
			print_circ_status(shmd, dev, &mst);
#endif
		}
	}

exit:
	return ret;
}

/**
 * process_res_ack
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * 1) Tries to transmit IPC messages in the skb_txq by invoking xmit_ipc_msg()
 *    function.
 * 2) Sends an interrupt to CP if there is no error from xmit_ipc_msg().
 * 3) Restarts SHM flow control if xmit_ipc_msg() returns -ENOSPC.
 *
 * Returns the return value from xmit_ipc_msg().
 */
static int process_res_ack(struct shmem_link_device *shmd, int dev)
{
	int ret;
	u16 mask;

	ret = xmit_ipc_msg(shmd, dev);
	if (ret > 0) {
		mask = get_mask_send(shmd, dev);
		send_int2cp(shmd, INT_NON_CMD(mask));
		get_shmem_status(shmd, TX, msq_get_free_slot(&shmd->stat_list));
	}

	if (ret >= 0)
		atomic_set(&shmd->res_required[dev], 0);

	return ret;
}

/**
 * fmt_tx_work: performs TX for FMT IPC device under SHM flow control
 * @work: pointer to an instance of the work_struct structure
 *
 * 1) Starts waiting for RES_ACK of FMT IPC device.
 * 2) Returns immediately if the wait is interrupted.
 * 3) Restarts SHM flow control if there is a timeout from the wait.
 * 4) Otherwise, it performs processing RES_ACK for FMT IPC device.
 */
static void fmt_tx_work(struct work_struct *work)
{
	struct link_device *ld;
	struct shmem_link_device *shmd;
	unsigned long delay = 0;
	int ret;

	ld = container_of(work, struct link_device, fmt_tx_dwork.work);
	shmd = to_shmem_link_device(ld);

	ret = wait_for_res_ack(shmd, IPC_FMT);
	/* ret < 0 if interrupted */
	if (ret < 0)
		return;

	/* ret == 0 on timeout */
	if (ret == 0) {
		queue_delayed_work(ld->tx_wq, ld->tx_dwork[IPC_FMT], 0);
		return;
	}

	ret = process_res_ack(shmd, IPC_FMT);
	if (ret >= 0) {
		allow_cp_sleep(shmd);
		return;
	}

	/* At this point, ret < 0 */
	if (ret == -ENOSPC)
		queue_delayed_work(ld->tx_wq, ld->tx_dwork[IPC_FMT], delay);
}

/**
 * raw_tx_work: performs TX for RAW IPC device under SHM flow control.
 * @work: pointer to an instance of the work_struct structure
 *
 * 1) Starts waiting for RES_ACK of RAW IPC device.
 * 2) Returns immediately if the wait is interrupted.
 * 3) Restarts SHM flow control if there is a timeout from the wait.
 * 4) Otherwise, it performs processing RES_ACK for RAW IPC device.
 */
static void raw_tx_work(struct work_struct *work)
{
	struct link_device *ld;
	struct shmem_link_device *shmd;
	unsigned long delay = 0;
	int ret;

	ld = container_of(work, struct link_device, raw_tx_dwork.work);
	shmd = to_shmem_link_device(ld);

	ret = wait_for_res_ack(shmd, IPC_RAW);
	/* ret < 0 if interrupted */
	if (ret < 0)
		return;

	/* ret == 0 on timeout */
	if (ret == 0) {
		queue_delayed_work(ld->tx_wq, ld->tx_dwork[IPC_RAW], 0);
		return;
	}

	ret = process_res_ack(shmd, IPC_RAW);
	if (ret >= 0) {
		allow_cp_sleep(shmd);
		mif_netif_wake(ld);
		return;
	}

	/* At this point, ret < 0 */
	if (ret == -ENOSPC)
		queue_delayed_work(ld->tx_wq, ld->tx_dwork[IPC_RAW], delay);
}

/**
 * rfs_tx_work: performs TX for RFS IPC device under SHM flow control
 * @work: pointer to an instance of the work_struct structure
 *
 * 1) Starts waiting for RES_ACK of RFS IPC device.
 * 2) Returns immediately if the wait is interrupted.
 * 3) Restarts SHM flow control if there is a timeout from the wait.
 * 4) Otherwise, it performs processing RES_ACK for RFS IPC device.
 */
static void rfs_tx_work(struct work_struct *work)
{
	struct link_device *ld;
	struct shmem_link_device *shmd;
	unsigned long delay = 0;
	int ret;

	ld = container_of(work, struct link_device, rfs_tx_dwork.work);
	shmd = to_shmem_link_device(ld);

	ret = wait_for_res_ack(shmd, IPC_RFS);
	/* ret < 0 if interrupted */
	if (ret < 0)
		return;

	/* ret == 0 on timeout */
	if (ret == 0) {
		queue_delayed_work(ld->tx_wq, ld->tx_dwork[IPC_RFS], 0);
		return;
	}

	ret = process_res_ack(shmd, IPC_RFS);
	if (ret >= 0) {
		allow_cp_sleep(shmd);
		return;
	}

	/* At this point, ret < 0 */
	if (ret == -ENOSPC)
		queue_delayed_work(ld->tx_wq, ld->tx_dwork[IPC_RFS], delay);
}

/**
 * shmem_send_ipc
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 *
 * 1) Tries to transmit IPC messages in the skb_txq by invoking xmit_ipc_msg()
 *    function.
 * 2) Sends an interrupt to CP if there is no error from xmit_ipc_msg().
 * 3) Starts SHM flow control if xmit_ipc_msg() returns -ENOSPC.
 */
static int shmem_send_ipc(struct shmem_link_device *shmd, int dev)
{
	struct link_device *ld = &shmd->ld;
	int ret;
	u16 mask;

	if (atomic_read(&shmd->res_required[dev]) > 0) {
		mif_err("%s: %s_TXQ is full\n", ld->name, get_dev_name(dev));
		return 0;
	}

	if (wake_cp_up(shmd) < 0) {
		trigger_force_cp_crash(shmd);
		return -EIO;
	}

	if (!ipc_active(shmd)) {
		mif_err("%s: IPC is NOT active\n", ld->name);
		ret = -EIO;
		goto exit;
	}

	ret = xmit_ipc_msg(shmd, dev);
	if (likely(ret > 0)) {
		mask = get_mask_send(shmd, dev);
		send_int2cp(shmd, INT_NON_CMD(mask));
		get_shmem_status(shmd, TX, msq_get_free_slot(&shmd->stat_list));
		goto exit;
	}

	/* If there was no TX, just exit */
	if (ret == 0)
		goto exit;

	/* At this point, ret < 0 */
	if (ret == -ENOSPC) {
		/* Prohibit CP from sleeping until the TXQ buffer is empty */
		if (wake_cp_up(shmd) < 0) {
			trigger_force_cp_crash(shmd);
			goto exit;
		}

		/*----------------------------------------------------*/
		/* shmd->res_required[dev] was set in xmit_ipc_msg(). */
		/*----------------------------------------------------*/

		if (dev == IPC_RAW)
			mif_netif_stop(ld);

		queue_delayed_work(ld->tx_wq, ld->tx_dwork[dev], 0);
	}

exit:
	allow_cp_sleep(shmd);
	return ret;
}

/**
 * shmem_try_send_ipc
 * @shmd: pointer to an instance of shmem_link_device structure
 * @dev: IPC device (IPC_FMT, IPC_RAW, etc.)
 * @iod: pointer to an instance of the io_device structure
 * @skb: pointer to an skb that will be transmitted
 *
 * 1) Enqueues an skb to the skb_txq for @dev in the link device instance.
 * 2) Tries to transmit IPC messages in the skb_txq by invoking xmit_ipc_msg()
 *    function.
 * 3) Sends an interrupt to CP if there is no error from xmit_ipc_msg().
 * 4) Starts SHM flow control if xmit_ipc_msg() returns -ENOSPC.
 */
static void shmem_try_send_ipc(struct shmem_link_device *shmd, int dev,
			struct io_device *iod, struct sk_buff *skb)
{
	struct link_device *ld = &shmd->ld;
	struct sk_buff_head *txq = ld->skb_txq[dev];
	int ret;

	if (unlikely(txq->qlen >= MAX_SKB_TXQ_DEPTH)) {
		mif_err("%s: %s txq->qlen %d >= %d\n", ld->name,
			get_dev_name(dev), txq->qlen, MAX_SKB_TXQ_DEPTH);
		dev_kfree_skb_any(skb);
		return;
	}

	skb_queue_tail(txq, skb);

	ret = shmem_send_ipc(shmd, dev);
	if (ret < 0) {
		mif_err("%s->%s: ERR! shmem_send_ipc fail (err %d)\n",
			iod->name, ld->name, ret);
	}
}

static int shmem_send_cp_binary(struct link_device *ld, struct sk_buff *skb)
{
	struct shmem_link_device *shmd = to_shmem_link_device(ld);
	return -ENODEV;
}

/**
 * shmem_send
 * @ld: pointer to an instance of the link_device structure
 * @iod: pointer to an instance of the io_device structure
 * @skb: pointer to an skb that will be transmitted
 *
 * Returns the length of data transmitted or an error code.
 *
 * Normal call flow for an IPC message:
 *   shmem_try_send_ipc -> shmem_send_ipc -> xmit_ipc_msg -> write_ipc_to_txq
 *
 * Call flow on congestion in a IPC TXQ:
 *   shmem_try_send_ipc -> xmit_ipc_msg ,,, queue_delayed_work
 *   => xxx_tx_work -> wait_for_res_ack
 *   => recv_ipc_msg
 *   => process_res_ack -> xmit_ipc_msg (,,, queue_delayed_work ...)
 */
static int shmem_send(struct link_device *ld, struct io_device *iod,
			struct sk_buff *skb)
{
	struct shmem_link_device *shmd = to_shmem_link_device(ld);
	int dev = iod->format;
	int len = skb->len;

	switch (dev) {
	case IPC_FMT:
	case IPC_RAW:
	case IPC_RFS:
		if (likely(ld->mode == LINK_MODE_IPC)) {
			shmem_try_send_ipc(shmd, dev, iod, skb);
		} else {
			mif_err("%s: ld->mode != LINK_MODE_IPC\n", ld->name);
			dev_kfree_skb_any(skb);
		}
		return len;

	case IPC_BOOT:
		return shmem_send_cp_binary(ld, skb);

	default:
		mif_err("%s: ERR! no TXQ for %s\n", ld->name, iod->name);
		dev_kfree_skb_any(skb);
		return -ENODEV;
	}
}

static int shmem_xmit_boot(struct link_device *ld, struct io_device *iod,
			unsigned long arg)
{
	struct shmem_link_device *shmd = to_shmem_link_device(ld);
	return -ENODEV;
}

static int shmem_set_dload_magic(struct link_device *ld, struct io_device *iod)
{
	struct shmem_link_device *shmd = to_shmem_link_device(ld);

	ld->mode = LINK_MODE_DLOAD;

	mif_err("%s: magic = 0x%08X\n", ld->name, DP_MAGIC_DMDL);
	iowrite32(DP_MAGIC_DMDL, shmd->dl_map.magic);

	return 0;
}

static int shmem_force_dump(struct link_device *ld, struct io_device *iod)
{
	struct shmem_link_device *shmd = to_shmem_link_device(ld);
	trigger_force_cp_crash(shmd);
	return 0;
}

static int shmem_dump_start(struct link_device *ld, struct io_device *iod)
{
	struct shmem_link_device *shmd = to_shmem_link_device(ld);
	return -ENODEV;
}

static int shmem_dump_update(struct link_device *ld, struct io_device *iod,
		unsigned long arg)
{
	struct shmem_link_device *shmd = to_shmem_link_device(ld);
	return -ENODEV;
}

static int shmem_dump_finish(struct link_device *ld, struct io_device *iod,
			unsigned long arg)
{
	struct shmem_link_device *shmd = to_shmem_link_device(ld);
	return -ENODEV;
}

static int shmem_init_dload_map(struct shmem_link_device *shmd)
{
	u8 __iomem *dp_base = shmd->base;
	u32 magic_size = DP_DLOAD_MAGIC_SIZE;
	u32 mbx_size = DP_MBX_SET_SIZE;

	shmd->dl_map.magic = (u32 *)(dp_base);
	shmd->dl_map.buff = (u8 *)(dp_base + magic_size);
	shmd->dl_map.space = shmd->size - (magic_size + mbx_size);

	return 0;
}

static int shmem_init_uload_map(struct shmem_link_device *shmd)
{
	u8 __iomem *dp_base = shmd->base;
	u32 magic_size = DP_ULOAD_MAGIC_SIZE;
	u32 mbx_size = DP_MBX_SET_SIZE;

	shmd->ul_map.magic = (u32 *)(dp_base);
	shmd->ul_map.buff = (u8 *)(dp_base + DP_ULOAD_BUFF_OFFSET);
	shmd->ul_map.space = shmd->size - (magic_size + mbx_size);

	return 0;
}

static void c2c_remap_4m_ipc_region(struct shmem_link_device *shmd)
{
	struct c2c_ipc_4m_map *map;
	struct shmem_ipc_device *dev;
	mif_err("+++\n");

	map = (struct c2c_ipc_4m_map *)shmd->base;

	/* Magic code and access enable fields */
	shmd->ipc_map.magic = (u16 __iomem *)&map->magic;
	shmd->ipc_map.access = (u16 __iomem *)&map->access;

	/* FMT */
	dev = &shmd->ipc_map.dev[IPC_FMT];

	strcpy(dev->name, "FMT");
	dev->id = IPC_FMT;

	dev->txq.head = (u32 __iomem *)&map->fmt_tx_head;
	dev->txq.tail = (u32 __iomem *)&map->fmt_tx_tail;
	dev->txq.buff = (u8 __iomem *)&map->fmt_tx_buff[0];
	dev->txq.size = C2C_4M_FMT_TX_BUFF_SZ;

	dev->rxq.head = (u32 __iomem *)&map->fmt_rx_head;
	dev->rxq.tail = (u32 __iomem *)&map->fmt_rx_tail;
	dev->rxq.buff = (u8 __iomem *)&map->fmt_rx_buff[0];
	dev->rxq.size = C2C_4M_FMT_RX_BUFF_SZ;

	dev->mask_req_ack = INT_MASK_REQ_ACK_F;
	dev->mask_res_ack = INT_MASK_RES_ACK_F;
	dev->mask_send    = INT_MASK_SEND_F;

	/* RAW */
	dev = &shmd->ipc_map.dev[IPC_RAW];

	strcpy(dev->name, "RAW");
	dev->id = IPC_RAW;

	dev->txq.head = (u32 __iomem *)&map->raw_tx_head;
	dev->txq.tail = (u32 __iomem *)&map->raw_tx_tail;
	dev->txq.buff = (u8 __iomem *)&map->raw_tx_buff[0];
	dev->txq.size = C2C_4M_RAW_TX_BUFF_SZ;

	dev->rxq.head = (u32 __iomem *)&map->raw_rx_head;
	dev->rxq.tail = (u32 __iomem *)&map->raw_rx_tail;
	dev->rxq.buff = (u8 __iomem *)&map->raw_rx_buff[0];
	dev->rxq.size = C2C_4M_RAW_RX_BUFF_SZ;

	dev->mask_req_ack = INT_MASK_REQ_ACK_R;
	dev->mask_res_ack = INT_MASK_RES_ACK_R;
	dev->mask_send    = INT_MASK_SEND_R;

	/* interrupt ports */
	shmd->ipc_map.mbx2ap = NULL;
	shmd->ipc_map.mbx2cp = NULL;

	mif_err("---\n");
}

static void shmem_remap_4m_ipc_region(struct shmem_link_device *shmd)
{
	struct shmem_ipc_4m_map *map;
	struct shmem_ipc_device *dev;
	mif_err("+++\n");

	map = (struct shmem_ipc_4m_map *)shmd->base;

	/* "magic code" and "access enable" fields */
	shmd->ipc_map.magic = (u16 __iomem *)&map->magic;
	shmd->ipc_map.access = (u16 __iomem *)&map->access;

	/* FMT */
	dev = &shmd->ipc_map.dev[IPC_FMT];

	strcpy(dev->name, "FMT");
	dev->id = IPC_FMT;

	dev->txq.head = (u32 __iomem *)&map->fmt_tx_head;
	dev->txq.tail = (u32 __iomem *)&map->fmt_tx_tail;
	dev->txq.buff = (u8 __iomem *)&map->fmt_tx_buff[0];
	dev->txq.size = SHM_4M_FMT_TX_BUFF_SZ;

	dev->rxq.head = (u32 __iomem *)&map->fmt_rx_head;
	dev->rxq.tail = (u32 __iomem *)&map->fmt_rx_tail;
	dev->rxq.buff = (u8 __iomem *)&map->fmt_rx_buff[0];
	dev->rxq.size = SHM_4M_FMT_RX_BUFF_SZ;

	dev->mask_req_ack = INT_MASK_REQ_ACK_F;
	dev->mask_res_ack = INT_MASK_RES_ACK_F;
	dev->mask_send    = INT_MASK_SEND_F;

	/* RAW */
	dev = &shmd->ipc_map.dev[IPC_RAW];

	strcpy(dev->name, "RAW");
	dev->id = IPC_RAW;

	dev->txq.head = (u32 __iomem *)&map->raw_tx_head;
	dev->txq.tail = (u32 __iomem *)&map->raw_tx_tail;
	dev->txq.buff = (u8 __iomem *)&map->raw_tx_buff[0];
	dev->txq.size = SHM_4M_RAW_TX_BUFF_SZ;

	dev->rxq.head = (u32 __iomem *)&map->raw_rx_head;
	dev->rxq.tail = (u32 __iomem *)&map->raw_rx_tail;
	dev->rxq.buff = (u8 __iomem *)&map->raw_rx_buff[0];
	dev->rxq.size = SHM_4M_RAW_RX_BUFF_SZ;

	dev->mask_req_ack = INT_MASK_REQ_ACK_R;
	dev->mask_res_ack = INT_MASK_RES_ACK_R;
	dev->mask_send    = INT_MASK_SEND_R;

	/* interrupt ports */
	shmd->ipc_map.mbx2ap = (u16 __iomem *)&map->mbx2ap;
	shmd->ipc_map.mbx2cp = (u16 __iomem *)&map->mbx2cp;

	mif_err("---\n");
}

static int shmem_init_ipc_map(struct shmem_link_device *shmd)
{
	int i;
	struct link_device *ld = &shmd->ld;

	if (shmd->size >= SHMEM_SIZE_4MB)
		c2c_remap_4m_ipc_region(shmd);
	else
		return -EINVAL;

	shmd->magic = shmd->ipc_map.magic;
	shmd->access = shmd->ipc_map.access;
	for (i = 0; i < ld->max_ipc_dev; i++)
		shmd->dev[i] = &shmd->ipc_map.dev[i];
	shmd->mbx2ap = shmd->ipc_map.mbx2ap;
	shmd->mbx2cp = shmd->ipc_map.mbx2cp;

	return 0;
}

static int shmem_link_init(struct link_device *ld, struct io_device *iod)
{
	return 0;
}

static void shmem_link_terminate(struct link_device *ld, struct io_device *iod)
{
	if (iod->format == IPC_FMT && ld->mode == LINK_MODE_IPC) {
		if (!atomic_read(&iod->opened)) {
			ld->mode = LINK_MODE_OFFLINE;
			mif_err("%s: %s: link mode is changed: IPC->OFFLINE\n",
				iod->name, ld->name);
		}
	}

	return;
}

struct link_device *c2c_create_link_device(struct platform_device *pdev)
{
	struct shmem_link_device *shmd = NULL;
	struct link_device *ld = NULL;
	struct modem_data *modem = NULL;
	struct resource *res = NULL;
	int err = 0;
	int i = 0;
	unsigned long irq_flags;
	char irq_name[MIF_MAX_NAME_LEN];
	mif_err("+++\n");

	/*
	** Get the modem (platform) data
	*/
	modem = (struct modem_data *)pdev->dev.platform_data;
	if (!modem) {
		mif_err("ERR! modem == NULL\n");
		goto error;
	}
	mif_err("%s: modem = %s\n", modem->link_name, modem->name);

	/*
	** Alloc an instance of shmem_link_device structure
	*/
	shmd = kzalloc(sizeof(struct shmem_link_device), GFP_KERNEL);
	if (!shmd) {
		mif_err("%s: ERR! shmd kzalloc fail\n", modem->link_name);
		goto error;
	}
	ld = &shmd->ld;

	/*
	** Retrieve modem data and SHM control data from the modem data
	*/
	ld->mdm_data = modem;
	ld->name = modem->link_name;
	ld->aligned = 1;
	ld->ipc_version = modem->ipc_version;
	if (ld->ipc_version < SIPC_VER_50) {
		if (!modem->max_ipc_dev) {
			mif_err("%s: ERR! no max_ipc_dev\n", ld->name);
			goto error;
		}
		ld->max_ipc_dev = MAX_IPC_DEV;
	} else {
		ld->max_ipc_dev = MAX_SIPC5_DEV;
	}

	/*
	** Set attributes as a link device
	*/
	ld->init_comm = shmem_link_init;
	ld->terminate_comm = shmem_link_terminate;
	ld->send = shmem_send;
	ld->xmit_boot = shmem_xmit_boot;
	ld->dload_start = shmem_set_dload_magic;
	ld->force_dump = shmem_force_dump;
	ld->dump_start = shmem_dump_start;
	ld->dump_update = shmem_dump_update;
	ld->dump_finish = shmem_dump_finish;

	INIT_LIST_HEAD(&ld->list);

	skb_queue_head_init(&ld->sk_fmt_tx_q);
	skb_queue_head_init(&ld->sk_raw_tx_q);
	skb_queue_head_init(&ld->sk_rfs_tx_q);
	ld->skb_txq[IPC_FMT] = &ld->sk_fmt_tx_q;
	ld->skb_txq[IPC_RAW] = &ld->sk_raw_tx_q;
	ld->skb_txq[IPC_RFS] = &ld->sk_rfs_tx_q;

	skb_queue_head_init(&ld->sk_fmt_rx_q);
	skb_queue_head_init(&ld->sk_raw_rx_q);
	skb_queue_head_init(&ld->sk_rfs_rx_q);
	ld->skb_rxq[IPC_FMT] = &ld->sk_fmt_rx_q;
	ld->skb_rxq[IPC_RAW] = &ld->sk_raw_rx_q;
	ld->skb_rxq[IPC_RFS] = &ld->sk_rfs_rx_q;

	init_completion(&ld->init_cmpl);
	init_completion(&ld->pif_cmpl);

	/*
	** Retrieve SHM resource
	*/
	if (modem->link_types & LINKTYPE(LINKDEV_C2C))
		shmd->type = C2C_SHMEM;
	else
		shmd->type = REAL_SHMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		mif_err("%s: ERR! no SHMEM resource\n", ld->name);
		goto error;
	}

	shmd->start = res->start;
	shmd->size = resource_size(res);
	shmd->base = c2c_request_sh_region(shmd->start, shmd->size);
	if (!shmd->base) {
		mif_err("%s: ERR! c2c_request_sh_region fail\n", ld->name);
		goto error;
	}

	mif_err("%s: phys_addr 0x%08X, virt_addr 0x%08X, size %d\n",
		ld->name, (int)shmd->start, (int)shmd->base, shmd->size);

	/*
	** Initialize SHM maps (physical map -> logical map)
	*/
	err = shmem_init_dload_map(shmd);
	if (err < 0) {
		mif_err("%s: ERR! shmem_init_dload_map fail (err %d)\n",
			ld->name, err);
		goto error;
	}

	err = shmem_init_uload_map(shmd);
	if (err < 0) {
		mif_err("%s: ERR! shmem_init_uload_map fail (err %d)\n",
			ld->name, err);
		goto error;
	}

	err = shmem_init_ipc_map(shmd);
	if (err < 0) {
		mif_err("%s: ERR! shmem_init_ipc_map fail (err %d)\n",
			ld->name, err);
		goto error;
	}

	/*
	** Initialize locks, completions, and bottom halves
	*/
	snprintf(shmd->wlock_name, MIF_MAX_NAME_LEN, "%s_wlock", ld->name);
	wake_lock_init(&shmd->wlock, WAKE_LOCK_SUSPEND, shmd->wlock_name);

	init_completion(&shmd->udl_cmpl);
	init_completion(&shmd->crash_cmpl);

	for (i = 0; i < ld->max_ipc_dev; i++)
		init_completion(&shmd->req_ack_cmpl[i]);

	tasklet_init(&shmd->rx_tsk, ipc_rx_task, (unsigned long)shmd);

	for (i = 0; i < ld->max_ipc_dev; i++) {
		spin_lock_init(&shmd->tx_lock[i]);
		atomic_set(&shmd->res_required[i], 0);
	}

	ld->tx_wq = create_singlethread_workqueue("shmem_tx_wq");
	if (!ld->tx_wq) {
		mif_err("%s: ERR! fail to create tx_wq\n", ld->name);
		goto error;
	}
	INIT_DELAYED_WORK(&ld->fmt_tx_dwork, fmt_tx_work);
	INIT_DELAYED_WORK(&ld->raw_tx_dwork, raw_tx_work);
	INIT_DELAYED_WORK(&ld->rfs_tx_dwork, rfs_tx_work);
	ld->tx_dwork[IPC_FMT] = &ld->fmt_tx_dwork;
	ld->tx_dwork[IPC_RAW] = &ld->raw_tx_dwork;
	ld->tx_dwork[IPC_RFS] = &ld->rfs_tx_dwork;

#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	INIT_DELAYED_WORK(&shmd->dump_dwork, save_shmem_dump_work);
	INIT_DELAYED_WORK(&shmd->trace_dwork, save_ipc_trace_work);
	spin_lock_init(&shmd->stat_list.lock);
	spin_lock_init(&shmd->dump_list.lock);
	spin_lock_init(&shmd->trace_list.lock);
#endif

	/* Prepare a multi-purpose miscellaneous buffer */
	shmd->buff = kzalloc(C2C_SH_RGN_SIZE, GFP_KERNEL);
	if (!shmd->buff) {
		mif_err("%s: ERR! kzalloc shmd->buff fail\n", ld->name);
		goto error;
	}

	/*
	** Retrieve SHM IRQ GPIO#, IRQ#, and IRQ flags
	*/
	shmd->gpio_ap_wakeup = modem->gpio_ap_wakeup;
	shmd->irq_ap_wakeup = modem->irq_ap_wakeup;
	shmd->gpio_ap_status = modem->gpio_ap_status;

	shmd->gpio_cp_wakeup = modem->gpio_cp_wakeup;
	shmd->gpio_cp_status = modem->gpio_cp_status;

	/*
	** Register interrupt handlers
	*/
	err = c2c_register_handler(c2c_irq_handler, shmd);
	if (err) {
		mif_err("%s: ERR! c2c_register_handler fail (err %d)\n",
			ld->name, err);
		goto error;
	}

	snprintf(irq_name, MIF_MAX_NAME_LEN, "%s_ap_wakeup_irq", ld->name);
	irq_flags = (IRQF_NO_SUSPEND | IRQF_TRIGGER_RISING);
	err = register_isr(shmd->irq_ap_wakeup, ap_wakeup_irq_handler,
			irq_flags, irq_name, shmd);
	if (err) {
		mif_err("%s: register_isr fail (err %d)\n", ld->name, err);
		goto error;
	}

	mif_err("---\n");
	return ld;

error:
	if (shmd) {
		if (shmd->buff)
			kfree(shmd->buff);
		kfree(shmd);
	}

	mif_err("xxx\n");
	return NULL;
}

