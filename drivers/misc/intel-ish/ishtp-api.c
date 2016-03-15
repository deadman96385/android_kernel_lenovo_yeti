/*
 * User-mode ISHTP API
 *
 * Copyright (c) 2016, Intel Corporation.
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
#include <linux/ioctl.h>
#include <linux/uuid.h>
#include <linux/compat.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include "ishtp-api.h"
#include "ishtp_dev.h"
#include "client.h"

/* ishtp_open - the open function */
static int ishtp_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct ishtp_cl *cl;
	struct ishtp_device *dev;
	int err;

	/* Non-blocking semantics are not supported */
	if (file->f_flags & O_NONBLOCK)
		return	-EINVAL;

	err = -ENODEV;
	if (!misc->parent)
		goto out;

	dev = dev_get_drvdata(misc->parent);
	if (!dev)
		goto out;

	err = -ENOMEM;
	cl = ishtp_cl_allocate(dev);
	if (!cl)
		goto out_free;

	/*
	 * We may have a case of issued open() with
	 * dev->dev_state == ISHTP_DEV_DISABLED, as part of re-enabling path
	 */
	err = -ENODEV;
	if (dev->dev_state != ISHTP_DEV_ENABLED)
		goto out_free;

	err = ishtp_cl_link(cl, ISHTP_HOST_CLIENT_ID_ANY);
	if (err)
		goto out_free;

	file->private_data = cl;

	return nonseekable_open(inode, file);

out_free:
	kfree(cl);
out:
	return err;
}

/* ishtp_release - the release function */
static int ishtp_release(struct inode *inode, struct file *file)
{
	struct ishtp_cl *cl = file->private_data;
	struct ishtp_device *dev;
	int rets = 0;

	if (WARN_ON(!cl || !cl->dev))
		return -ENODEV;

	dev = cl->dev;

	/*
	 * May happen if device sent FW reset or was intentionally
	 * halted by host SW. The client is then invalid
	 */
	if ((dev->dev_state == ISHTP_DEV_ENABLED) &&
			(cl->state == ISHTP_CL_CONNECTED)) {
		cl->state = ISHTP_CL_DISCONNECTING;
		rets = ishtp_cl_disconnect(cl);
	}

	ishtp_cl_unlink(cl);
	ishtp_cl_flush_queues(cl);
	file->private_data = NULL;

	/* disband and free all Tx and Rx client-level rings */
	ishtp_cl_free(cl);

	return rets;
}

/* ishtp_read - the read function */
static ssize_t ishtp_read(struct file *file, char __user *ubuf,
			size_t length, loff_t *offset)
{
	struct ishtp_cl *cl = file->private_data;
	struct ishtp_cl_rb *rb = NULL;
	struct ishtp_device *dev;
	unsigned long flags;
	int rets = 0;

	/* Non-blocking semantics are not supported */
	if (file->f_flags & O_NONBLOCK)
		return	-EINVAL;

	if (WARN_ON(!cl || !cl->dev))
		return -ENODEV;

	dev = cl->dev;
	if (dev->dev_state != ISHTP_DEV_ENABLED) {
		rets = -ENODEV;
		goto out;
	}

	spin_lock_irqsave(&cl->in_process_spinlock, flags);
	if (!list_empty(&cl->in_process_list.list)) {
		rb = list_entry(cl->in_process_list.list.next,
			struct ishtp_cl_rb, list);
		list_del_init(&rb->list);
		spin_unlock_irqrestore(&cl->in_process_spinlock, flags);
		goto copy_buffer;
	}
	spin_unlock_irqrestore(&cl->in_process_spinlock, flags);

	if (waitqueue_active(&cl->rx_wait)) {
		rets = -EBUSY;
		goto out;
	}

	if (wait_event_interruptible(cl->rx_wait,
			(dev->dev_state == ISHTP_DEV_ENABLED &&
			(cl->read_rb || ISHTP_CL_INITIALIZING == cl->state ||
			ISHTP_CL_DISCONNECTED == cl->state ||
			ISHTP_CL_DISCONNECTING == cl->state)))) {
		dev_err(ishtp_misc_device.this_device,
			"Woke up not in success; signal pending = %d signal = %08lX\n",
			signal_pending(current),
			current->pending.signal.sig[0]);
		return	-ERESTARTSYS;
	}

	/*
	 * If FW reset arrived, this will happen. Don't check cl->,
	 * as 'cl' may be freed already
	 */
	if (dev->dev_state != ISHTP_DEV_ENABLED) {
		rets = -ENODEV;
		goto	out;
	}

	if (ISHTP_CL_INITIALIZING == cl->state ||
			ISHTP_CL_DISCONNECTED == cl->state ||
			ISHTP_CL_DISCONNECTING == cl->state) {
		rets = -EBUSY;
		goto out;
	}

	rb = cl->read_rb;
	if (!rb) {
		rets = -ENODEV;
		goto out;
	}

copy_buffer:
	/* now copy the data to user space */
	if (length == 0 || ubuf == NULL || *offset > rb->buf_idx) {
		rets = -EMSGSIZE;
		goto free;
	}

	/* length is being truncated, however buf_idx may point beyond that */
	length = min_t(size_t, length, rb->buf_idx - *offset);

	if (copy_to_user(ubuf, rb->buffer.data + *offset, length)) {
		rets = -EFAULT;
		goto free;
	}

	rets = length;
	*offset += length;
	if ((unsigned long)*offset < rb->buf_idx)
		goto out;

free:
	ishtp_io_rb_recycle(rb);
	cl->read_rb = NULL;
	*offset = 0;
out:
	return rets;
}

/* ishtp_write - the write function */
static ssize_t ishtp_write(struct file *file, const char __user *ubuf,
	size_t length, loff_t *offset)
{
	struct ishtp_cl *cl = file->private_data;
	void *write_buf = NULL;
	struct ishtp_device *dev;
	int rets;

	/* Non-blocking semantics are not supported */
	if (file->f_flags & O_NONBLOCK)
		return	-EINVAL;

	if (WARN_ON(!cl || !cl->dev))
		return -ENODEV;

	dev = cl->dev;

	if (dev->dev_state != ISHTP_DEV_ENABLED) {
		rets = -ENODEV;
		goto out;
	}

	if (cl->state != ISHTP_CL_CONNECTED) {
		dev_err(ishtp_misc_device.this_device,
			"host client = %d, is not connected to fw client = %d",
			cl->host_client_id, cl->fw_client_id);
		rets = -ENODEV;
		goto out;
	}

	if (length <= 0) {
		rets = -EMSGSIZE;
		goto out;
	}

	if (length > cl->device->fw_client->props.max_msg_length) {
		rets = -EMSGSIZE;
		goto out;
	}

	write_buf = kmalloc(length, GFP_KERNEL);
	if (!write_buf) {
		dev_err(ishtp_misc_device.this_device,
			"write buffer allocation failed\n");
		rets = -ENOMEM;
		goto	out;
	}

	rets = copy_from_user(write_buf, ubuf, length);
	if (rets)
		goto out;
	rets = ishtp_cl_send(cl, write_buf, length);
	if (!rets)
		rets = length;
	else
		rets = -EIO;
out:
	kfree(write_buf);
	return rets;
}

/* ishtp_ioctl_connect_client - the connect to fw client IOCTL function */
static int ishtp_ioctl_connect_client(struct file *file,
	struct ishtp_connect_client_data *data)
{
	struct ishtp_device *dev;
	struct ishtp_client *client;
	struct ishtp_cl *cl = file->private_data;
	unsigned long flags;
	int i;

	if (WARN_ON(!cl || !cl->dev))
		return -ENODEV;

	dev = cl->dev;

	if (dev->dev_state != ISHTP_DEV_ENABLED)
		return -ENODEV;

	if (cl->state != ISHTP_CL_INITIALIZING &&
			cl->state != ISHTP_CL_DISCONNECTED)
		return -EBUSY;

	/* find the fw client we're trying to connect to */
	spin_lock_irqsave(&dev->fw_clients_lock, flags);
	i = ishtp_fw_cl_by_uuid(dev, &data->in_client_uuid);
	if (i < 0 || dev->fw_clients[i].props.fixed_address) {
		spin_unlock_irqrestore(&dev->fw_clients_lock, flags);
		return -ENODEV;
	}
	/* Check if there's driver attached to this UUID */
	if (!ishtp_can_client_connect(dev, &data->in_client_uuid))
		return -EBUSY;

	cl->fw_client_id = dev->fw_clients[i].client_id;
	cl->state = ISHTP_CL_CONNECTING;

	/* prepare the output buffer */
	client = &data->out_client_properties;
	client->max_msg_length = dev->fw_clients[i].props.max_msg_length;
	client->protocol_version = dev->fw_clients[i].props.protocol_version;
	spin_unlock_irqrestore(&dev->fw_clients_lock, flags);

	return ishtp_cl_connect(cl);
}

/* ishtp_ioctl - the IOCTL function */
static long ishtp_ioctl(struct file *file, unsigned int cmd, unsigned long data)
{
	struct ishtp_device *dev;
	struct ishtp_cl *cl = file->private_data;
	struct ishtp_connect_client_data *connect_data = NULL;
	unsigned ring_size;
	int rets = 0;

	if (!cl)
		return -EINVAL;

	dev = cl->dev;

	/* Test API for triggering PCI reset */
	if (cmd == IOCTL_ISH_HW_RESET)
		return	ish_hw_reset(dev);

	if (cmd == IOCTL_ISHTP_SET_RX_FIFO_SIZE) {
		ring_size = data;
		if (ring_size > CL_MAX_RX_RING_SIZE)
			return	-EINVAL;
		if (cl->state != ISHTP_CL_INITIALIZING)
			return	-EBUSY;
		cl->rx_ring_size = ring_size;
		return	0;
	}

	if (cmd == IOCTL_ISHTP_SET_TX_FIFO_SIZE) {
		ring_size = data;
		if (ring_size > CL_MAX_TX_RING_SIZE)
			return	-EINVAL;
		if (cl->state != ISHTP_CL_INITIALIZING)
			return	-EBUSY;
		cl->tx_ring_size = ring_size;
		return	0;
	}

	if (cmd == IOCTL_ISH_GET_FW_STATUS) {
		char fw_stat_buf[20];
		scnprintf(fw_stat_buf, sizeof(fw_stat_buf),
			"%08X\n", dev->ops->get_fw_status(dev));
		if (copy_to_user((char __user *)data, fw_stat_buf,
				strlen(fw_stat_buf)))
			return -EFAULT;
		return strlen(fw_stat_buf);
	}

	if (cmd != IOCTL_ISHTP_CONNECT_CLIENT)
		return -EINVAL;

	if (WARN_ON(!cl || !cl->dev))
		return -ENODEV;

	if (dev->dev_state != ISHTP_DEV_ENABLED) {
		rets = -ENODEV;
		goto out;
	}

	connect_data = kzalloc(sizeof(struct ishtp_connect_client_data),
							GFP_KERNEL);
	if (!connect_data) {
		rets = -ENOMEM;
		goto out;
	}
	if (copy_from_user(connect_data, (char __user *)data,
			sizeof(struct ishtp_connect_client_data))) {
		rets = -EFAULT;
		goto out;
	}

	rets = ishtp_ioctl_connect_client(file, connect_data);

	/* if all is ok, copying the data back to user. */
	if (rets)
		goto out;

	if (copy_to_user((char __user *)data, connect_data,
				sizeof(struct ishtp_connect_client_data))) {
		rets = -EFAULT;
		goto out;
	}

out:
	kfree(connect_data);
	return rets;
}

/* ishtp_compat_ioctl - the compat IOCTL function */
#ifdef CONFIG_COMPAT
static long ishtp_compat_ioctl(struct file *file,
			unsigned int cmd, unsigned long data)
{
	return ishtp_ioctl(file, cmd, (unsigned long)compat_ptr(data));
}
#endif /* CONFIG_COMPAT */


/*
 * file operations structure will be used for ishtp char device.
 */
static const struct file_operations ishtp_fops = {
	.owner = THIS_MODULE,
	.read = ishtp_read,
	.unlocked_ioctl = ishtp_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ishtp_compat_ioctl,
#endif /* CONFIG_COMPAT */
	.open = ishtp_open,
	.release = ishtp_release,
	.write = ishtp_write,
	.llseek = no_llseek
};

/*
 * Misc Device Struct
 */
struct miscdevice	ishtp_misc_device = {
		.name = "ish",
		.fops = &ishtp_fops,
		.minor = MISC_DYNAMIC_MINOR,
};

int ishtp_register(struct ishtp_device *dev)
{
	int ret;
	ishtp_misc_device.parent = dev->devc;
	ret = misc_register(&ishtp_misc_device);
	if (ret)
		return ret;

	if (ishtp_dbgfs_register(dev, ishtp_misc_device.name))
		dev_err(ishtp_misc_device.this_device,
			"cannot register debugfs\n");
	return 0;
}
EXPORT_SYMBOL(ishtp_register);

void ishtp_deregister(struct ishtp_device *dev)
{
	if (ishtp_misc_device.parent == NULL)
		return;

	ishtp_dbgfs_deregister(dev);
	misc_deregister(&ishtp_misc_device);
	ishtp_misc_device.parent = NULL;
}
EXPORT_SYMBOL(ishtp_deregister);

static int __init ishtp_init(void)
{
	return ishtp_cl_bus_init();
}
module_init(ishtp_init);

static void __exit ishtp_exit(void)
{
	ishtp_cl_bus_exit();
}
/*
 * Currently, we block the removal of the module
 * (it will be a permanent module).
 * The ish device is not a removable device, so its drivers
 * should be allways up.
 */
/* module_exit(ishtp_exit); */

MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("Intel(R)ISHTP user-mode API ");
MODULE_LICENSE("GPL");

