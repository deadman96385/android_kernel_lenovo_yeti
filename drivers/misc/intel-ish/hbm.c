/*
 * ISHTP bus layer messages handling
 *
 * Copyright (c) 2003-2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 */

#include <linux/export.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/miscdevice.h>
#include "ishtp_dev.h"
#include "hbm.h"
#include "client.h"

/* ishtp_hbm_fw_cl_allocate - allocates storage for fw clients */
static void ishtp_hbm_fw_cl_allocate(struct ishtp_device *dev)
{
	struct ishtp_fw_client *clients;
	int b;

	/* count how many ISH clients we have */
	for_each_set_bit(b, dev->fw_clients_map, ISHTP_CLIENTS_MAX)
		dev->fw_clients_num++;

	if (dev->fw_clients_num <= 0)
		return;

	kfree(dev->fw_clients);
	dev->fw_clients = NULL;

	/* allocate storage for fw clients representation */
	clients = kcalloc(dev->fw_clients_num, sizeof(struct ishtp_fw_client),
		GFP_ATOMIC);
	if (!clients) {
		dev_err(dev->devc, "memory allocation for FW clients failed.\n");
		dev->dev_state = ISHTP_DEV_RESETTING;
		ish_hw_reset(dev);
		return;
	}
	dev->fw_clients = clients;
	return;
}

/**
 * ishtp_hbm_cl_hdr - construct client hbm header
 * @cl: client
 * @hbm_cmd: host bus message command
 * @buf: buffer for cl header
 * @len: buffer length
 */
static inline void ishtp_hbm_cl_hdr(struct ishtp_cl *cl, uint8_t hbm_cmd,
	void *buf, size_t len)
{
	struct ishtp_hbm_cl_cmd *cmd = buf;

	memset(cmd, 0, len);

	cmd->hbm_cmd = hbm_cmd;
	cmd->host_addr = cl->host_client_id;
	cmd->fw_addr = cl->fw_client_id;
}

/* ishtp_hbm_cl_addr_equal - tells if they have the same address */
static inline bool ishtp_hbm_cl_addr_equal(struct ishtp_cl *cl, void *buf)
{
	struct ishtp_hbm_cl_cmd *cmd = buf;
	return cl->host_client_id == cmd->host_addr &&
		cl->fw_client_id == cmd->fw_addr;
}

int ishtp_hbm_start_wait(struct ishtp_device *dev)
{
	int ret;
	if (dev->hbm_state > ISHTP_HBM_START)
		return 0;

	dev_dbg(dev->devc, "Going to wait for ishtp start. hbm_state=%08X\n",
		dev->hbm_state);
	ret = wait_event_timeout(dev->wait_hbm_recvd_msg,
			dev->hbm_state >= ISHTP_HBM_STARTED,
			(ISHTP_INTEROP_TIMEOUT * HZ));

	dev_dbg(dev->devc,
		"Woke up from waiting for ishtp start. hbm_state=%08X\n",
		dev->hbm_state);

	if (ret <= 0 && (dev->hbm_state <= ISHTP_HBM_START)) {
		dev->hbm_state = ISHTP_HBM_IDLE;
		dev_err(dev->devc,
			"wating for ishtp start failed. ret=%d hbm_state=%08X\n",
			ret, dev->hbm_state);
		return -ETIMEDOUT;
	}
	return 0;
}

/* ishtp_hbm_start_req - sends start request message */
int ishtp_hbm_start_req(struct ishtp_device *dev)
{
	struct ishtp_msg_hdr hdr;
	unsigned char data[128];
	struct ishtp_msg_hdr *ishtp_hdr = &hdr;
	struct hbm_host_version_request *start_req;
	const size_t len = sizeof(struct hbm_host_version_request);

	ishtp_hbm_hdr(ishtp_hdr, len);

	/* host start message */
	start_req = (struct hbm_host_version_request *)data;
	memset(start_req, 0, len);
	start_req->hbm_cmd = HOST_START_REQ_CMD;
	start_req->host_version.major_version = HBM_MAJOR_VERSION;
	start_req->host_version.minor_version = HBM_MINOR_VERSION;

	/*
	 * (!) Response to HBM start may be so quick that this thread would get
	 * preempted BEFORE managing to set hbm_state = ISHTP_HBM_START.
	 * So set it at first, change back to ISHTP_HBM_IDLE upon failure
	 */
	dev->hbm_state = ISHTP_HBM_START;
	if (ishtp_write_message(dev, ishtp_hdr, data)) {
		dev_err(dev->devc, "version message send failed\n");
		dev->dev_state = ISHTP_DEV_RESETTING;
		dev->hbm_state = ISHTP_HBM_IDLE;
		ish_hw_reset(dev);
		return -ENODEV;
	}

	return 0;
}
EXPORT_SYMBOL(ishtp_hbm_start_req);

/* ishtp_hbm_enum_clients_req - sends enumeration client request message */
void ishtp_hbm_enum_clients_req(struct ishtp_device *dev)
{
	struct ishtp_msg_hdr hdr;
	unsigned char data[128];
	struct ishtp_msg_hdr *ishtp_hdr = &hdr;
	struct hbm_host_enum_request *enum_req;
	const size_t len = sizeof(struct hbm_host_enum_request);
	/* enumerate clients */
	ishtp_hbm_hdr(ishtp_hdr, len);

	enum_req = (struct hbm_host_enum_request *)data;
	memset(enum_req, 0, len);
	enum_req->hbm_cmd = HOST_ENUM_REQ_CMD;

	if (ishtp_write_message(dev, ishtp_hdr, data)) {
		dev->dev_state = ISHTP_DEV_RESETTING;
		dev_err(dev->devc, "enumeration request send failed\n");
		ish_hw_reset(dev);
	}
	dev->hbm_state = ISHTP_HBM_ENUM_CLIENTS;
	return;
}

/* ishtp_hbm_prop_requsest - request property for a single client */
static int ishtp_hbm_prop_req(struct ishtp_device *dev)
{

	struct ishtp_msg_hdr hdr;
	unsigned char data[128];
	struct ishtp_msg_hdr *ishtp_hdr = &hdr;
	struct hbm_props_request *prop_req;
	const size_t len = sizeof(struct hbm_props_request);
	unsigned long next_client_index;
	uint8_t client_num;

	client_num = dev->fw_client_presentation_num;

	next_client_index = find_next_bit(dev->fw_clients_map,
		ISHTP_CLIENTS_MAX, dev->fw_client_index);

	/* We got all client properties */
	if (next_client_index == ISHTP_CLIENTS_MAX) {
		dev->hbm_state = ISHTP_HBM_WORKING;
		dev->dev_state = ISHTP_DEV_ENABLED;

		for (dev->fw_client_presentation_num = 1;
			dev->fw_client_presentation_num < client_num + 1;
				++dev->fw_client_presentation_num)
			/* Add new client device */
			ishtp_bus_new_client(dev);
		return 0;
	}

	dev->fw_clients[client_num].client_id = next_client_index;


	ishtp_hbm_hdr(ishtp_hdr, len);
	prop_req = (struct hbm_props_request *)data;

	memset(prop_req, 0, sizeof(struct hbm_props_request));

	prop_req->hbm_cmd = HOST_CLIENT_PROPERTIES_REQ_CMD;
	prop_req->address = next_client_index;

	if (ishtp_write_message(dev, ishtp_hdr, data)) {
		dev->dev_state = ISHTP_DEV_RESETTING;
		dev_err(dev->devc, "properties request send failed\n");
		ish_hw_reset(dev);
		return -EIO;
	}

	dev->fw_client_index = next_client_index;

	return 0;
}

/* ishtp_hbm_stop_req - send stop request message */
static void ishtp_hbm_stop_req(struct ishtp_device *dev)
{
	struct ishtp_msg_hdr hdr;
	unsigned char data[128];
	struct ishtp_msg_hdr *ishtp_hdr = &hdr;
	struct hbm_host_stop_request *req;
	const size_t len = sizeof(struct hbm_host_stop_request);

	ishtp_hbm_hdr(ishtp_hdr, len);
	req = (struct hbm_host_stop_request *)data;

	memset(req, 0, sizeof(struct hbm_host_stop_request));
	req->hbm_cmd = HOST_STOP_REQ_CMD;
	req->reason = DRIVER_STOP_REQUEST;

	ishtp_write_message(dev, ishtp_hdr, data);
}

/* ishtp_hbm_cl_flow_control_req - sends flow control requst */
int ishtp_hbm_cl_flow_control_req(struct ishtp_device *dev, struct ishtp_cl *cl)
{
	struct ishtp_msg_hdr hdr;
	unsigned char data[128];
	struct ishtp_msg_hdr *ishtp_hdr = &hdr;
	const size_t len = sizeof(struct hbm_flow_control);
	int	rv;
	unsigned	num_frags;
	unsigned long	flags;

	spin_lock_irqsave(&cl->fc_spinlock, flags);
	ishtp_hbm_hdr(ishtp_hdr, len);
	ishtp_hbm_cl_hdr(cl, ISHTP_FLOW_CONTROL_CMD, data, len);

	/* Sync possible race when RB recycle and packet receive paths
	   both try to send an out FC */
	if (cl->out_flow_ctrl_creds) {
		spin_unlock_irqrestore(&cl->fc_spinlock, flags);
		return	0;
	}

	num_frags = cl->recv_msg_num_frags;
	cl->recv_msg_num_frags = 0;

	rv = ishtp_write_message(dev, ishtp_hdr, data);
	if (!rv) {
		struct timeval	tv;

		++cl->out_flow_ctrl_creds;
		++cl->out_flow_ctrl_cnt;
		do_gettimeofday(&tv);
		cl->out_fc_sec = tv.tv_sec;
		cl->out_fc_usec = tv.tv_usec;
		if (cl->rx_sec && cl->rx_usec) {
			unsigned long	s, us;

			s = cl->out_fc_sec - cl->rx_sec;
			us = cl->out_fc_usec - cl->rx_usec;
			if (cl->rx_usec > cl->out_fc_usec) {
				us += 1000000UL;
				--s;
			}
			if (s > cl->max_fc_delay_sec ||
					(s == cl->max_fc_delay_sec &&
					us > cl->max_fc_delay_usec)) {
				cl->max_fc_delay_sec = s;
				cl->max_fc_delay_usec = us;
			}
		}
	} else {
		++cl->err_send_fc;
	}

	spin_unlock_irqrestore(&cl->fc_spinlock, flags);
	return	rv;
}
EXPORT_SYMBOL(ishtp_hbm_cl_flow_control_req);

/* ishtp_hbm_cl_disconnect_req - sends disconnect message to fw */
int ishtp_hbm_cl_disconnect_req(struct ishtp_device *dev, struct ishtp_cl *cl)
{
	struct ishtp_msg_hdr hdr;
	unsigned char data[128];
	struct ishtp_msg_hdr *ishtp_hdr = &hdr;
	const size_t len = sizeof(struct hbm_client_connect_request);

	ishtp_hbm_hdr(ishtp_hdr, len);
	ishtp_hbm_cl_hdr(cl, CLIENT_DISCONNECT_REQ_CMD, data, len);

	return ishtp_write_message(dev, ishtp_hdr, data);
}

/*
 * ishtp_hbm_cl_disconnect_res - disconnect response from FW
 *
 * @rs: disconnect response bus message
 */
static void ishtp_hbm_cl_disconnect_res(struct ishtp_device *dev,
	struct hbm_client_connect_response *rs)
{
	struct ishtp_cl *cl = NULL, *next = NULL;
	unsigned long	flags;

	spin_lock_irqsave(&dev->cl_list_lock, flags);
	list_for_each_entry_safe(cl, next, &dev->cl_list, link) {
		if (!rs->status && ishtp_hbm_cl_addr_equal(cl, rs)) {
			cl->state = ISHTP_CL_DISCONNECTED;
			break;
		}
	}
	if (cl)
		wake_up(&cl->wait_ctrl_res);
	spin_unlock_irqrestore(&dev->cl_list_lock, flags);
}

/* ishtp_hbm_cl_connect_req - send connection request to specific fw client */
int ishtp_hbm_cl_connect_req(struct ishtp_device *dev, struct ishtp_cl *cl)
{
	struct ishtp_msg_hdr hdr;
	unsigned char data[128];
	struct ishtp_msg_hdr *ishtp_hdr = &hdr;
	const size_t len = sizeof(struct hbm_client_connect_request);

	ishtp_hbm_hdr(ishtp_hdr, len);
	ishtp_hbm_cl_hdr(cl, CLIENT_CONNECT_REQ_CMD, data, len);

	return ishtp_write_message(dev, ishtp_hdr, data);
}
EXPORT_SYMBOL(ishtp_hbm_cl_connect_req);

/**
 * ishtp_hbm_cl_connect_res - connect resposne from the FW
 *
 * @rs: connect response bus message
 */
static void ishtp_hbm_cl_connect_res(struct ishtp_device *dev,
	struct hbm_client_connect_response *rs)
{
	struct ishtp_cl *cl = NULL, *next = NULL;
	unsigned long	flags;

	spin_lock_irqsave(&dev->cl_list_lock, flags);
	list_for_each_entry_safe(cl, next, &dev->cl_list, link) {
		if (ishtp_hbm_cl_addr_equal(cl, rs)) {
			if (!rs->status) {
				cl->state = ISHTP_CL_CONNECTED;
				cl->status = 0;
			} else {
				cl->state = ISHTP_CL_DISCONNECTED;
				cl->status = -ENODEV;
			}
			break;
		}
	}
	if (cl)
		wake_up(&cl->wait_ctrl_res);
	spin_unlock_irqrestore(&dev->cl_list_lock, flags);
}

/**
 * ishtp_client_disconnect_request - disconnect request initiated by fw
 * host sends disconnect response
 *
 * @dev: the device structure.
 * @disconnect_req: disconnect request bus message from the fw
 */
static void ishtp_hbm_fw_disconnect_req(struct ishtp_device *dev,
	struct hbm_client_connect_request *disconnect_req)
{
	struct ishtp_cl *cl, *next;
	const size_t len = sizeof(struct hbm_client_connect_response);
	unsigned long	flags;
	struct ishtp_msg_hdr hdr;
	unsigned char data[4];	/* All HBM messages are 4 bytes */

	spin_lock_irqsave(&dev->cl_list_lock, flags);
	list_for_each_entry_safe(cl, next, &dev->cl_list, link) {
		if (ishtp_hbm_cl_addr_equal(cl, disconnect_req)) {
			cl->state = ISHTP_CL_DISCONNECTED;

			/* send disconnect response */
			ishtp_hbm_hdr(&hdr, len);
			ishtp_hbm_cl_hdr(cl, CLIENT_DISCONNECT_RES_CMD, data,
				len);
			ishtp_write_message(dev, &hdr, data);
			break;
		}
	}
	spin_unlock_irqrestore(&dev->cl_list_lock, flags);
}

/*
 * get_dma_send_buf - find and return free address of "size" bytes
 * in dma tx buffer.
 * the function will mark this address as "in-used" memory
 *
 * returns NULL when no free buffer is available
 */
void *get_dma_send_buf(struct ishtp_device *dev, uint32_t size)
{
	unsigned long	flags;
	int i, j, free;
	/* additional slot is needed if there is rem */
	int required_slots = (size / DMA_SLOT_SIZE)
		+ 1 * (size % DMA_SLOT_SIZE != 0);

	spin_lock_irqsave(&dma_tx_lock, flags);
	for (i = 0; i <= (dma_num_slots - required_slots); i++) {
		free = 1;
		for (j = 0; j < required_slots; j++)
			if (dma_tx_map[i+j]) {
				free = 0;
				i += j;
				break;
			}
		if (free) {
			/* mark memory as "caught" */
			for (j = 0; j < required_slots; j++)
				dma_tx_map[i+j] = 1;
			spin_unlock_irqrestore(&dma_tx_lock, flags);
			return (i * DMA_SLOT_SIZE) +
				(unsigned char *)host_dma_tx_buf;
		}
	}
	spin_unlock_irqrestore(&dma_tx_lock, flags);
	dev_err(dev->devc, "No free DMA buffer to send msg\n");
	return NULL;
}

/*
 * release_dma_acked_mem - returnes the acked memory to free list.
 * (from msg_addr, size bytes long)
 */
void release_dma_acked_mem(struct ishtp_device *dev, void *msg_addr,
	uint8_t size)
{
	unsigned long	flags;
	int acked_slots = (size / DMA_SLOT_SIZE)
		+ 1 * (size % DMA_SLOT_SIZE != 0);
	int i, j;

	if ((msg_addr - host_dma_tx_buf) % DMA_SLOT_SIZE) {
		dev_err(dev->devc, "Bad DMA Tx ack address\n");
		return;
	}

	i = (msg_addr - host_dma_tx_buf) / DMA_SLOT_SIZE;
	spin_lock_irqsave(&dma_tx_lock, flags);
	for (j = 0; j < acked_slots; j++) {
		if ((i + j) >= dma_num_slots || !dma_tx_map[i+j]) {
			/* no such slot, or memory is already free */
			spin_unlock_irqrestore(&dma_tx_lock, flags);
			dev_err(dev->devc, "Bad DMA Tx ack address\n");
			return;
		}
		dma_tx_map[i+j] = 0;
	}
	spin_unlock_irqrestore(&dma_tx_lock, flags);
}

/*
 * ishtp_hbm_dma_xfer_ack - receive ack for ISHTP-over-DMA client message
 *
 * Constraint:
 * First implementation is one ISHTP message per DMA transfer
 */
void ishtp_hbm_dma_xfer_ack(struct ishtp_device *dev,
	struct dma_xfer_hbm *dma_xfer)
{
	void	*msg;
	uint64_t	offs;
	struct ishtp_msg_hdr	*ishtp_hdr =
		(struct ishtp_msg_hdr *)&dev->ishtp_msg_hdr;
	unsigned	msg_offs;
	struct ishtp_cl *cl, *next;

	for (msg_offs = 0; msg_offs < ishtp_hdr->length;
		msg_offs += sizeof(struct dma_xfer_hbm)) {
		offs = dma_xfer->msg_addr - host_dma_tx_buf_phys;
		if (offs > host_dma_tx_buf_size) {
			dev_err(dev->devc, "Bad DMA Tx ack message address\n");
			return;
		}
		if (dma_xfer->msg_length > host_dma_tx_buf_size - offs) {
			dev_err(dev->devc, "Bad DMA Tx ack message size\n");
			return;
		}

		/* logical address of the acked mem */
		msg = (unsigned char *)host_dma_tx_buf + offs;
		release_dma_acked_mem(dev, msg, dma_xfer->msg_length);

		list_for_each_entry_safe(cl, next, &dev->cl_list, link) {
			if (cl->fw_client_id == dma_xfer->fw_client_id &&
					cl->host_client_id ==
						dma_xfer->host_client_id)
				/* in case that a single ack may be sent
				over several dma transfers, and the last msg
				addr was inside the acked memory, but not in
				its start */
				if (cl->last_dma_addr >= (unsigned char *)msg &&
						cl->last_dma_addr <
						(unsigned char *)msg +
						dma_xfer->msg_length) {
					cl->last_dma_acked = 1;

					if (!list_empty(&cl->tx_list.list) &&
						cl->ishtp_flow_ctrl_creds) {
						/*
						 * start sending the first msg
						 */
						ishtp_cl_send_msg(dev, cl);
					}
				}
		}
		++dma_xfer;
	}
}

/* ishtp_hbm_dma_xfer - receive ISHTP-over-DMA client message */
void ishtp_hbm_dma_xfer(struct ishtp_device *dev, struct dma_xfer_hbm *dma_xfer)
{
	void	*msg;
	uint64_t	offs;
	struct ishtp_msg_hdr	hdr;
	struct ishtp_msg_hdr	*ishtp_hdr =
		(struct ishtp_msg_hdr *) &dev->ishtp_msg_hdr;
	struct dma_xfer_hbm	*prm = dma_xfer;
	unsigned	msg_offs;

	for (msg_offs = 0; msg_offs < ishtp_hdr->length;
		msg_offs += sizeof(struct dma_xfer_hbm)) {

		offs = dma_xfer->msg_addr - host_dma_rx_buf_phys;
		if (offs > host_dma_rx_buf_size) {
			dev_err(dev->devc, "Bad DMA Rx message address\n");
			return;
		}
		if (dma_xfer->msg_length > host_dma_rx_buf_size - offs) {
			dev_err(dev->devc, "Bad DMA Rx message size\n");
			return;
		}
		msg = host_dma_rx_buf + offs;
		recv_ishtp_cl_msg_dma(dev, msg, dma_xfer);
		dma_xfer->hbm = DMA_XFER_ACK;	/* Prepare for response */
		++dma_xfer;
	}

	/* Send DMA_XFER_ACK [...] */
	ishtp_hbm_hdr(&hdr, ishtp_hdr->length);
	ishtp_write_message(dev, &hdr, (unsigned char *)prm);
}

/**
 * ishtp_hbm_dispatch - bottom half read routine after ISR to
 * handle the read bus message cmd processing.
 */
void ishtp_hbm_dispatch(struct ishtp_device *dev, struct ishtp_bus_message *hdr)
{
	struct ishtp_bus_message *ishtp_msg;
	struct ishtp_fw_client *fw_client;
	struct hbm_host_version_response *version_res;
	struct hbm_client_connect_response *connect_res;
	struct hbm_client_connect_response *disconnect_res;
	struct hbm_client_connect_request *disconnect_req;
	struct hbm_props_response *props_res;
	struct hbm_host_enum_response *enum_res;
	struct ishtp_msg_hdr ishtp_hdr;
	struct dma_alloc_notify	dma_alloc_notify;
	struct dma_xfer_hbm	*dma_xfer;

	ishtp_msg = hdr;

	switch (ishtp_msg->hbm_cmd) {
	case HOST_START_RES_CMD:
		version_res = (struct hbm_host_version_response *)ishtp_msg;
		if (!version_res->host_version_supported) {
			dev->version = version_res->fw_max_version;

			dev->hbm_state = ISHTP_HBM_STOPPED;
			ishtp_hbm_stop_req(dev);
			return;
		}

		dev->version.major_version = HBM_MAJOR_VERSION;
		dev->version.minor_version = HBM_MINOR_VERSION;
		if (dev->dev_state == ISHTP_DEV_INIT_CLIENTS &&
				dev->hbm_state == ISHTP_HBM_START) {
			dev->hbm_state = ISHTP_HBM_STARTED;
			ishtp_hbm_enum_clients_req(dev);
		} else {
			dev_err(dev->devc, "reset: wrong host start response\n");
			/* BUG: why do we arrive here? */
			ish_hw_reset(dev);
			return;
		}

		wake_up(&dev->wait_hbm_recvd_msg);
		break;

	case CLIENT_CONNECT_RES_CMD:
		connect_res = (struct hbm_client_connect_response *)ishtp_msg;
		ishtp_hbm_cl_connect_res(dev, connect_res);
		break;

	case CLIENT_DISCONNECT_RES_CMD:
		disconnect_res =
			(struct hbm_client_connect_response *)ishtp_msg;
		ishtp_hbm_cl_disconnect_res(dev, disconnect_res);
		break;

	case HOST_CLIENT_PROPERTIES_RES_CMD:
		props_res = (struct hbm_props_response *)ishtp_msg;
		fw_client = &dev->fw_clients[dev->fw_client_presentation_num];

		if (props_res->status || !dev->fw_clients) {
			dev_err(dev->devc, "reset: properties response hbm "
				"wrong status\n");
			ish_hw_reset(dev);
			return;
		}

		if (fw_client->client_id != props_res->address) {
			dev_err(dev->devc,
				"reset: host properties response address "
				"mismatch [%02X %02X]\n",
				fw_client->client_id, props_res->address);
			ish_hw_reset(dev);
			return;
		}

		if (dev->dev_state != ISHTP_DEV_INIT_CLIENTS ||
			dev->hbm_state != ISHTP_HBM_CLIENT_PROPERTIES) {
			dev_err(dev->devc,
				"reset: unexpected properties response\n");
			ish_hw_reset(dev);
			return;
		}

		fw_client->props = props_res->client_properties;
		dev->fw_client_index++;
		dev->fw_client_presentation_num++;

		/* request property for the next client */
		ishtp_hbm_prop_req(dev);

		if (dev->dev_state != ISHTP_DEV_ENABLED)
			break;

		ishtp_cl_alloc_dma_buf();
		if (host_dma_rx_buf) {
			const size_t len = sizeof(dma_alloc_notify);

			memset(&dma_alloc_notify, 0, sizeof(dma_alloc_notify));
			dma_alloc_notify.hbm = DMA_BUFFER_ALLOC_NOTIFY;
			dma_alloc_notify.buf_size = host_dma_rx_buf_size;
			dma_alloc_notify.buf_address = host_dma_rx_buf_phys;
			ishtp_hbm_hdr(&ishtp_hdr, len);
			ishtp_write_message(dev, &ishtp_hdr,
				(unsigned char *)&dma_alloc_notify);
		}

		break;

	case HOST_ENUM_RES_CMD:
		enum_res = (struct hbm_host_enum_response *) ishtp_msg;
		memcpy(dev->fw_clients_map, enum_res->valid_addresses, 32);
		if (dev->dev_state == ISHTP_DEV_INIT_CLIENTS &&
			dev->hbm_state == ISHTP_HBM_ENUM_CLIENTS) {
				dev->fw_client_presentation_num = 0;
				dev->fw_client_index = 0;

				ishtp_hbm_fw_cl_allocate(dev);
				dev->hbm_state = ISHTP_HBM_CLIENT_PROPERTIES;

				/* first property request */
				ishtp_hbm_prop_req(dev);
		} else {
			dev_err(dev->devc,
				"reset: unexpected enumeration response hbm\n");
			ish_hw_reset(dev);
			return;
		}
		break;

	case HOST_STOP_RES_CMD:
		if (dev->hbm_state != ISHTP_HBM_STOPPED)
			dev_err(dev->devc, "unexpected stop response\n");

		dev->dev_state = ISHTP_DEV_DISABLED;
		dev_info(dev->devc, "reset: FW stop response\n");
		ish_hw_reset(dev);
		break;

	case CLIENT_DISCONNECT_REQ_CMD:
		/* search for client */
		disconnect_req = (struct hbm_client_connect_request *)ishtp_msg;
		ishtp_hbm_fw_disconnect_req(dev, disconnect_req);
		break;

	case FW_STOP_REQ_CMD:
		dev->hbm_state = ISHTP_HBM_STOPPED;
		break;

	case DMA_BUFFER_ALLOC_RESPONSE:
		host_dma_enabled = 1;
		break;

	case DMA_XFER:
		dma_xfer = (struct dma_xfer_hbm *)ishtp_msg;
		if (!host_dma_enabled) {
			dev_err(dev->devc,
				"DMA XFER requested but DMA is not enabled\n");
			break;
		}
		ishtp_hbm_dma_xfer(dev, dma_xfer);
		break;

	case DMA_XFER_ACK:
		dma_xfer = (struct dma_xfer_hbm *)ishtp_msg;
		if (!host_dma_enabled || !host_dma_tx_buf) {
			dev_err(dev->devc,
				"DMA XFER acked but DMA Tx is not enabled\n");
			break;
		}
		ishtp_hbm_dma_xfer_ack(dev, dma_xfer);
		break;

	default:
		/*BUG();*/
		dev_err(dev->devc, "unknown HBM: %u\n",
			(unsigned)ishtp_msg->hbm_cmd);

		break;
	}
}

/*
 * BH processing work function (instead of thread handler)
 * for processing hbm messages
 */
void	bh_hbm_work_fn(struct work_struct *work)
{
	unsigned long	flags;
	struct ishtp_device	*dev;
	unsigned char	hbm[IPC_PAYLOAD_SIZE];

	dev = container_of(work, struct ishtp_device, bh_hbm_work);
	spin_lock_irqsave(&dev->rd_msg_spinlock, flags);
	if (dev->rd_msg_fifo_head != dev->rd_msg_fifo_tail) {
		memcpy(hbm, dev->rd_msg_fifo + dev->rd_msg_fifo_head,
			IPC_PAYLOAD_SIZE);
		dev->rd_msg_fifo_head =
			(dev->rd_msg_fifo_head + IPC_PAYLOAD_SIZE) %
			(RD_INT_FIFO_SIZE * IPC_PAYLOAD_SIZE);
		spin_unlock_irqrestore(&dev->rd_msg_spinlock, flags);
		ishtp_hbm_dispatch(dev, (struct ishtp_bus_message *)hbm);
	} else {
		spin_unlock_irqrestore(&dev->rd_msg_spinlock, flags);
	}
}

/*
 *	Receive and process ISHTP bus messages
 *
 *	(!) ISR context
 */
void	recv_hbm(struct ishtp_device *dev, struct ishtp_msg_hdr *ishtp_hdr)
{
	uint8_t	rd_msg_buf[ISHTP_RD_MSG_BUF_SIZE];
	struct ishtp_bus_message	*ishtp_msg =
		(struct ishtp_bus_message *)rd_msg_buf;
	unsigned long	flags;

	dev->ops->ishtp_read(dev, rd_msg_buf, ishtp_hdr->length);

	/* Flow control - handle in place */
	if (ishtp_msg->hbm_cmd == ISHTP_FLOW_CONTROL_CMD) {
		struct hbm_flow_control *flow_control =
			(struct hbm_flow_control *)ishtp_msg;
		struct ishtp_cl *cl = NULL;
		struct ishtp_cl *next = NULL;
		unsigned long	flags, tx_flags;

		spin_lock_irqsave(&dev->cl_list_lock, flags);
		list_for_each_entry_safe(cl, next, &dev->cl_list, link) {
			if (cl->host_client_id == flow_control->host_addr &&
					cl->fw_client_id ==
					flow_control->fw_addr) {
				/*
				 * NOTE: It's valid only for counting
				 * flow-control implementation to receive a
				 * FC in the middle of sending. Meanwhile not
				 * supported
				 */
				if (cl->ishtp_flow_ctrl_creds)
					dev_err(dev->devc,
						"recv extra FC from FW client "
						"%u (host client %u) "
						"(FC count was %u)\n",
						(unsigned)cl->fw_client_id,
						(unsigned)cl->host_client_id,
						(unsigned)cl->ishtp_flow_ctrl_creds);
				else {
					++cl->ishtp_flow_ctrl_creds;
					++cl->ishtp_flow_ctrl_cnt;
					cl->last_ipc_acked = 1;
					spin_lock_irqsave(&cl->tx_list_spinlock,
						tx_flags);
					if (!list_empty(&cl->tx_list.list)) {
						/*
						 * start sending the first msg
						 *	= the callback function
						 */
						spin_unlock_irqrestore(
							&cl->tx_list_spinlock,
							tx_flags);
						ishtp_cl_send_msg(dev, cl);
					} else {
						spin_unlock_irqrestore(
							&cl->tx_list_spinlock,
							tx_flags);
					}
				}
				break;
			}
		}
		spin_unlock_irqrestore(&dev->cl_list_lock, flags);
		goto	eoi;
	}

	/*
	 * Some messages that are safe for ISR processing and important
	 * to be done "quickly" and in-order, go here
	 */
	if (ishtp_msg->hbm_cmd == CLIENT_CONNECT_RES_CMD ||
			ishtp_msg->hbm_cmd == CLIENT_DISCONNECT_RES_CMD ||
			ishtp_msg->hbm_cmd == CLIENT_DISCONNECT_REQ_CMD ||
			ishtp_msg->hbm_cmd == DMA_XFER) {
		ishtp_hbm_dispatch(dev, ishtp_msg);
		goto	eoi;
	}

	/*
	 * All other HBMs go here.
	 * We schedule HBMs for processing serially by using system wq,
	 * possibly there will be multiple HBMs scheduled at the same time.
	 */
	spin_lock_irqsave(&dev->rd_msg_spinlock, flags);
	if ((dev->rd_msg_fifo_tail + IPC_PAYLOAD_SIZE) %
			(RD_INT_FIFO_SIZE * IPC_PAYLOAD_SIZE) ==
			dev->rd_msg_fifo_head) {
		spin_unlock_irqrestore(&dev->rd_msg_spinlock, flags);
		dev_err(dev->devc, "BH buffer overflow, dropping HBM %u\n",
			(unsigned)ishtp_msg->hbm_cmd);
		goto	eoi;
	}
	memcpy(dev->rd_msg_fifo + dev->rd_msg_fifo_tail, ishtp_msg,
		ishtp_hdr->length);
	dev->rd_msg_fifo_tail = (dev->rd_msg_fifo_tail + IPC_PAYLOAD_SIZE) %
		(RD_INT_FIFO_SIZE * IPC_PAYLOAD_SIZE);
	spin_unlock_irqrestore(&dev->rd_msg_spinlock, flags);
	schedule_work(&dev->bh_hbm_work);
eoi:
	return;
}

/*
 * Receive and process ISHTP fixed client messages
 *
 * (!) ISR context
 */
void recv_fixed_cl_msg(struct ishtp_device *dev,
	struct ishtp_msg_hdr *ishtp_hdr)
{
	uint8_t rd_msg_buf[ISHTP_RD_MSG_BUF_SIZE];

	dev->print_log(dev,
		"%s() got fixed client msg from client #%d\n",
		__func__, ishtp_hdr->fw_addr);
	dev->ops->ishtp_read(dev, rd_msg_buf, ishtp_hdr->length);
	if (ishtp_hdr->fw_addr == ISHTP_SYSTEM_STATE_CLIENT_ADDR) {
		struct ish_system_states_header *msg_hdr =
			(struct ish_system_states_header *)rd_msg_buf;
		if (msg_hdr->cmd == SYSTEM_STATE_SUBSCRIBE)
			send_resume(dev);	/* if FW request arrived here,
						the system is not suspended */
		else
			dev_err(dev->devc, "unknown fixed client msg [%02X]\n",
				msg_hdr->cmd);
	}
}

static inline void fix_cl_hdr(struct ishtp_msg_hdr *hdr, size_t length,
	uint8_t cl_addr)
{
	hdr->host_addr = 0;
	hdr->fw_addr = cl_addr;
	hdr->length = length;
	hdr->msg_complete = 1;
	hdr->reserved = 0;
}

/*** Suspend and resume notification ***/

/* Global var for suspend & resume */
uint32_t current_state = 0;
uint32_t supported_states = 0 | SUSPEND_STATE_BIT;

void send_suspend(struct ishtp_device *dev)
{
	struct ishtp_msg_hdr	ishtp_hdr;
	struct ish_system_states_status state_status_msg;
	const size_t len = sizeof(struct ish_system_states_status);

	fix_cl_hdr(&ishtp_hdr, len, ISHTP_SYSTEM_STATE_CLIENT_ADDR);

	memset(&state_status_msg, 0, len);
	state_status_msg.hdr.cmd = SYSTEM_STATE_STATUS;
	state_status_msg.supported_states = supported_states;
	current_state |= SUSPEND_STATE_BIT;
	dev->print_log(dev, "%s() sends SUSPEND notification\n", __func__);
	state_status_msg.states_status = current_state;

	ishtp_write_message(dev, &ishtp_hdr,
		(unsigned char *)&state_status_msg);
}
EXPORT_SYMBOL(send_suspend);

void send_resume(struct ishtp_device *dev)
{
	struct ishtp_msg_hdr	ishtp_hdr;
	struct ish_system_states_status state_status_msg;
	const size_t len = sizeof(struct ish_system_states_status);

	fix_cl_hdr(&ishtp_hdr, len, ISHTP_SYSTEM_STATE_CLIENT_ADDR);

	memset(&state_status_msg, 0, len);
	state_status_msg.hdr.cmd = SYSTEM_STATE_STATUS;
	state_status_msg.supported_states = supported_states;
	current_state &= ~SUSPEND_STATE_BIT;
	dev->print_log(dev, "%s() sends RESUME notification\n", __func__);
	state_status_msg.states_status = current_state;

	ishtp_write_message(dev, &ishtp_hdr,
		(unsigned char *)&state_status_msg);
}
EXPORT_SYMBOL(send_resume);

void query_subscribers(struct ishtp_device *dev)
{
	struct ishtp_msg_hdr	ishtp_hdr;
	struct ish_system_states_query_subscribers query_subscribers_msg;
	const size_t len = sizeof(struct ish_system_states_query_subscribers);

	fix_cl_hdr(&ishtp_hdr, len, ISHTP_SYSTEM_STATE_CLIENT_ADDR);

	memset(&query_subscribers_msg, 0, len);
	query_subscribers_msg.hdr.cmd = SYSTEM_STATE_QUERY_SUBSCRIBERS;

	ishtp_write_message(dev, &ishtp_hdr,
		(unsigned char *)&query_subscribers_msg);
}

