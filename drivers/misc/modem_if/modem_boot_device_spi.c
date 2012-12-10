/*
 * Copyright (C) 2011 Samsung Electronics.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>

#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/wakelock.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>

#include <linux/platform_data/modem.h>
#include "modem_prj.h"

#define SPI_XMIT_DELEY	100

#ifdef AIRPLANE_MODE_TEST
int lte_airplane_mode;
#endif

static inline int spi_boot_xmit(struct modem_spi_boot *loader, const char val)
{
	char buff[1];
	int ret;
	struct spi_message msg;
	struct spi_transfer xfer = {
		.len = 1,
		.tx_buf = buff,
	};

	buff[0] = val;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	ret = spi_sync(loader->spi_dev, &msg);
	if (ret < 0)
		mif_err("ERR! spi_sync fail (err %d)\n", ret);

	return ret;
}

static int spi_boot_write(struct modem_spi_boot *loader, const char *addr,
			const int len)
{
	int i;
	int ret = 0;
	char *buff = NULL;
	unsigned char lenbuf[4];
	mif_err("+++\n");

	buff = kzalloc(len, GFP_KERNEL);
	if (!buff) {
		mif_err("ERR! kzalloc(%d) fail\n", len);
		ret = -ENOMEM;
		goto exit;
	}

	if (loader->xmit_status == XMIT_LOADER_READY) {
		memcpy(lenbuf, &len, ARRAY_SIZE(lenbuf));
		for (i = 0 ; i < ARRAY_SIZE(lenbuf); i++) {
			ret = spi_boot_xmit(loader, lenbuf[i]);
			if (ret < 0) {
				mif_err("ERR! spi_boot_xmit fail (err %d)\n",
					ret);
				goto exit;
			}
		}
		msleep(SPI_XMIT_DELEY);
	}

	ret = copy_from_user(buff, (const void __user *)addr, len);
	if (ret) {
		mif_err("ERR! copy_from_user fail (err %d)\n", ret);
		ret = -EFAULT;
		goto exit;
	}

	for (i = 0 ; i < len ; i++) {
		ret = spi_boot_xmit(loader, buff[i]);
		if (ret < 0) {
			mif_err("ERR! spi_boot_xmit fail (err %d)\n", ret);
			goto exit;
		}
	}

exit:
	if (buff)
		kfree(buff);

	mif_err("---\n");
	return ret;
}

static int spi_boot_open(struct inode *inode, struct file *filp)
{
	struct modem_spi_boot *loader = to_modem_spi_boot(filp->private_data);
	filp->private_data = loader;
	return 0;
}

static long spi_boot_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg)
{
	int ret = 0;
	struct modem_firmware param;
	struct modem_spi_boot *loader = filp->private_data;

	mutex_lock(&loader->lock);
	switch (cmd) {
	case IOCTL_MODEM_XMIT_BOOT:
		ret = copy_from_user(&param, (const void __user *)arg,
					sizeof(struct modem_firmware));
		if (ret) {
			mif_err("ERR! copy_from_user fail (err %d)\n", ret);
			ret = -EFAULT;
			goto exit_err;
		}
		mif_err("IOCTL_MODEM_XMIT_BOOT (addr 0x%08X, size %d)\n",
			(int)param.binary, param.size);

		ret = spi_boot_write(loader, param.binary, param.size);
		if (ret < 0) {
			mif_err("ERR! spi_boot_write fail (err %d)\n", ret);
		} else {
			if (loader->xmit_status == XMIT_BOOT_READY)
				loader->xmit_status = XMIT_LOADER_READY;
			else
				loader->xmit_status = XMIT_BOOT_READY;
		}

		break;

#ifdef AIRPLANE_MODE_TEST
	case IOCTL_LTE_MODEM_AIRPLAIN_ON:
		lte_airplane_mode = 1;
		mif_info("IOCTL_LTE_MODEM LPM_ON\n");
		break;

	case IOCTL_LTE_MODEM_AIRPLAIN_OFF:
		mif_info("IOCTL_LTE_MODEM LPM_OFF\n");
		lte_airplane_mode = 0;
		break;
#endif

	default:
		mif_err("ioctl cmd error\n");
		ret = -ENOIOCTLCMD;

		break;
	}
	mutex_unlock(&loader->lock);

exit_err:
	return ret;
}

static const struct file_operations modem_spi_boot_fops = {
	.owner = THIS_MODULE,
	.open = spi_boot_open,
	.unlocked_ioctl = spi_boot_ioctl,
};

static int __devinit modem_spi_boot_probe(struct spi_device *spi)
{
	int ret;
	struct modem_spi_boot *loader;
	struct modem_spi_boot_platform_data *pdata;
	mif_err("+++\n");

	loader = kzalloc(sizeof(*loader), GFP_KERNEL);
	if (!loader) {
		mif_err("failed to allocate for modem_spi_boot\n");
		ret = -ENOMEM;
		goto err_alloc;
	}
	mutex_init(&loader->lock);

	spi->bits_per_word = 8;

	if (spi_setup(spi)) {
		mif_err("ERR! spi_setup fail\n");
		ret = -EINVAL;
		goto err_setup;
	}
	loader->spi_dev = spi;

	if (!spi->dev.platform_data) {
		mif_err("ERR! no platform_data\n");
		ret = -EINVAL;
		goto err_setup;
	}
	pdata = (struct modem_spi_boot_platform_data *)spi->dev.platform_data;

	loader->gpio_cp2ap_status = pdata->gpio_cp2ap_status;
	loader->xmit_status = XMIT_BOOT_READY;

	spi_set_drvdata(spi, loader);

	loader->dev.minor = MISC_DYNAMIC_MINOR;
	loader->dev.name = "lte_spi";
	loader->dev.fops = &modem_spi_boot_fops;
	ret = misc_register(&loader->dev);
	if (ret) {
		mif_err("ERR! misc_register fail (err %d)\n", ret);
		goto err_setup;
	}

#ifdef AIRPLANE_MODE_TEST
	lte_airplane_mode = 0;
#endif
	mif_err("---\n");
	return 0;

err_setup:
	mutex_destroy(&loader->lock);
	kfree(loader);

err_alloc:
	mif_err("xxx\n");
	return ret;
}

static int __devexit modem_spi_boot_remove(struct spi_device *spi)
{
	struct modem_spi_boot *loader = spi_get_drvdata(spi);

	misc_deregister(&loader->dev);
	mutex_destroy(&loader->lock);
	kfree(loader);

	return 0;
}

static struct spi_driver modem_spi_boot_driver = {
	.driver = {
		.name = "modem_spi_boot",
		.owner = THIS_MODULE,
	},
	.probe = modem_spi_boot_probe,
	.remove = __devexit_p(modem_spi_boot_remove),
};

static int __init modem_spi_boot_init(void)
{
	int err;
	mif_err("+++\n");

	err = spi_register_driver(&modem_spi_boot_driver);
	if (err) {
		mif_err("spi_register_driver fail (err %d)\n", err);
		mif_err("xxx\n");
		return err;
	}

	mif_err("---\n");
	return 0;
}

static void __exit modem_spi_boot_exit(void)
{
	spi_unregister_driver(&modem_spi_boot_driver);
}

module_init(modem_spi_boot_init);
module_exit(modem_spi_boot_exit);

MODULE_DESCRIPTION("LTE Modem Bootloader driver");
MODULE_LICENSE("GPL");

