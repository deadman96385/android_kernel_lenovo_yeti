/*
 * ISHTP bus definitions
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
#ifndef _LINUX_ISHTP_CL_BUS_H
#define _LINUX_ISHTP_CL_BUS_H

#include <linux/device.h>
#include <linux/mod_devicetable.h>

struct ishtp_cl;
struct ishtp_cl_device;
struct ishtp_device;
struct ishtp_msg_hdr;

/**
 * struct ishtp_cl_device - ISHTP device handle
 * An ishtp_cl_device pointer is returned from ishtp_add_device()
 * and links ISHTP bus clients to their actual host client pointer.
 * Drivers for ISHTP devices will get an ishtp_cl_device pointer
 * when being probed and shall use it for doing bus I/O.
 */
struct ishtp_cl_device {
	struct device dev;
	struct ishtp_device	*ishtp_dev;
	struct ishtp_fw_client	*fw_client;
	struct list_head	device_link;
	struct work_struct event_work;
	void (*event_cb)(struct ishtp_cl_device *device);
};

struct ishtp_cl_driver {
	struct device_driver driver;
	const char *name;
	int (*probe)(struct ishtp_cl_device *dev);
	int (*remove)(struct ishtp_cl_device *dev);
};

int	__ishtp_cl_driver_register(struct ishtp_cl_driver *driver,
	struct module *owner);
#define ishtp_cl_driver_register(driver)		\
	__ishtp_cl_driver_register(driver, THIS_MODULE)
void	ishtp_cl_driver_unregister(struct ishtp_cl_driver *driver);

int	ishtp_register_event_cb(struct ishtp_cl_device *device,
	void (*read_cb)(struct ishtp_cl_device *));

int	ishtp_cl_bus_init(void);
void	ishtp_cl_bus_exit(void);
int	ishtp_bus_new_client(struct ishtp_device *dev);
void	ishtp_remove_all_clients(struct ishtp_device *dev);
int	ishtp_cl_device_bind(struct ishtp_cl *cl);
void	ishtp_cl_bus_rx_event(struct ishtp_cl_device *device);
int	ishtp_reset_handler(struct ishtp_device *dev);
int	ishtp_reset_compl_handler(struct ishtp_device *dev);
void	recv_ishtp(struct ishtp_device *dev);

/* Write a multi-fragment message */
int	send_ishtp_msg(struct ishtp_device *dev,
	struct ishtp_msg_hdr *hdr, void *msg, void(*ipc_send_compl)(void *),
	void *ipc_send_compl_prm);

/* Write a single-fragment message */
int	ishtp_write_message(struct ishtp_device *dev,
	struct ishtp_msg_hdr *hdr, unsigned char *buf);

#endif /* _LINUX_ISHTP_CL_BUS_H */
