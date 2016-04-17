/*
 * PCI glue for ISHTP provider device (ISH) driver
 *
 * Copyright (c) 2014-2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/miscdevice.h>
#include "ishtp_dev.h"
#include "hw-ish.h"

/*
 * ishtp driver strings
 */
static bool nomsi;
module_param_named(nomsi, nomsi, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(nomsi, "don't use msi (default = false)");

/* Currently this driver works as long as there is only a single ISHTP device */
struct pci_dev *ishtp_pci_device;

static const struct pci_device_id ish_pci_tbl[] = {
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x22D8)},
	{0, }
};
MODULE_DEVICE_TABLE(pci, ish_pci_tbl);

static DEFINE_MUTEX(ishtp_mutex);
struct init_work_t {
	struct work_struct init_work;
	struct ishtp_device *dev;
};
struct init_work_t *pci_init_work;
struct workqueue_struct *workqueue_for_init;

/* global variables for suspend */
int	suspend_flag = 0;
wait_queue_head_t	suspend_wait;

#if ISH_DEBUGGER
/*
 * view or modify registers content from user space
 */
struct ish_hw *hw_dbg;
static char	dbg_resp_buf[2048];
static int	resp_buf_read;

static int ishdbg_open(struct inode *inode, struct file *file)
{
	return	0;
}

static int ishdbg_release(struct inode *inode, struct file *file)
{
	return	0;
}

static ssize_t ishdbg_read(struct file *file, char __user *ubuf, size_t length,
	loff_t *offset)
{
	int rv;
	int copy_len;

	if (resp_buf_read)
		return	0;	/* EOF */
	copy_len = (length > strlen(dbg_resp_buf)) ?
		strlen(dbg_resp_buf) : length;
	rv = copy_to_user(ubuf, dbg_resp_buf, copy_len);
	if (rv)
		return -EINVAL;
	resp_buf_read = 1;
	return copy_len;
}

static ssize_t ishdbg_write(struct file *file, const char __user *ubuf,
	size_t length, loff_t *offset)
{
	char	dbg_req_buf[400];
	char	cmd[400];
	int	rv;
	unsigned	addr, count;
	int	sscanf_match, i, cur_index;
	uint32_t __iomem *reg_data;

	if (length > sizeof(dbg_req_buf))
		length = sizeof(dbg_req_buf);
	rv = copy_from_user(dbg_req_buf, ubuf, length);
	if (rv)
		return -EINVAL;
	if (sscanf(dbg_req_buf, "%s ", cmd) != 1) {
		dev_err(&ishtp_pci_device->dev,
			"[ish-dbg] getting request failed\n");
		return -EINVAL;
	}
	sscanf_match = sscanf(dbg_req_buf + 2, "%x %u", &addr, &count);

	if (addr >= IPC_REG_MAX) {
		dev_err(&ishtp_pci_device->dev,
			"[ish-dbg] address %08X out of range\n", addr);
		return	-EINVAL;
	}

	if (!strcmp(cmd, "d")) {
		/* Dump values: d <addr> [count] */
		if (sscanf_match == 1)
			count = 1;
		else if (sscanf_match != 2) {
			dev_err(&ishtp_pci_device->dev,
				"[ish-dbg] bad parameters number (%d)\n",
				sscanf_match);
			return -EINVAL;
		}
		if (addr % 4) {
			dev_err(&ishtp_pci_device->dev,
				"[ish-dbg] address isn't aligned to 4 bytes\n");
			return -EINVAL;
		}
		cur_index = 0;
		for (i = 0; i < count; i++) {
			reg_data = (uint32_t __iomem *)
				((char *)hw_dbg->mem_addr + addr + i*4);
			cur_index += scnprintf(dbg_resp_buf + cur_index,
				sizeof(dbg_resp_buf) - cur_index, "%08X ",
				readl(reg_data));
		}
		cur_index += scnprintf(dbg_resp_buf + cur_index,
			sizeof(dbg_resp_buf) - cur_index, "\n");
		resp_buf_read = 0;
	} else if (!strcmp(cmd, "e")) {
		/* Enter values e <addr> <value> */
		if (sscanf_match != 2) {
			dev_err(&ishtp_pci_device->dev,
				"[ish-dbg] bad parameters number (%d)\n",
				sscanf_match);
			return -EINVAL;
		}
		if (addr % 4) {
			dev_err(&ishtp_pci_device->dev,
				"[ish-dbg] address isn't aligned to 4 bytes\n");
			return -EINVAL;
		}
		reg_data = (uint32_t __iomem *)((char *)hw_dbg->mem_addr
			+ addr);
		writel(count, reg_data);
		scnprintf(dbg_resp_buf, sizeof(dbg_resp_buf), "OK\n");
		resp_buf_read = 0;
	}
	return length;
}

static long ishdbg_ioctl(struct file *file, unsigned int cmd,
	unsigned long data)
{
	return	0;
}

/*
 * file operations structure will be used for ishtp char device.
 */
static const struct file_operations ishdbg_fops = {
	.owner = THIS_MODULE,
	.read = ishdbg_read,
	.unlocked_ioctl = ishdbg_ioctl,
	.open = ishdbg_open,
	.release = ishdbg_release,
	.write = ishdbg_write,
	.llseek = no_llseek
};

/*
 * Misc Device Struct
 */
static struct miscdevice ishdbg_misc_device = {
		.name = "ishdbg",
		.fops = &ishdbg_fops,
		.minor = MISC_DYNAMIC_MINOR,
};

#endif /* ISH_DEBUGGER */

#if ISH_LOG
/*
 * ish internal log, without impact on performance
 */
void delete_from_log(struct ishtp_device *dev, size_t min_chars)
{
	int i;
	/* set log_tail to point at the last char to be deleted */
	dev->log_tail = (dev->log_tail + min_chars - 1) % PRINT_BUFFER_SIZE;
	for (i = dev->log_tail; dev->log_buffer[i] != '\n';
			i = (i+1) % PRINT_BUFFER_SIZE)
		;
	dev->log_tail = (i+1) % PRINT_BUFFER_SIZE;
}

static void ish_print_log(struct ishtp_device *dev, char *format, ...)
{
	char tmp_buf[900];
	va_list args;
	int length, i, full_space, free_space;
	unsigned long	flags;
	struct timeval tv;

	/* Fix for power-off path */
	if (!ishtp_pci_device)
		return;

	do_gettimeofday(&tv);
	i = scnprintf(tmp_buf, sizeof(tmp_buf), "[%ld.%06ld] ",
		tv.tv_sec, tv.tv_usec);

	va_start(args, format);
	length = vsnprintf(tmp_buf + i, sizeof(tmp_buf)-i, format, args);
	va_end(args);

	length = length + i;
	/* if the msg does not end with \n, add it */
	if (tmp_buf[length-1] != '\n') {
		tmp_buf[length] = '\n';
		length++;
	}

	spin_lock_irqsave(&dev->log_spinlock, flags);

	full_space = dev->log_head - dev->log_tail;
	if (full_space < 0)
		full_space = PRINT_BUFFER_SIZE + full_space;
	free_space = PRINT_BUFFER_SIZE - full_space;

	if (free_space <= length)
		/*
		 * not enougth space. needed at least 1 empty char to recognize
		 * whether buffer is full or empty
		 */
		delete_from_log(dev, (length - free_space) + 1);

	if (dev->log_head + length <= PRINT_BUFFER_SIZE) {
		memcpy(dev->log_buffer + dev->log_head, tmp_buf, length);
	} else {
		memcpy(dev->log_buffer + dev->log_head, tmp_buf,
			PRINT_BUFFER_SIZE - dev->log_head);
		memcpy(dev->log_buffer,
			tmp_buf + PRINT_BUFFER_SIZE - dev->log_head,
			length - (PRINT_BUFFER_SIZE - dev->log_head));
	}
	dev->log_head = (dev->log_head + length) % PRINT_BUFFER_SIZE;

	spin_unlock_irqrestore(&dev->log_spinlock, flags);
}

/* print to log without need to send ishtp_dev */
void	g_ish_print_log(char *fmt, ...)
{
	char tmp_buf[900];
	va_list args;
	struct ishtp_device	*dev;

	/* Fix for power-off path */
	if (!ishtp_pci_device)
		return;

	dev = pci_get_drvdata(ishtp_pci_device);
	va_start(args, fmt);
	vsnprintf(tmp_buf, sizeof(tmp_buf), fmt, args);
	va_end(args);
	ish_print_log(dev, tmp_buf);
}
EXPORT_SYMBOL(g_ish_print_log);

static ssize_t ish_read_log(struct ishtp_device *dev, char *buf, size_t size)
{
	int i, full_space, ret_val;

	if (dev->log_head == dev->log_tail) /* log is empty */
		return 0;

	/* read size the minimum between full_space and the buffer size */
	full_space = dev->log_head - dev->log_tail;
	if (full_space < 0)
		full_space = PRINT_BUFFER_SIZE + full_space;

	if (full_space < size)
		i = (dev->log_tail + full_space) % PRINT_BUFFER_SIZE;
		/* log has less than 'size' bytes, i = dev->log_head */
	else
		i = (dev->log_tail + size) % PRINT_BUFFER_SIZE;

	/* i is the last character to be readen */
	i = (i-1) % PRINT_BUFFER_SIZE;

	/* read from tail to last '\n' before i */
	for (; dev->log_buffer[i] != '\n' && i != dev->log_tail;
			i = (i-1) % PRINT_BUFFER_SIZE)
		;
	if (i == dev->log_tail) /* buffer is not big enough, no '\n' found */
		return 0;

	if (dev->log_tail < i) {
		memcpy(buf, dev->log_buffer + dev->log_tail,
			i - dev->log_tail + 1);
		ret_val = i - dev->log_tail + 1;
	} else {
		memcpy(buf, dev->log_buffer + dev->log_tail,
			PRINT_BUFFER_SIZE - dev->log_tail);
		memcpy(buf + PRINT_BUFFER_SIZE - dev->log_tail,
			dev->log_buffer, i + 1);
		ret_val = PRINT_BUFFER_SIZE - dev->log_tail + i + 1;
	}
	return ret_val;
}

static ssize_t ish_read_flush_log(struct ishtp_device *dev, char *buf,
	size_t size)
{
	int ret;

	ret = ish_read_log(dev, buf, size);
	delete_from_log(dev, ret);
	return ret;
}

/* show & store functions for both read and flush char devices*/
ssize_t show_read(struct device *dev, struct device_attribute *dev_attr,
	char *buf)
{
	struct ishtp_device *ishtp_dev;
	ssize_t ret;
	unsigned long	flags;

	ishtp_dev = dev_get_drvdata(dev);
	spin_lock_irqsave(&ishtp_dev->log_spinlock, flags);
	ret = ish_read_log(ishtp_dev, buf, PAGE_SIZE);
	spin_unlock_irqrestore(&ishtp_dev->log_spinlock, flags);

	return ret;
}

ssize_t store_read(struct device *dev, struct device_attribute *dev_attr,
	const char *buf, size_t count)
{
	return count;
}

static struct device_attribute read_attr = {
	.attr = {
		.name = "ish_read_log",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = show_read,
	.store = store_read
};

ssize_t show_flush(struct device *dev, struct device_attribute *dev_attr,
	char *buf)
{
	struct ishtp_device *ishtp_dev;
	unsigned long	flags;
	ssize_t ret;

	ishtp_dev = dev_get_drvdata(dev);
	spin_lock_irqsave(&ishtp_dev->log_spinlock, flags);
	ret = ish_read_flush_log(ishtp_dev, buf, PAGE_SIZE);
	spin_unlock_irqrestore(&ishtp_dev->log_spinlock, flags);

	return ret;
}

ssize_t store_flush(struct device *dev, struct device_attribute *dev_attr,
	const char *buf, size_t count)
{
	struct ishtp_device *ishtp_dev;
	unsigned long	flags;

	ishtp_dev = dev_get_drvdata(dev);

	/* empty the log */
	if (!strncmp(buf, "empty", 5)) {
		spin_lock_irqsave(&ishtp_dev->log_spinlock, flags);
		ishtp_dev->log_tail = ishtp_dev->log_head;
		spin_unlock_irqrestore(&ishtp_dev->log_spinlock, flags);
	}
	return count;
}

static struct device_attribute flush_attr = {
	.attr = {
		.name = "ish_flush_log",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = show_flush,
	.store = store_flush
};
#else

static void ish_print_log_nolog(struct ishtp_device *dev, char *format, ...)
{
}

void	g_ish_print_log(char *fmt, ...)
{
}
EXPORT_SYMBOL(g_ish_print_log);

#endif /* ISH_LOG */

ssize_t show_ishtp_dev_props(struct device *dev,
	struct device_attribute *dev_attr, char *buf)
{
	struct ishtp_device *ishtp_dev;
	ssize_t	ret = -ENOENT;
	unsigned	count;
	unsigned long	flags;

	ishtp_dev = dev_get_drvdata(dev);

	if (!strcmp(dev_attr->attr.name, "ishtp_dev_state")) {
		scnprintf(buf, PAGE_SIZE, "%u\n",
			(unsigned)ishtp_dev->dev_state);
		ret = strlen(buf);
	} else if (!strcmp(dev_attr->attr.name, "hbm_state")) {
		scnprintf(buf, PAGE_SIZE, "%u\n",
			(unsigned)ishtp_dev->hbm_state);
		ret = strlen(buf);
	} else if (!strcmp(dev_attr->attr.name, "fw_status")) {
		scnprintf(buf, PAGE_SIZE, "%08X\n",
			ishtp_dev->ops->get_fw_status(ishtp_dev));
		ret = strlen(buf);
	} else if (!strcmp(dev_attr->attr.name, "ipc_buf")) {
		struct wr_msg_ctl_info *ipc_link, *ipc_link_next;

		count = 0;
		spin_lock_irqsave(&ishtp_dev->wr_processing_spinlock, flags);
		list_for_each_entry_safe(ipc_link, ipc_link_next,
			&ishtp_dev->wr_processing_list_head.link, link)
			++count;
		spin_unlock_irqrestore(&ishtp_dev->wr_processing_spinlock,
			flags);
		scnprintf(buf, PAGE_SIZE, "outstanding %u messages\n", count);
		ret = strlen(buf);
	} else if (!strcmp(dev_attr->attr.name, "transfer_path")) {
		/* transfer path is DMA or ipc */
		scnprintf(buf, PAGE_SIZE, "%u\n", ishtp_dev->transfer_path);
		ret = strlen(buf);
	}

	return	ret;
}

ssize_t store_ishtp_dev_props(struct device *dev,
	struct device_attribute *dev_attr, const char *buf, size_t count)
{
	return	-EINVAL;
}

/*
 * store function for transfer_path attribute.
 * this is the only attribute the user may set
 */
ssize_t store_transfer_path(struct device *dev,
	struct device_attribute *dev_attr, const char *buf, size_t count)
{
	struct pci_dev *pdev;
	struct ishtp_device *ishtp_dev;
	int	rv;

	pdev = container_of(dev, struct pci_dev, dev);
	ishtp_dev = pci_get_drvdata(pdev);

	rv = sscanf(buf, " %d ", &ishtp_dev->transfer_path);
	if (!rv)
		return	-EINVAL;
	return	strlen(buf);
}

static struct device_attribute ishtp_dev_state_attr = {
	.attr = {
		.name = "ishtp_dev_state",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = show_ishtp_dev_props,
	.store = store_ishtp_dev_props
};

static struct device_attribute hbm_state_attr = {
	.attr = {
		.name = "hbm_state",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = show_ishtp_dev_props,
	.store = store_ishtp_dev_props
};

static struct device_attribute fw_status_attr = {
	.attr = {
		.name = "fw_status",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = show_ishtp_dev_props,
	.store = store_ishtp_dev_props
};

static struct device_attribute ipc_buf_attr = {
	.attr = {
		.name = "ipc_buf",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = show_ishtp_dev_props,
	.store = store_ishtp_dev_props
};

static struct device_attribute transfer_path = {
	.attr = {
		.name = "transfer_path",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = show_ishtp_dev_props,
	.store = store_transfer_path
};

/**********************************/

static void workqueue_init_function(struct work_struct *work)
{
	struct ishtp_device *dev = ((struct init_work_t *)work)->dev;
	int err;

	dev_set_drvdata(dev->devc, dev);

	device_create_file(dev->devc, &ishtp_dev_state_attr);
	device_create_file(dev->devc, &hbm_state_attr);
	device_create_file(dev->devc, &fw_status_attr);
	device_create_file(dev->devc, &ipc_buf_attr);
	device_create_file(dev->devc, &transfer_path);

#if ISH_LOG
	device_create_file(dev->devc, &read_attr);
	device_create_file(dev->devc, &flush_attr);

	dev->log_head = dev->log_tail = 0;
	dev->print_log = ish_print_log;
	spin_lock_init(&dev->log_spinlock);

	dev->print_log(dev, "[pci-ish]: Build "BUILD_ID "\n");
	dev->print_log(dev, "[pci-ish]: running on %s revision [%02X]\n",
		ishtp_pci_device->revision == REVISION_ID_CHT_A0 ||
		(ishtp_pci_device->revision & REVISION_ID_SI_MASK) ==
		REVISION_ID_CHT_Ax_SI ? "CHT Ax" :
		ishtp_pci_device->revision == REVISION_ID_CHT_B0 ||
		(ishtp_pci_device->revision & REVISION_ID_SI_MASK) ==
		REVISION_ID_CHT_Bx_SI ? "CHT Bx" :
		(ishtp_pci_device->revision & REVISION_ID_SI_MASK) ==
		REVISION_ID_CHT_Kx_SI ? "CHT Kx/Cx" :
		(ishtp_pci_device->revision & REVISION_ID_SI_MASK) ==
		REVISION_ID_CHT_Dx_SI ? "CHT Dx" : "Unknown",
		ishtp_pci_device->revision);
#else
	dev->print_log = ish_print_log_nolog;
#endif /* ISH_LOG */

	init_waitqueue_head(&suspend_wait);

	mutex_lock(&ishtp_mutex);

	if (ish_hw_start(dev)) {
		dev_err(dev->devc, "ISH: Init hw failed.\n");
		err = -ENODEV;
		goto out_err;
	}

	if (ishtp_start(dev)) {
		dev_err(dev->devc, "ISHTP: Protocol init failed.\n");
		err = -ENOENT;
		goto out_err;
	}

	err = ishtp_register(dev);
	if (err)
		goto out_err;

	mutex_unlock(&ishtp_mutex);

	kfree((void *)work);
	return;

out_err:
	mutex_unlock(&ishtp_mutex);
	kfree((void *)work);
}

/**
 * ish_probe - Device Initialization Routine
 *
 * @pdev: PCI device structure
 * @ent: entry in ish_pci_tbl
 *
 * returns 0 on success, <0 on failure.
 */
static int ish_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct ishtp_device *dev;
	struct ish_hw *hw;
	int	err;
	int	rv;

	dev_alert(&pdev->dev, "[ish]: Build "BUILD_ID "\n");
	dev_alert(&pdev->dev, "[ish]: running on %s revision [%02X]\n",
		 pdev->revision == REVISION_ID_CHT_A0 ||
		(pdev->revision & REVISION_ID_SI_MASK) ==
			REVISION_ID_CHT_Ax_SI ? "CHT A0" :
		pdev->revision == REVISION_ID_CHT_B0 ||
		(pdev->revision & REVISION_ID_SI_MASK) ==
			REVISION_ID_CHT_Bx_SI ? "CHT B0" :
		(pdev->revision & REVISION_ID_SI_MASK) ==
			REVISION_ID_CHT_Kx_SI ? "CHT Kx/Cx" :
		(pdev->revision & REVISION_ID_SI_MASK) ==
			REVISION_ID_CHT_Dx_SI ? "CHT Dx" : "Unknown",
		pdev->revision);

	mutex_lock(&ishtp_mutex);
	if (ishtp_pci_device) {
		err = -EEXIST;
		goto end;
	}
	/* enable pci dev */
	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "ISH: Failed to enable PCI device\n");
		goto end;
	}
	/* set PCI host mastering */
	pci_set_master(pdev);
	/* pci request regions for ISH driver */
	err = pci_request_regions(pdev, KBUILD_MODNAME);
	if (err) {
		dev_err(&pdev->dev, "ISH: Failed to get PCI regions\n");
		goto disable_device;
	}

	/* allocates and initializes the ISH dev structure */
	dev = ish_dev_init(pdev);
	if (!dev) {
		err = -ENOMEM;
		goto release_regions;
	}
	hw = to_ish_hw(dev);

	/* mapping IO device memory */
	hw->mem_addr = pci_iomap(pdev, 0, 0);
	if (!hw->mem_addr) {
		dev_err(&pdev->dev, "ISH: mapping I/O range failure\n");
		err = -ENOMEM;
		goto free_device;
	}

	ishtp_pci_device = pdev;
	/* PCI quirk: prevent from being put into D3 state */
	pdev->dev_flags |= PCI_DEV_FLAGS_NO_D3;


	/* request and enable interrupt */
	err = request_irq(pdev->irq, ish_irq_handler, IRQF_NO_SUSPEND,
		KBUILD_MODNAME, dev);
	if (err) {
		dev_err(&pdev->dev, "ISH: request IRQ failure (%d)\n",
			pdev->irq);
		goto free_device;
	}

#if ISH_DEBUGGER
	ishdbg_misc_device.parent = &pdev->dev;
	rv = misc_register(&ishdbg_misc_device);
	if (rv)
		dev_err(&pdev->dev,
			"error starting ISH debugger: %d\n", rv);
	hw_dbg = hw;
#endif /* ISH_DEBUGGER */

	/*
	 * in order to not stick Android boot,
	 * from here & below needs to run in work queue
	 * and here we should return success
	 */
	pci_init_work = kmalloc(sizeof(struct init_work_t), GFP_KERNEL);
	if (!pci_init_work) {
		err = -ENOMEM;
		goto free_device;
	}
	pci_init_work->dev = dev;
	workqueue_for_init = create_workqueue("workqueue_for_init");
	if (!workqueue_for_init) {
		kfree(pci_init_work);
		err = -ENOMEM;
		goto free_device;
	}
	INIT_WORK(&pci_init_work->init_work, workqueue_init_function);
	queue_work(workqueue_for_init, &pci_init_work->init_work);

	mutex_unlock(&ishtp_mutex);
	return 0;

free_device:
	pci_iounmap(pdev, hw->mem_addr);
	kfree(dev);
release_regions:
	pci_release_regions(pdev);
disable_device:
	pci_disable_device(pdev);
end:
	mutex_unlock(&ishtp_mutex);
	dev_err(&pdev->dev, "ISH: PCI driver initialization failed.\n");
	return err;
}

/**
 * ishtp_remove - Device Removal Routine
 *
 * @pdev: PCI device structure
 *
 * ishtp_remove is called by the PCI subsystem to alert the driver
 * that it should release a PCI device.
 */
static void ish_remove(struct pci_dev *pdev)
{
	struct ishtp_device *dev;
	struct ish_hw *hw;

	/*
	 * This happens during power-off/reboot and may be at the same time as
	 * a lot of bi-directional communication happens
	 */
	if (ishtp_pci_device != pdev) {
		dev_err(&pdev->dev, "ISH: wrong PCI device in remove\n");
		return;
	}

	dev = pci_get_drvdata(pdev);
	if (!dev) {
		dev_err(&pdev->dev, "ISH: dev is NULL\n");
		return;
	}

	hw = to_ish_hw(dev);

	/*
	 * Set ISHTP device state to disabled.
	 * Invalidate all other possible communication in both directions
	 */
	ishtp_device_disable(dev);

	free_irq(pdev->irq, dev);
	pci_disable_msi(pdev);
	pci_iounmap(pdev, hw->mem_addr);
	ishtp_pci_device = NULL;
	if (workqueue_for_init) {
		flush_workqueue(workqueue_for_init);
		destroy_workqueue(workqueue_for_init);
		workqueue_for_init = NULL;
	}
	pci_set_drvdata(pdev, NULL);
	ishtp_deregister(dev);
	kfree(dev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

int ish_suspend(struct device *device)
{
	struct pci_dev *pdev = to_pci_dev(device);
	struct ishtp_device *dev = pci_get_drvdata(pdev);

	enable_irq_wake(pdev->irq);

	/* If previous suspend hasn't been asnwered then ISH is likely dead,
	don't attempt nested notification */
	if (suspend_flag)
		return	0;

	suspend_flag = 1;
	send_suspend(dev);

	/* 250 ms should be likely enough for live ISH to flush all IPC buf */
	if (suspend_flag)
		wait_event_timeout(suspend_wait, !suspend_flag, HZ / 4);
	return 0;
}

int ish_resume(struct device *device)
{
	struct pci_dev *pdev = to_pci_dev(device);
	struct ishtp_device *dev = pci_get_drvdata(pdev);

	disable_irq_wake(pdev->irq);
	send_resume(dev);
	return 0;
}

#ifdef CONFIG_PM
static const struct dev_pm_ops ish_pm_ops = {
	.suspend = ish_suspend,
	.resume = ish_resume,
};
#define ISHTP_ISH_PM_OPS	(&ish_pm_ops)
#else
#define ISHTP_ISH_PM_OPS	NULL
#endif /* CONFIG_PM */

/*
 * PCI driver structure
 */
static struct pci_driver ish_driver = {
	.name = KBUILD_MODNAME,
	.id_table = ish_pci_tbl,
	.probe = ish_probe,
	.remove = ish_remove,
	.shutdown = ish_remove,
	.driver.pm = ISHTP_ISH_PM_OPS,
};

module_pci_driver(ish_driver);

MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("Intel(R) Integrated Sensor Hub PCI Device Driver");
MODULE_LICENSE("GPL");

