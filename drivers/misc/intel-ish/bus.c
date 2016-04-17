/*
 * ISHTP bus driver
 *
 * Copyright (c) 2012-2016, Intel Corporation.
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

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <asm/page.h>
#include "bus.h"
#include "ishtp_dev.h"
#include "client.h"
#include "hbm.h"
#include "hw-ish-regs.h"

#define to_ishtp_cl_driver(d) container_of(d, struct ishtp_cl_driver, driver)
#define to_ishtp_cl_device(d) container_of(d, struct ishtp_cl_device, dev)


/* ISHTP Handler for IPC_RESET notification */
int	ishtp_reset_handler(struct ishtp_device *dev)
{
	unsigned long	flags;

	/* Handle FW-initiated reset */
	dev->dev_state = ISHTP_DEV_RESETTING;

	/* Clear BH processing queue - no further HBMs */
	spin_lock_irqsave(&dev->rd_msg_spinlock, flags);
	dev->rd_msg_fifo_head = dev->rd_msg_fifo_tail = 0;
	spin_unlock_irqrestore(&dev->rd_msg_spinlock, flags);

	/* Handle ISH FW reset against upper layers */
	ishtp_bus_remove_all_clients(dev);	/* Remove all client devices */

	return	0;
}
EXPORT_SYMBOL(ishtp_reset_handler);

/* ISHTP handler for IPC_RESET sequence completion  */
int	ishtp_reset_compl_handler(struct ishtp_device *dev)
{
	dev->dev_state = ISHTP_DEV_INIT_CLIENTS;
	dev->hbm_state = ISHTP_HBM_START;
	ishtp_hbm_start_req(dev);

	return	0;
}
EXPORT_SYMBOL(ishtp_reset_compl_handler);

void	recv_ishtp(struct ishtp_device *dev)
{
	uint32_t	msg_hdr;
	struct ishtp_msg_hdr	*ishtp_hdr;

	/* Read ISHTP header dword */
	msg_hdr = dev->ops->ishtp_read_hdr(dev);
	if (!msg_hdr)
		return;

	dev->ops->sync_fw_clock(dev);

	ishtp_hdr = (struct ishtp_msg_hdr *)&msg_hdr;
	dev->ishtp_msg_hdr = msg_hdr;

	/* Sanity check: ISHTP frag. length in header */
	if (ishtp_hdr->length > dev->mtu) {
		dev_err(dev->devc,
			"ISHTP hdr - bad length: %u; dropped [%08X]\n",
			(unsigned)ishtp_hdr->length, msg_hdr);
		return;
	}

	/* ISHTP bus message */
	if (!ishtp_hdr->host_addr && !ishtp_hdr->fw_addr)
		recv_hbm(dev, ishtp_hdr);
	/* ISHTP fixed-client message */
	else if (!ishtp_hdr->host_addr)
		recv_fixed_cl_msg(dev, ishtp_hdr);
	else
		/* ISHTP client message */
		recv_ishtp_cl_msg(dev, ishtp_hdr);
}
EXPORT_SYMBOL(recv_ishtp);

/* Write a multi-fragment message */
int	send_ishtp_msg(struct ishtp_device *dev,
	struct ishtp_msg_hdr *hdr, void *msg, void(*ipc_send_compl)(void *),
	void *ipc_send_compl_prm)
{
	unsigned char	ipc_msg[IPC_FULL_MSG_SIZE];
	uint32_t	drbl_val;

	drbl_val = IPC_BUILD_HEADER(hdr->length + sizeof(struct ishtp_msg_hdr),
		IPC_PROTOCOL_ISHTP, 1);

	memcpy(ipc_msg, &drbl_val, sizeof(uint32_t));
	memcpy(ipc_msg + sizeof(uint32_t), hdr, sizeof(uint32_t));
	memcpy(ipc_msg + 2 * sizeof(uint32_t), msg, hdr->length);
	return	dev->ops->write(dev, ipc_send_compl, ipc_send_compl_prm,
		ipc_msg, 2 * sizeof(uint32_t) + hdr->length);
}

/* Write a single-fragment message */
int ishtp_write_message(struct ishtp_device *dev,
	struct ishtp_msg_hdr *hdr, unsigned char *buf)
{
	return send_ishtp_msg(dev, hdr, buf, NULL, NULL);
}

/**
 * ishtp_fw_cl_by_uuid - locate index of fw client
 *
 * @dev: ishtp device
 * returns fw client index or -ENOENT if not found
 */
int ishtp_fw_cl_by_uuid(struct ishtp_device *dev, const uuid_le *uuid)
{
	int i, res = -ENOENT;
	for (i = 0; i < dev->fw_clients_num; ++i) {
		if (uuid_le_cmp(*uuid, dev->fw_clients[i].props.protocol_name)
				== 0) {
			res = i;
			break;
		}
	}
	return res;
}
EXPORT_SYMBOL(ishtp_fw_cl_by_uuid);

/**
 * ishtp_fw_cl_by_id - return index to fw_clients for client_id
 *
 * @dev: the device structure
 * @client_id: fw client id
 *
 * returns index on success, -ENOENT on failure.
 */

int ishtp_fw_cl_by_id(struct ishtp_device *dev, uint8_t client_id)
{
	int i;
	unsigned long	flags;
	spin_lock_irqsave(&dev->fw_clients_lock, flags);
	for (i = 0; i < dev->fw_clients_num; i++)
		if (dev->fw_clients[i].client_id == client_id)
			break;
	if (WARN_ON(dev->fw_clients[i].client_id != client_id)) {
		spin_unlock_irqrestore(&dev->fw_clients_lock, flags);
		return -ENOENT;
	}

	if (i == dev->fw_clients_num) {
		spin_unlock_irqrestore(&dev->fw_clients_lock, flags);
		return -ENOENT;
	}
	spin_unlock_irqrestore(&dev->fw_clients_lock, flags);
	return i;
}

static int ishtp_cl_device_match(struct device *dev, struct device_driver *drv)
{
	/*
	 * DD -- return true and let driver's probe() routine decide.
	 * we can rearrange it by simply removing match() routine at all
	 */
	return	1;
}

static int ishtp_cl_device_probe(struct device *dev)
{
	struct ishtp_cl_device *device = to_ishtp_cl_device(dev);
	struct ishtp_cl_driver *driver;

	if (!device)
		return 0;

	/* in many cases here will be NULL */
	driver = to_ishtp_cl_driver(dev->driver);
	if (!driver || !driver->probe)
		return -ENODEV;

	return driver->probe(device);
}

static int ishtp_cl_device_remove(struct device *dev)
{
	struct ishtp_cl_device *device = to_ishtp_cl_device(dev);
	struct ishtp_cl_driver *driver;

	if (!device || !dev->driver)
		return 0;

	if (device->event_cb) {
		device->event_cb = NULL;
		cancel_work_sync(&device->event_work);
	}

	driver = to_ishtp_cl_driver(dev->driver);
	if (!driver->remove) {
		dev->driver = NULL;

		return 0;
	}

	return driver->remove(device);
}

static ssize_t modalias_show(struct device *dev, struct device_attribute *a,
	char *buf)
{
	int len;

	len = snprintf(buf, PAGE_SIZE, "ishtp:%s\n", dev_name(dev));
	return (len >= PAGE_SIZE) ? (PAGE_SIZE - 1) : len;
}

static struct device_attribute ishtp_cl_dev_attrs[] = {
	__ATTR_RO(modalias),
	__ATTR_NULL,
};

static int ishtp_cl_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	if (add_uevent_var(env, "MODALIAS=ishtp:%s", dev_name(dev)))
		return -ENOMEM;

	return 0;
}

static struct bus_type ishtp_cl_bus_type = {
	.name		= "ishtp",
	.dev_attrs	= ishtp_cl_dev_attrs,
	.match		= ishtp_cl_device_match,
	.probe		= ishtp_cl_device_probe,
	.remove		= ishtp_cl_device_remove,
	.uevent		= ishtp_cl_uevent,
};

static void ishtp_cl_dev_release(struct device *dev)
{
	kfree(to_ishtp_cl_device(dev));
}

static struct device_type ishtp_cl_device_type = {
	.release	= ishtp_cl_dev_release,
};

/*
 * Allocate ISHTP bus client device, attach it to uuid
 * and register with ISHTP bus
 */
struct ishtp_cl_device *ishtp_bus_add_device(struct ishtp_device *dev,
	uuid_le uuid, char *name)
{
	struct ishtp_cl_device *device;
	int status;
	unsigned long flags;

	device = kzalloc(sizeof(struct ishtp_cl_device), GFP_KERNEL);
	if (!device)
		return NULL;

	device->dev.parent = dev->devc;
	device->dev.bus = &ishtp_cl_bus_type;
	device->dev.type = &ishtp_cl_device_type;
	device->ishtp_dev = dev;

	device->fw_client =
		&dev->fw_clients[dev->fw_client_presentation_num - 1];

	dev_set_name(&device->dev, "%s", name);

	spin_lock_irqsave(&dev->device_list_lock, flags);
	list_add_tail(&device->device_link, &dev->device_list);
	spin_unlock_irqrestore(&dev->device_list_lock, flags);

	status = device_register(&device->dev);
	if (status) {
		spin_lock_irqsave(&dev->device_list_lock, flags);
		list_del(&device->device_link);
		spin_unlock_irqrestore(&dev->device_list_lock, flags);
		dev_err(dev->devc, "Failed to register ISHTP client device\n");
		kfree(device);
		return NULL;
	}
	return device;
}

/*
 * This is a counterpart of ishtp_bus_add_device.
 * Device is unregistered.
 * the device structure is freed in 'ishtp_cl_dev_release' function
 */
void ishtp_bus_remove_device(struct ishtp_cl_device *device)
{
	device_unregister(&device->dev);
}

/*
 * Part of reset flow
 */
void	ishtp_bus_remove_all_clients(struct ishtp_device *ishtp_dev)
{
	struct ishtp_cl_device	*cl_device, *next_device;
	struct ishtp_cl	*cl, *next;
	unsigned long	flags;

	spin_lock_irqsave(&ishtp_dev->cl_list_lock, flags);
	list_for_each_entry_safe(cl, next, &ishtp_dev->cl_list, link) {
		cl->state = ISHTP_CL_DISCONNECTED;

		/*
		 * Wake any pending process. The waiter would check dev->state
		 * and determine that it's not enabled already,
		 * and will return error to its caller
		 */
		if (waitqueue_active(&cl->rx_wait))
			wake_up_interruptible(&cl->rx_wait);
		if (waitqueue_active(&cl->wait_ctrl_res))
			wake_up(&cl->wait_ctrl_res);

		/* Disband any pending read/write requests and free rb */
		ishtp_cl_flush_queues(cl);

		/* Remove read_rb for user-mode API clients */
		if (cl->read_rb) {
			struct ishtp_cl_rb *rb = NULL;

			rb = ishtp_cl_find_read_rb(cl);
			/* Remove entry from read list */
			if (rb)
				list_del(&rb->list);

			rb = cl->read_rb;
			cl->read_rb = NULL;

			if (rb) {
				ishtp_io_rb_free(rb);
				rb = NULL;
			}
		}

		/* Remove all free and in_process rings, both Rx and Tx */
		ishtp_cl_free_rx_ring(cl);
		ishtp_cl_free_tx_ring(cl);

		/* Free client and ISHTP bus client device structures */
		/* don't free host client because it is part of the OS fd
		   structure */
	}
	spin_unlock_irqrestore(&ishtp_dev->cl_list_lock, flags);

	/* remove bus clients */
	spin_lock_irqsave(&ishtp_dev->device_list_lock, flags);
	list_for_each_entry_safe(cl_device, next_device,
		&ishtp_dev->device_list, device_link) {
			list_del(&cl_device->device_link);
			spin_unlock_irqrestore(&ishtp_dev->device_list_lock,
				flags);
			ishtp_bus_remove_device(cl_device);
			spin_lock_irqsave(&ishtp_dev->device_list_lock, flags);
		}
	spin_unlock_irqrestore(&ishtp_dev->device_list_lock, flags);

	/* Free all client structures */
	spin_lock_irqsave(&ishtp_dev->fw_clients_lock, flags);
	kfree(ishtp_dev->fw_clients);
	ishtp_dev->fw_clients = NULL;
	ishtp_dev->fw_clients_num = 0;
	ishtp_dev->fw_client_presentation_num = 0;
	ishtp_dev->fw_client_index = 0;
	bitmap_zero(ishtp_dev->fw_clients_map, ISHTP_CLIENTS_MAX);
	spin_unlock_irqrestore(&ishtp_dev->fw_clients_lock, flags);
}
EXPORT_SYMBOL(ishtp_bus_remove_all_clients);

int __ishtp_cl_driver_register(struct ishtp_cl_driver *driver,
	struct module *owner)
{
	int err;

	driver->driver.name = driver->name;
	driver->driver.owner = owner;
	driver->driver.bus = &ishtp_cl_bus_type;

	err = driver_register(&driver->driver);
	if (err)
		return err;
	return 0;
}
EXPORT_SYMBOL(__ishtp_cl_driver_register);

void ishtp_cl_driver_unregister(struct ishtp_cl_driver *driver)
{
	driver_unregister(&driver->driver);
}
EXPORT_SYMBOL(ishtp_cl_driver_unregister);

static void ishtp_bus_event_work(struct work_struct *work)
{
	struct ishtp_cl_device *device;

	device = container_of(work, struct ishtp_cl_device, event_work);

	if (device->event_cb)
		device->event_cb(device);
}

void ishtp_cl_bus_rx_event(struct ishtp_cl_device *device)
{
	if (!device || !device->event_cb)
		return;

	if (device->event_cb)
		schedule_work(&device->event_work);
}

int ishtp_register_event_cb(struct ishtp_cl_device *device,
	void (*event_cb)(struct ishtp_cl_device *))
{
	if (device->event_cb)
		return -EALREADY;

	device->event_cb = event_cb;
	INIT_WORK(&device->event_work, ishtp_bus_event_work);

	return 0;
}
EXPORT_SYMBOL(ishtp_register_event_cb);

ssize_t cl_prop_read(struct device *dev, struct device_attribute *dev_attr,
	char *buf)
{
	ssize_t	rv = -EINVAL;
	struct ishtp_cl_device	*cl_device = to_ishtp_cl_device(dev);

	if (!strcmp(dev_attr->attr.name, "max_msg_length")) {
		scnprintf(buf, PAGE_SIZE, "%u\n",
			(unsigned)cl_device->fw_client->props.max_msg_length);
		rv = strlen(buf);
	} else if (!strcmp(dev_attr->attr.name, "protocol_version")) {
		scnprintf(buf, PAGE_SIZE, "%u\n",
			(unsigned)cl_device->fw_client->props.protocol_version);
		rv = strlen(buf);
	} else if (!strcmp(dev_attr->attr.name, "max_number_of_connections")) {
		scnprintf(buf, PAGE_SIZE, "%u\n",
			(unsigned)cl_device->fw_client->props.max_number_of_connections);
		rv = strlen(buf);
	} else if (!strcmp(dev_attr->attr.name, "fixed_address")) {
		scnprintf(buf, PAGE_SIZE, "%u\n",
			(unsigned)cl_device->fw_client->props.fixed_address);
		rv = strlen(buf);
	} else if (!strcmp(dev_attr->attr.name, "single_recv_buf")) {
		scnprintf(buf, PAGE_SIZE, "%u\n",
			(unsigned)cl_device->fw_client->props.single_recv_buf);
		rv = strlen(buf);
	} else if (!strcmp(dev_attr->attr.name, "dma_hdr_len")) {
		scnprintf(buf, PAGE_SIZE, "%u\n",
			(unsigned)cl_device->fw_client->props.dma_hdr_len);
		rv = strlen(buf);
	} else if (!strcmp(dev_attr->attr.name, "client_id")) {
		scnprintf(buf, PAGE_SIZE, "%u\n",
			(unsigned)cl_device->fw_client->client_id);
		rv = strlen(buf);
	}

	return	rv;
}

ssize_t	cl_prop_write(struct device *dev, struct device_attribute *dev_attr,
	const char *buf, size_t count)
{
	return	-EINVAL;
}

static struct device_attribute	max_msg_length = {
	.attr = {
		.name = "max_msg_length",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = cl_prop_read,
	.store = cl_prop_write
};

static struct device_attribute	protocol_version = {
	.attr = {
		.name = "protocol_version",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = cl_prop_read,
	.store = cl_prop_write
};

static struct device_attribute	max_number_of_connections = {
	.attr = {
		.name = "max_number_of_connections",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = cl_prop_read,
	.store = cl_prop_write
};

static struct device_attribute	fixed_address = {
	.attr = {
		.name = "fixed_address",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = cl_prop_read,
	.store = cl_prop_write
};

static struct device_attribute	single_recv_buf = {
	.attr = {
		.name = "single_recv_buf",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = cl_prop_read,
	.store = cl_prop_write
};

static struct device_attribute	dma_hdr_len = {
	.attr = {
		.name = "dma_hdr_len",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = cl_prop_read,
	.store = cl_prop_write
};

static struct device_attribute	client_id = {
	.attr = {
		.name = "client_id",
		.mode = (S_IWUSR | S_IRUGO)
	},
	.show = cl_prop_read,
	.store = cl_prop_write
};

/*
 * Enum-completion callback for ISHTP bus -
 * ishtp_device has reported its clients
 */
int	ishtp_bus_new_client(struct ishtp_device *dev)
{
	int	i;
	char	*dev_name;
	struct ishtp_cl_device	*cl_device;
	uuid_le	device_uuid;

	/*
	 * For all reported clients, create an unconnected client and add its
	 * device to ISHTP bus.
	 * If appropriate driver has loaded, this will trigger its probe().
	 * Otherwise, probe() will be called when driver is loaded
	 */
	i = dev->fw_client_presentation_num - 1;
	device_uuid = dev->fw_clients[i].props.protocol_name;
	dev_name = kasprintf(GFP_KERNEL,
		"{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		device_uuid.b[3], device_uuid.b[2], device_uuid.b[1],
		device_uuid.b[0], device_uuid.b[5], device_uuid.b[4],
		device_uuid.b[7], device_uuid.b[6], device_uuid.b[8],
		device_uuid.b[9], device_uuid.b[10], device_uuid.b[11],
		device_uuid.b[12], device_uuid.b[13], device_uuid.b[14],
		device_uuid.b[15]);
	if (!dev_name)
		return	-ENOMEM;

	cl_device = ishtp_bus_add_device(dev, device_uuid, dev_name);
	if (!cl_device) {
		kfree(dev_name);
		return	-ENOENT;
	}

	/* Export several properties per client device */
	device_create_file(&cl_device->dev, &max_msg_length);
	device_create_file(&cl_device->dev, &protocol_version);
	device_create_file(&cl_device->dev, &max_number_of_connections);
	device_create_file(&cl_device->dev, &fixed_address);
	device_create_file(&cl_device->dev, &single_recv_buf);
	device_create_file(&cl_device->dev, &dma_hdr_len);
	device_create_file(&cl_device->dev, &client_id);
	kfree(dev_name);

	return	0;
}

static int	does_driver_bind_uuid(struct device *dev, void *id)
{
	uuid_le	*uuid = id;
	struct ishtp_cl_device	*device;

	if (!dev->driver)
		return	0;

	device = to_ishtp_cl_device(dev);
	if (!uuid_le_cmp(device->fw_client->props.protocol_name, *uuid))
		return	1;

	return	0;
}

/* Checks if there is a driver attached to this uuid */
int	ishtp_can_client_connect(struct ishtp_device *ishtp_dev, uuid_le *uuid)
{
	int	rv;

	rv = bus_for_each_dev(&ishtp_cl_bus_type, NULL, uuid,
		does_driver_bind_uuid);
	return	!rv;
}

/* Binds connected ishtp_cl to ISHTP bus device */
int	ishtp_cl_device_bind(struct ishtp_cl *cl)
{
	struct ishtp_cl_device	*cl_device, *next;
	unsigned long flags;
	int	rv;

	if (!cl->fw_client_id || cl->state != ISHTP_CL_CONNECTED)
		return	-EFAULT;

	rv = -ENOENT;
	spin_lock_irqsave(&cl->dev->device_list_lock, flags);
	list_for_each_entry_safe(cl_device, next, &cl->dev->device_list,
			device_link) {
		if (cl_device->fw_client->client_id == cl->fw_client_id) {
			cl->device = cl_device;
			rv = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&cl->dev->device_list_lock, flags);
	return	rv;
}

int __init ishtp_cl_bus_init(void)
{
	int	rv;

	rv = bus_register(&ishtp_cl_bus_type);
	return	rv;
}

void __exit ishtp_cl_bus_exit(void)
{
	bus_unregister(&ishtp_cl_bus_type);
}
