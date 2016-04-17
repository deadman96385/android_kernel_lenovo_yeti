/*
 * ISHTP client logic (for both ISHTP bus driver and user-mode API)
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
#include <linux/delay.h>
#include "hbm.h"
#include "client.h"

/* dma global vars */
int	host_dma_enabled;

void	*host_dma_tx_buf;
unsigned	host_dma_tx_buf_size = (1024*1024);
uint64_t	host_dma_tx_buf_phys;

int	dma_num_slots;
/* map of 4k blocks in Tx dma buf: 0-free, 1-caught */
uint8_t	*dma_tx_map;
spinlock_t	dma_tx_lock;

void	*host_dma_rx_buf;
unsigned	host_dma_rx_buf_size = (1024*1024);
uint64_t	host_dma_rx_buf_phys;

void	ishtp_cl_alloc_dma_buf(void)
{
	int	order;
	unsigned	temp;

	/* Allocate Tx buffer and init usage bitmap */
	for (order = 0, temp = host_dma_tx_buf_size / PAGE_SIZE + 1; temp;
			temp >>= 1)
		++order;
	host_dma_tx_buf = (void *)__get_free_pages(GFP_KERNEL, order);
	if (host_dma_tx_buf)
		host_dma_tx_buf_phys = __pa(host_dma_tx_buf);

	dma_num_slots = host_dma_tx_buf_size / DMA_SLOT_SIZE;
	dma_tx_map = kzalloc(dma_num_slots * sizeof(uint8_t), GFP_KERNEL);
	spin_lock_init(&dma_tx_lock);

	for (order = 0, temp = host_dma_rx_buf_size / PAGE_SIZE + 1; temp;
			temp >>= 1)
		++order;
	host_dma_rx_buf = (void *)__get_free_pages(GFP_KERNEL, order);
	if (host_dma_rx_buf)
		host_dma_rx_buf_phys = __pa(host_dma_rx_buf);
}

/* ishtp_read_list_flush - removes list entry belonging to cl */
void ishtp_read_list_flush(struct ishtp_cl *cl)
{
	struct ishtp_cl_rb *rb;
	struct ishtp_cl_rb *next;
	unsigned long	flags;

	spin_lock_irqsave(&cl->dev->read_list_spinlock, flags);
	list_for_each_entry_safe(rb, next, &cl->dev->read_list.list, list) {
		if (rb->cl && ishtp_cl_cmp_id(cl, rb->cl)) {
			list_del(&rb->list);
			ishtp_io_rb_free(rb);
		}
	}
	spin_unlock_irqrestore(&cl->dev->read_list_spinlock, flags);
}

/* ishtp_io_rb_free - free ishtp_rb_private related memory */
void ishtp_io_rb_free(struct ishtp_cl_rb *rb)
{
	if (rb == NULL)
		return;
	kfree(rb->buffer.data);
	kfree(rb);
}

/**
 * ishtp_io_rb_init - allocate and initialize request block
 *
 * returns ishtp_cl_rb pointer or NULL;
 */
struct ishtp_cl_rb *ishtp_io_rb_init(struct ishtp_cl *cl)
{
	struct ishtp_cl_rb *rb;

	rb = kzalloc(sizeof(struct ishtp_cl_rb), GFP_KERNEL);
	if (!rb)
		return NULL;

	INIT_LIST_HEAD(&rb->list);
	rb->cl = cl;
	rb->buf_idx = 0;
	return rb;
}

/* ishtp_io_rb_alloc_buf - allocate respose buffer */
int ishtp_io_rb_alloc_buf(struct ishtp_cl_rb *rb, size_t length)
{
	if (!rb)
		return -EINVAL;

	if (length == 0)
		return 0;

	rb->buffer.data = kmalloc(length, GFP_KERNEL);
	if (!rb->buffer.data)
		return -ENOMEM;
	rb->buffer.size = length;
	return 0;
}

/*
 * ishtp_io_rb_recycle - re-append rb to its client's free list
 * and send flow control if needed
 */
int ishtp_io_rb_recycle(struct ishtp_cl_rb *rb)
{
	struct ishtp_cl *cl;
	int	rets = 0;
	unsigned long	flags;

	if (!rb || !rb->cl)
		return	-EFAULT;

	cl = rb->cl;
	spin_lock_irqsave(&cl->free_list_spinlock, flags);
	list_add_tail(&rb->list, &cl->free_rb_list.list);
	spin_unlock_irqrestore(&cl->free_list_spinlock, flags);

	/*
	 * If we returned the first buffer to empty 'free' list,
	 * send flow control
	 */
	if (!cl->out_flow_ctrl_creds)
		rets = ishtp_cl_read_start(cl);

	return	rets;
}
EXPORT_SYMBOL(ishtp_io_rb_recycle);

/* ishtp_cl_flush_queues - flushes queue lists belonging to cl */
int ishtp_cl_flush_queues(struct ishtp_cl *cl)
{
	if (WARN_ON(!cl || !cl->dev))
		return -EINVAL;
	ishtp_read_list_flush(cl);

	return 0;
}
EXPORT_SYMBOL(ishtp_cl_flush_queues);

/* ishtp_cl_init - initializes cl */
void ishtp_cl_init(struct ishtp_cl *cl, struct ishtp_device *dev)
{
	memset(cl, 0, sizeof(struct ishtp_cl));
	init_waitqueue_head(&cl->wait);
	init_waitqueue_head(&cl->rx_wait);
	init_waitqueue_head(&cl->wait_ctrl_res);
	spin_lock_init(&cl->free_list_spinlock);
	spin_lock_init(&cl->in_process_spinlock);
	spin_lock_init(&cl->tx_list_spinlock);
	spin_lock_init(&cl->tx_free_list_spinlock);
	spin_lock_init(&cl->fc_spinlock);
	INIT_LIST_HEAD(&cl->link);
	cl->dev = dev;

	INIT_LIST_HEAD(&cl->free_rb_list.list);
	INIT_LIST_HEAD(&cl->tx_list.list);
	INIT_LIST_HEAD(&cl->tx_free_list.list);
	INIT_LIST_HEAD(&cl->in_process_list.list);

	cl->rx_ring_size = CL_DEF_RX_RING_SIZE;
	cl->tx_ring_size = CL_DEF_TX_RING_SIZE;

	/* dma */
	/* CHANGEME: will change to "default" once validated */
	cl->last_tx_path = CL_TX_PATH_IPC;
	cl->last_dma_acked = 1;
	cl->last_dma_addr = NULL;
	cl->last_ipc_acked = 1;
}

int	ishtp_cl_free_rx_ring(struct ishtp_cl *cl)
{
	struct ishtp_cl_rb *rb;
	unsigned long	flags;

	/* release allocated memory - pass over free_rb_list */
	spin_lock_irqsave(&cl->free_list_spinlock, flags);
	while (!list_empty(&cl->free_rb_list.list)) {
		rb = list_entry(cl->free_rb_list.list.next, struct ishtp_cl_rb,
			list);
		list_del(&rb->list);
		kfree(rb->buffer.data);
		kfree(rb);
	}
	spin_unlock_irqrestore(&cl->free_list_spinlock, flags);
	/* release allocated memory - pass over in_process_list */
	spin_lock_irqsave(&cl->in_process_spinlock, flags);
	while (!list_empty(&cl->in_process_list.list)) {
		rb = list_entry(cl->in_process_list.list.next,
			struct ishtp_cl_rb, list);
		list_del(&rb->list);
		kfree(rb->buffer.data);
		kfree(rb);
	}
	spin_unlock_irqrestore(&cl->in_process_spinlock, flags);
	return	0;
}

int	ishtp_cl_free_tx_ring(struct ishtp_cl *cl)
{
	struct ishtp_cl_tx_ring	*tx_buf;
	unsigned long	flags;

	spin_lock_irqsave(&cl->tx_free_list_spinlock, flags);
	/* release allocated memory - pass over tx_free_list */
	while (!list_empty(&cl->tx_free_list.list)) {
		tx_buf = list_entry(cl->tx_free_list.list.next,
			struct ishtp_cl_tx_ring, list);
		list_del(&tx_buf->list);
		kfree(tx_buf->send_buf.data);
		kfree(tx_buf);
	}
	spin_unlock_irqrestore(&cl->tx_free_list_spinlock, flags);

	spin_lock_irqsave(&cl->tx_list_spinlock, flags);
	/* release allocated memory - pass over tx_list */
	while (!list_empty(&cl->tx_list.list)) {
		tx_buf = list_entry(cl->tx_list.list.next,
			struct ishtp_cl_tx_ring, list);
		list_del(&tx_buf->list);
		kfree(tx_buf->send_buf.data);
		kfree(tx_buf);
	}
	spin_unlock_irqrestore(&cl->tx_list_spinlock, flags);

	return	0;
}

int	ishtp_cl_alloc_rx_ring(struct ishtp_cl *cl)
{
	size_t	len = cl->device->fw_client->props.max_msg_length;
	int	j;
	struct ishtp_cl_rb *rb;
	int	ret = 0;
	unsigned long	flags;

	for (j = 0; j < cl->rx_ring_size; ++j) {
		rb = ishtp_io_rb_init(cl);
		if (!rb) {
			ret = -ENOMEM;
			goto out;
		}
		ret = ishtp_io_rb_alloc_buf(rb, len);
		if (ret)
			goto out;
		spin_lock_irqsave(&cl->free_list_spinlock, flags);
		list_add_tail(&rb->list, &cl->free_rb_list.list);
		spin_unlock_irqrestore(&cl->free_list_spinlock, flags);
	}

	return	0;

out:
	dev_err(&cl->device->dev, "error in allocating Rx buffers\n");
	ishtp_cl_free_rx_ring(cl);
	return	ret;
}

int	ishtp_cl_alloc_tx_ring(struct ishtp_cl *cl)
{
	size_t	len = cl->device->fw_client->props.max_msg_length;
	int	j;
	unsigned long	flags;

	/* Allocate pool to free Tx bufs */
	for (j = 0; j < cl->tx_ring_size; ++j) {
		struct ishtp_cl_tx_ring	*tx_buf;

		tx_buf = kmalloc(sizeof(struct ishtp_cl_tx_ring), GFP_KERNEL);
		if (!tx_buf)
			goto	out;

		memset(tx_buf, 0, sizeof(struct ishtp_cl_tx_ring));
		tx_buf->send_buf.data = kmalloc(len, GFP_KERNEL);
		if (!tx_buf->send_buf.data) {
			kfree(tx_buf);
			goto	out;
		}

		spin_lock_irqsave(&cl->tx_free_list_spinlock, flags);
		list_add_tail(&tx_buf->list, &cl->tx_free_list.list);
		spin_unlock_irqrestore(&cl->tx_free_list_spinlock, flags);
	}
	return	0;
out:
	dev_err(&cl->device->dev, "error in allocating Tx pool\n");
	ishtp_cl_free_rx_ring(cl);
	return	-ENOMEM;
}

/**
 * ishtp_cl_allocate - allocates cl structure and sets it up.
 * returns The allocated file or NULL on failure
 */
struct ishtp_cl *ishtp_cl_allocate(struct ishtp_device *dev)
{
	struct ishtp_cl *cl;

	cl = kmalloc(sizeof(struct ishtp_cl), GFP_ATOMIC);
	if (!cl)
		return NULL;

	ishtp_cl_init(cl, dev);
	return cl;
}
EXPORT_SYMBOL(ishtp_cl_allocate);

void	ishtp_cl_free(struct ishtp_cl *cl)
{
	struct ishtp_device *dev;
	unsigned long flags;

	if (!cl)
		return;
	dev = cl->dev;
	if (!dev)
		return;

	spin_lock_irqsave(&dev->cl_list_lock, flags);
	ishtp_cl_free_rx_ring(cl);
	ishtp_cl_free_tx_ring(cl);
	kfree(cl);
	spin_unlock_irqrestore(&dev->cl_list_lock, flags);
}
EXPORT_SYMBOL(ishtp_cl_free);

/**
 * ishtp_cl_find_read_rb - find this cl's callback in the read list
 * returns rb on success, NULL on error
 */
struct ishtp_cl_rb *ishtp_cl_find_read_rb(struct ishtp_cl *cl)
{
	struct ishtp_device *dev = cl->dev;
	struct ishtp_cl_rb *rb = NULL;
	struct ishtp_cl_rb *next = NULL;
	unsigned long	dev_flags;

	spin_lock_irqsave(&dev->read_list_spinlock, dev_flags);
	list_for_each_entry_safe(rb, next, &dev->read_list.list, list)
		if (ishtp_cl_cmp_id(cl, rb->cl)) {
			spin_unlock_irqrestore(&dev->read_list_spinlock,
				dev_flags);
			return rb;
		}
	spin_unlock_irqrestore(&dev->read_list_spinlock, dev_flags);
	return NULL;
}

/**
 * ishtp_cl_link: allocate host id in the host map
 * @id - fixed host id or (-1) for generating one
 */
int ishtp_cl_link(struct ishtp_cl *cl, int id)
{
	struct ishtp_device *dev;
	unsigned long	flags, flags_cl;

	if (WARN_ON(!cl || !cl->dev))
		return -EINVAL;

	dev = cl->dev;

	spin_lock_irqsave(&dev->device_lock, flags);

	if (dev->open_handle_count >= ISHTP_MAX_OPEN_HANDLE_COUNT) {
		spin_unlock_irqrestore(&dev->device_lock, flags);
		return	-EMFILE;
	}

	/* If Id is not assigned get one*/
	if (id == ISHTP_HOST_CLIENT_ID_ANY)
		id = find_first_zero_bit(dev->host_clients_map,
			ISHTP_CLIENTS_MAX);

	if (id >= ISHTP_CLIENTS_MAX) {
		spin_unlock_irqrestore(&dev->device_lock, flags);
		dev_err(&cl->device->dev, "id exceded %d", ISHTP_CLIENTS_MAX);
		return -ENOENT;
	}

	dev->open_handle_count++;
	cl->host_client_id = id;
	spin_lock_irqsave(&dev->cl_list_lock, flags_cl);
	if (dev->dev_state != ISHTP_DEV_ENABLED) {
		spin_unlock_irqrestore(&dev->cl_list_lock, flags_cl);
		spin_unlock_irqrestore(&dev->device_lock, flags);
		return -ENODEV;
	}
	list_add_tail(&cl->link, &dev->cl_list);
	spin_unlock_irqrestore(&dev->cl_list_lock, flags_cl);
	set_bit(id, dev->host_clients_map);
	cl->state = ISHTP_CL_INITIALIZING;
	spin_unlock_irqrestore(&dev->device_lock, flags);

	return 0;
}
EXPORT_SYMBOL(ishtp_cl_link);

/* ishtp_cl_unlink - remove fw_cl from the list */
int ishtp_cl_unlink(struct ishtp_cl *cl)
{
	struct ishtp_device *dev;
	struct ishtp_cl *pos, *next;
	unsigned long	flags;

	/* don't shout on error exit path */
	if (!cl || !cl->dev)
		return 0;

	dev = cl->dev;

	spin_lock_irqsave(&dev->device_lock, flags);
	if (dev->open_handle_count > 0) {
		clear_bit(cl->host_client_id, dev->host_clients_map);
		dev->open_handle_count--;
	}
	spin_unlock_irqrestore(&dev->device_lock, flags);

	/*
	 * This checks that 'cl' is actually linked into device's structure,
	 * before attempting 'list_del'
	 */
	spin_lock_irqsave(&dev->cl_list_lock, flags);
	list_for_each_entry_safe(pos, next, &dev->cl_list, link) {
		if (cl->host_client_id == pos->host_client_id) {
			list_del_init(&pos->link);
			break;
		}
	}
	spin_unlock_irqrestore(&dev->cl_list_lock, flags);

	return 0;
}
EXPORT_SYMBOL(ishtp_cl_unlink);

/* ishtp_cl_disconnect - disconnect host client form the fw one */
int ishtp_cl_disconnect(struct ishtp_cl *cl)
{
	struct ishtp_device *dev;
	int err;

	if (WARN_ON(!cl || !cl->dev))
		return -ENODEV;

	dev = cl->dev;

	if (cl->state != ISHTP_CL_DISCONNECTING)
		return 0;

	if (ishtp_hbm_cl_disconnect_req(dev, cl)) {
		dev_err(&cl->device->dev, "failed to disconnect.\n");
		return -ENODEV;
	}

	err = wait_event_timeout(cl->wait_ctrl_res,
			(dev->dev_state != ISHTP_DEV_ENABLED ||
			ISHTP_CL_DISCONNECTED == cl->state),
			ishtp_secs_to_jiffies(ISHTP_CL_CONNECT_TIMEOUT));

	/*
	 * If FW reset arrived, this will happen. Don't check cl->,
	 * as 'cl' may be freed already
	 */
	if (dev->dev_state != ISHTP_DEV_ENABLED)
		return -ENODEV;

	if (ISHTP_CL_DISCONNECTED == cl->state)
		return 0;
	return -ENODEV;
}

/**
 * ishtp_cl_is_other_connecting - checks if other
 * client with the same fw client id is connecting
 * returns true if other client is connected, 0 - otherwise.
 */
bool ishtp_cl_is_other_connecting(struct ishtp_cl *cl)
{
	struct ishtp_device *dev;
	struct ishtp_cl *pos;
	struct ishtp_cl *next;
	unsigned long	flags;

	if (WARN_ON(!cl || !cl->dev))
		return false;

	dev = cl->dev;
	spin_lock_irqsave(&dev->cl_list_lock, flags);
	list_for_each_entry_safe(pos, next, &dev->cl_list, link) {
		if ((pos->state == ISHTP_CL_CONNECTING) && (pos != cl) &&
				cl->fw_client_id == pos->fw_client_id) {
			spin_unlock_irqrestore(&dev->cl_list_lock, flags);
			return true;
		}
	}
	spin_unlock_irqrestore(&dev->cl_list_lock, flags);

	return false;
}

/* ishtp_cl_connect - connect host client to the fw one */
int ishtp_cl_connect(struct ishtp_cl *cl)
{
	struct ishtp_device *dev;
	long timeout = ishtp_secs_to_jiffies(ISHTP_CL_CONNECT_TIMEOUT);
	int rets;

	if (WARN_ON(!cl || !cl->dev))
		return -ENODEV;

	if (ishtp_cl_is_other_connecting(cl))
		return	-EBUSY;

	dev = cl->dev;

	if (ishtp_hbm_cl_connect_req(dev, cl))
		return -ENODEV;


	rets = wait_event_timeout(cl->wait_ctrl_res,
		(dev->dev_state == ISHTP_DEV_ENABLED &&
		(cl->state == ISHTP_CL_CONNECTED ||
		cl->state == ISHTP_CL_DISCONNECTED)), timeout * HZ);
	/*
	 * If FW reset arrived, this will happen. Don't check cl->,
	 * as 'cl' may be freed already
	 */
	if (dev->dev_state != ISHTP_DEV_ENABLED)
		return -EFAULT;

	if (cl->state != ISHTP_CL_CONNECTED)
		return -EFAULT;

	rets = cl->status;
	if (rets)
		return rets;

	rets = ishtp_cl_device_bind(cl);
	if (rets) {
		ishtp_cl_disconnect(cl);
		return rets;
	}

	rets = ishtp_cl_alloc_rx_ring(cl);
	if (rets) {
		/* if failed allocation, disconnect */
		ishtp_cl_disconnect(cl);
		return rets;
	}

	rets = ishtp_cl_alloc_tx_ring(cl);
	if (rets) {
		/* if failed allocation, disconnect */
		ishtp_cl_free_rx_ring(cl);
		ishtp_cl_disconnect(cl);
		return rets;
	}

	/* Upon successful connection and allocation, emit flow-control */
	rets = ishtp_cl_read_start(cl);
	return rets;
}
EXPORT_SYMBOL(ishtp_cl_connect);

/* ishtp_cl_read_start - start to read client message */
int ishtp_cl_read_start(struct ishtp_cl *cl)
{
	struct ishtp_device *dev;
	struct ishtp_cl_rb *rb;
	int rets;
	int i;
	unsigned long	flags;
	unsigned long	dev_flags;

	if (WARN_ON(!cl || !cl->dev))
		return -ENODEV;

	dev = cl->dev;

	if (cl->state != ISHTP_CL_CONNECTED)
		return -ENODEV;

	if (dev->dev_state != ISHTP_DEV_ENABLED)
		return -ENODEV;

	i = ishtp_fw_cl_by_id(dev, cl->fw_client_id);
	if (i < 0) {
		dev_err(&cl->device->dev, "no such fw client %d\n",
			cl->fw_client_id);
		return -ENODEV;
	}

	/* The current rb is the head of the free rb list */
	spin_lock_irqsave(&cl->free_list_spinlock, flags);
	if (list_empty(&cl->free_rb_list.list)) {
		dev_warn(&cl->device->dev, "[ishtp-ish] Rx buffers pool is empty\n");
		rets = -ENOMEM;
		rb = NULL;
		spin_unlock_irqrestore(&cl->free_list_spinlock, flags);
		goto out;
	}
	rb = list_entry(cl->free_rb_list.list.next, struct ishtp_cl_rb, list);
	list_del_init(&rb->list);
	spin_unlock_irqrestore(&cl->free_list_spinlock, flags);

	rb->cl = cl;
	rb->buf_idx = 0;

	INIT_LIST_HEAD(&rb->list);
	rets = 0;

	/*
	 * This must be BEFORE sending flow control -
	 * response in ISR may come too fast...
	 */
	spin_lock_irqsave(&dev->read_list_spinlock, dev_flags);
	list_add_tail(&rb->list, &dev->read_list.list);
	spin_unlock_irqrestore(&dev->read_list_spinlock, dev_flags);
	if (ishtp_hbm_cl_flow_control_req(dev, cl)) {
		rets = -ENODEV;
		goto out;
	}
out:
	/* if ishtp_hbm_cl_flow_control_req failed, return rb to free list */
	if (rets && rb) {
		spin_lock_irqsave(&dev->read_list_spinlock, dev_flags);
		list_del(&rb->list);
		spin_unlock_irqrestore(&dev->read_list_spinlock, dev_flags);

		spin_lock_irqsave(&cl->free_list_spinlock, flags);
		list_add_tail(&rb->list, &cl->free_rb_list.list);
		spin_unlock_irqrestore(&cl->free_list_spinlock, flags);
	}
	return rets;
}

int ishtp_cl_send(struct ishtp_cl *cl, uint8_t *buf, size_t length)
{
	struct ishtp_device	*dev;
	int	id;
	struct ishtp_cl_tx_ring	*cl_msg;
	int	have_msg_to_send = 0;
	unsigned long	tx_flags, tx_free_flags;

	if (WARN_ON(!cl || !cl->dev))
		return -ENODEV;

	dev = cl->dev;

	if (cl->state != ISHTP_CL_CONNECTED) {
		++cl->err_send_msg;
		return -EPIPE;
	}

	if (dev->dev_state != ISHTP_DEV_ENABLED) {
		++cl->err_send_msg;
		return -ENODEV;
	}

	/* Check if we have fw client device */
	id = ishtp_fw_cl_by_id(dev, cl->fw_client_id);
	if (id < 0) {
		++cl->err_send_msg;
		return -ENOENT;
	}

	if (length > dev->fw_clients[id].props.max_msg_length) {
		++cl->err_send_msg;
		return -EMSGSIZE;
	}

	/* No free bufs */
	spin_lock_irqsave(&cl->tx_free_list_spinlock, tx_free_flags);
	if (list_empty(&cl->tx_free_list.list)) {
		spin_unlock_irqrestore(&cl->tx_free_list_spinlock,
			tx_free_flags);
		++cl->err_send_msg;
		return	-ENOMEM;
	}

	cl_msg = list_first_entry(&cl->tx_free_list.list,
		struct ishtp_cl_tx_ring, list);
	if (!cl_msg->send_buf.data)
		return	-EIO;		/* Should not happen,
					as free list is pre-allocated */
	/*
	 * This is safe, as 'length' is already checked for not exceeding
	 * max ISHTP message size per client
	 */
	list_del_init(&cl_msg->list);
	spin_unlock_irqrestore(&cl->tx_free_list_spinlock, tx_free_flags);
	memcpy(cl_msg->send_buf.data, buf, length);
	cl_msg->send_buf.size = length;
	spin_lock_irqsave(&cl->tx_list_spinlock, tx_flags);
	have_msg_to_send = !list_empty(&cl->tx_list.list);
	list_add_tail(&cl_msg->list, &cl->tx_list.list);
	spin_unlock_irqrestore(&cl->tx_list_spinlock, tx_flags);

	if (!have_msg_to_send && cl->ishtp_flow_ctrl_creds > 0)
		ishtp_cl_send_msg(dev, cl);

	return	0;
}
EXPORT_SYMBOL(ishtp_cl_send);

/* ishtp_cl_read_complete - processes completed operation for a client */
void ishtp_cl_read_complete(struct ishtp_cl_rb *rb)
{
	unsigned long	flags;
	int	schedule_work_flag = 0;
	struct ishtp_cl	*cl = rb->cl;

	if (waitqueue_active(&cl->rx_wait)) {
		cl->read_rb = rb;
		wake_up_interruptible(&cl->rx_wait);
	} else {
		spin_lock_irqsave(&cl->in_process_spinlock, flags);
		/*
		 * if in-process list is empty, then need to schedule
		 * the processing thread
		 */
		schedule_work_flag = list_empty(&cl->in_process_list.list);
		list_add_tail(&rb->list, &cl->in_process_list.list);
		spin_unlock_irqrestore(&cl->in_process_spinlock, flags);

		if (schedule_work_flag)
			ishtp_cl_bus_rx_event(cl->device);
	}
}

/* ishtp_cl_all_disconnect - disconnect forcefully all connected clients */
void ishtp_cl_all_disconnect(struct ishtp_device *dev)
{
	struct ishtp_cl *cl, *next;
	unsigned long	flags;

	spin_lock_irqsave(&dev->cl_list_lock, flags);
	list_for_each_entry_safe(cl, next, &dev->cl_list, link) {
		cl->state = ISHTP_CL_DISCONNECTED;
		cl->ishtp_flow_ctrl_creds = 0;
		cl->read_rb = NULL;
	}
	spin_unlock_irqrestore(&dev->cl_list_lock, flags);
}

/* ishtp_cl_all_read_wakeup - wake up all readings so they can be interrupted */
void ishtp_cl_all_read_wakeup(struct ishtp_device *dev)
{
	struct ishtp_cl *cl, *next;
	unsigned long	flags;

	spin_lock_irqsave(&dev->cl_list_lock, flags);
	list_for_each_entry_safe(cl, next, &dev->cl_list, link) {
		if (waitqueue_active(&cl->rx_wait))
			wake_up_interruptible(&cl->rx_wait);
	}
	spin_unlock_irqrestore(&dev->cl_list_lock, flags);
}

static void	ipc_tx_callback(void *prm)
{
	struct ishtp_cl	*cl = prm;
	struct ishtp_cl_tx_ring	*cl_msg;
	size_t	rem;
	struct ishtp_device	*dev = (cl ? cl->dev : NULL);
	struct ishtp_msg_hdr	ishtp_hdr;
	unsigned long	tx_flags, tx_free_flags;
	unsigned char	*pmsg;

	if (!dev)
		return;

	/*
	 * Other conditions if some critical error has
	 * occurred before this callback is called
	 */
	if (dev->dev_state != ISHTP_DEV_ENABLED)
		return;

	if (cl->state != ISHTP_CL_CONNECTED)
		return;

	spin_lock_irqsave(&cl->tx_list_spinlock, tx_flags);
	if (list_empty(&cl->tx_list.list)) {
		spin_unlock_irqrestore(&cl->tx_list_spinlock, tx_flags);
		return;
	}

	if (cl->ishtp_flow_ctrl_creds != 1 && !cl->sending) {
		spin_unlock_irqrestore(&cl->tx_list_spinlock, tx_flags);
		return;
	}

	if (!cl->sending) {
		--cl->ishtp_flow_ctrl_creds;
		cl->last_ipc_acked = 0;
		cl->last_tx_path = CL_TX_PATH_IPC;
		cl->sending = 1;
	}

	cl_msg = list_entry(cl->tx_list.list.next, struct ishtp_cl_tx_ring,
		list);
	rem = cl_msg->send_buf.size - cl->tx_offs;

	ishtp_hdr.host_addr = cl->host_client_id;
	ishtp_hdr.fw_addr = cl->fw_client_id;
	ishtp_hdr.reserved = 0;
	pmsg = cl_msg->send_buf.data + cl->tx_offs;

	if (rem <= dev->mtu) {
		ishtp_hdr.length = rem;
		ishtp_hdr.msg_complete = 1;
		cl->sending = 0;
		list_del_init(&cl_msg->list);	/* Must be before write */
		spin_unlock_irqrestore(&cl->tx_list_spinlock, tx_flags);
		/* Submit to IPC queue with no callback */
		ishtp_write_message(dev, &ishtp_hdr, pmsg);
		spin_lock_irqsave(&cl->tx_free_list_spinlock, tx_free_flags);
		list_add_tail(&cl_msg->list, &cl->tx_free_list.list);
		spin_unlock_irqrestore(&cl->tx_free_list_spinlock,
			tx_free_flags);
	} else {
		/* Send IPC fragment */
		spin_unlock_irqrestore(&cl->tx_list_spinlock, tx_flags);
		cl->tx_offs += dev->mtu;
		ishtp_hdr.length = dev->mtu;
		ishtp_hdr.msg_complete = 0;
		send_ishtp_msg(dev, &ishtp_hdr, pmsg, ipc_tx_callback, cl);
	}
}

static void	ishtp_cl_send_msg_ipc(struct ishtp_device *dev,
	struct ishtp_cl *cl)
{
	/* If last DMA message wasn't acked yet, leave this one in Tx queue */
	if (cl->last_tx_path == CL_TX_PATH_DMA && cl->last_dma_acked == 0)
		return;

	cl->tx_offs = 0;
	ipc_tx_callback(cl);
	++cl->send_msg_cnt_ipc;
}

static void	ishtp_cl_send_msg_dma(struct ishtp_device *dev,
	struct ishtp_cl *cl)
{
	struct ishtp_msg_hdr	hdr;
	struct dma_xfer_hbm	dma_xfer;
	unsigned char	*msg_addr;
	int off;
	struct ishtp_cl_tx_ring	*cl_msg;

	/* If last IPC message wasn't acked yet, leave this one in Tx queue */
	if (cl->last_tx_path == CL_TX_PATH_IPC && cl->last_ipc_acked == 0)
		return;

	cl_msg = list_entry(cl->tx_list.list.next, struct ishtp_cl_tx_ring,
		list);

	msg_addr = get_dma_send_buf(dev, cl_msg->send_buf.size);
	if (!msg_addr) {
		if (dev->transfer_path == CL_TX_PATH_DEFAULT)
			ishtp_cl_send_msg_ipc(dev, cl);
		return;
	}

	--cl->ishtp_flow_ctrl_creds;
	cl->last_dma_acked = 0;
	cl->last_dma_addr = msg_addr;
	cl->last_tx_path = CL_TX_PATH_DMA;

	/* write msg to dma buf */
	memcpy(msg_addr, cl_msg->send_buf.data, cl_msg->send_buf.size);

	/* send dma_xfer hbm msg */
	off = msg_addr - (unsigned char *)host_dma_tx_buf;
	ishtp_hbm_hdr(&hdr, sizeof(struct dma_xfer_hbm));
	dma_xfer.hbm = DMA_XFER;
	dma_xfer.fw_client_id = cl->fw_client_id;
	dma_xfer.host_client_id = cl->host_client_id;
	dma_xfer.reserved = 0;
	dma_xfer.msg_addr = host_dma_tx_buf_phys + off;
	dma_xfer.msg_length = cl_msg->send_buf.size;
	dma_xfer.reserved2 = 0;
	ishtp_write_message(dev, &hdr, (unsigned char *)&dma_xfer);
	++cl->send_msg_cnt_dma;
}

void ishtp_cl_send_msg(struct ishtp_device *dev, struct ishtp_cl *cl)
{
	/* CHANGEME: after enforcement path is validated */
	if (dev->transfer_path == CL_TX_PATH_DMA /*||
				dev->transfer_path == DEFAULT &&
			length > dev->mtu * DMA_WORTH_THRESHOLD */)
		ishtp_cl_send_msg_dma(dev, cl);
	else
		ishtp_cl_send_msg_ipc(dev, cl);
}
EXPORT_SYMBOL(ishtp_cl_send_msg);

/*
 * Receive and dispatch ISHTP client messages
 *
 * (!) ISR context
 */
void	recv_ishtp_cl_msg(struct ishtp_device *dev,
		struct ishtp_msg_hdr *ishtp_hdr)
{
	struct ishtp_cl *cl;
	struct ishtp_cl_rb *rb, *next;
	struct ishtp_cl_rb *new_rb;
	unsigned char *buffer = NULL;
	struct ishtp_cl_rb *complete_rb = NULL;
	unsigned long	dev_flags;
	unsigned long	flags;
	int	rb_count;


	if (ishtp_hdr->reserved) {
		dev_err(dev->devc, "corrupted message header.\n");
		goto	eoi;
	}

	if (ishtp_hdr->length > IPC_PAYLOAD_SIZE) {
		dev_err(dev->devc,
			"ISHTP message length in hdr exceeds IPC MTU\n");
		goto	eoi;
	}

	spin_lock_irqsave(&dev->read_list_spinlock, dev_flags);
	rb_count = -1;
	list_for_each_entry_safe(rb, next, &dev->read_list.list, list) {
		++rb_count;
		cl = rb->cl;
		if (!cl || !(cl->host_client_id == ishtp_hdr->host_addr &&
				cl->fw_client_id == ishtp_hdr->fw_addr) ||
				!(cl->state == ISHTP_CL_CONNECTED))
			continue;

		/*
		 * If no Rx buffer is allocated, disband the rb
		 */
		if (rb->buffer.size == 0 || rb->buffer.data == NULL) {
			spin_unlock_irqrestore(&dev->read_list_spinlock,
				dev_flags);
			dev_err(&cl->device->dev, "Rx buffer is not allocated.\n");
			list_del(&rb->list);
			ishtp_io_rb_free(rb);
			cl->status = -ENOMEM;
			goto	eoi;
		}

		/*
		 * If message buffer overflown (exceeds max. client msg
		 * size, drop message and return to free buffer.
		 * Do we need to disconnect such a client? (We don't send
		 * back FC, so communication will be stuck anyway)
		 */
		if (rb->buffer.size < ishtp_hdr->length + rb->buf_idx) {
			spin_unlock_irqrestore(&dev->read_list_spinlock,
				dev_flags);
			dev_err(&cl->device->dev,
				"message overflow. size %d len %d idx %ld\n",
				rb->buffer.size, ishtp_hdr->length,
				rb->buf_idx);
			list_del(&rb->list);
			ishtp_io_rb_recycle(rb);
			cl->status = -EIO;
			goto	eoi;
		}

		buffer = rb->buffer.data + rb->buf_idx;
		dev->ops->ishtp_read(dev, buffer, ishtp_hdr->length);

		rb->buf_idx += ishtp_hdr->length;
		if (ishtp_hdr->msg_complete) {
			/* Last fragment in message - it's complete */
			cl->status = 0;
			list_del(&rb->list);
			complete_rb = rb;

			--cl->out_flow_ctrl_creds;
			/*
			 * the whole msg arrived, send a new FC, and add a new
			 * rb buffer for the next coming msg
			 */
			spin_lock_irqsave(&cl->free_list_spinlock, flags);

			if (!list_empty(&cl->free_rb_list.list)) {
				new_rb = list_entry(cl->free_rb_list.list.next,
					struct ishtp_cl_rb, list);
				list_del_init(&new_rb->list);
				spin_unlock_irqrestore(&cl->free_list_spinlock,
					flags);
				new_rb->cl = cl;
				new_rb->buf_idx = 0;
				INIT_LIST_HEAD(&new_rb->list);
				list_add_tail(&new_rb->list,
					&dev->read_list.list);

				ishtp_hbm_cl_flow_control_req(dev, cl);
			} else {
				spin_unlock_irqrestore(&cl->free_list_spinlock,
					flags);
			}
		}
		/* One more fragment in message (even if this was last) */
		++cl->recv_msg_num_frags;

		/*
		 * We can safely break here (and in BH too),
		 * a single input message can go only to a single request!
		 */
		break;
	}

	spin_unlock_irqrestore(&dev->read_list_spinlock, dev_flags);
	/* If it's nobody's message, just read and discard it */
	if (!buffer) {
		uint8_t	rd_msg_buf[ISHTP_RD_MSG_BUF_SIZE];

		dev_err(dev->devc, "Dropped Rx msg - no request\n");
		dev->ops->ishtp_read(dev, rd_msg_buf, ishtp_hdr->length);
		goto	eoi;
	}

	/* Looks like this is interrupt-safe */
	if (complete_rb) {
		struct timeval	tv;
		do_gettimeofday(&tv);
		cl->rx_sec = tv.tv_sec;
		cl->rx_usec = tv.tv_usec;
		++cl->recv_msg_cnt_ipc;
		ishtp_cl_read_complete(complete_rb);
	}
eoi:
	return;
}

/*
 * Receive and dispatch ISHTP client dma message
 *
 * (!) ISR context
 */
void	recv_ishtp_cl_msg_dma(struct ishtp_device *dev, void *msg,
	struct dma_xfer_hbm *hbm)
{
	struct ishtp_cl *cl;
	struct ishtp_cl_rb *rb, *next;
	struct ishtp_cl_rb *new_rb;
	unsigned char *buffer = NULL;
	struct ishtp_cl_rb *complete_rb = NULL;
	unsigned long	dev_flags;
	unsigned long	flags;

	spin_lock_irqsave(&dev->read_list_spinlock, dev_flags);
	list_for_each_entry_safe(rb, next, &dev->read_list.list, list) {
		cl = rb->cl;
		if (!cl || !(cl->host_client_id == hbm->host_client_id &&
				cl->fw_client_id == hbm->fw_client_id) ||
				!(cl->state == ISHTP_CL_CONNECTED))
			continue;

		/*
		 * If no Rx buffer is allocated, disband the rb
		 */
		if (rb->buffer.size == 0 || rb->buffer.data == NULL) {
			spin_unlock_irqrestore(&dev->read_list_spinlock,
				dev_flags);
			dev_err(&cl->device->dev,
				"response buffer is not allocated.\n");
			list_del(&rb->list);
			ishtp_io_rb_free(rb);
			cl->status = -ENOMEM;
			goto	eoi;
		}

		/*
		 * If message buffer overflown (exceeds max. client msg
		 * size, drop message and return to free buffer.
		 * Do we need to disconnect such a client? (We don't send
		 * back FC, so communication will be stuck anyway)
		 */
		if (rb->buffer.size < hbm->msg_length) {
			spin_unlock_irqrestore(&dev->read_list_spinlock,
				dev_flags);
			dev_err(&cl->device->dev,
				"message overflow. size %d len %d idx %ld\n",
				rb->buffer.size, hbm->msg_length, rb->buf_idx);
			list_del(&rb->list);
			ishtp_io_rb_recycle(rb);
			cl->status = -EIO;
			goto	eoi;
		}

		buffer = rb->buffer.data;
		memcpy(buffer, msg, hbm->msg_length);
		rb->buf_idx = hbm->msg_length;

		/* Last fragment in message - it's complete */
		cl->status = 0;
		list_del(&rb->list);
		complete_rb = rb;

		--cl->out_flow_ctrl_creds;
		/*
		 * the whole msg arrived, send a new FC, and add a new
		 * rb buffer for the next coming msg
		 */
		spin_lock_irqsave(&cl->free_list_spinlock, flags);

		if (!list_empty(&cl->free_rb_list.list)) {
			new_rb = list_entry(cl->free_rb_list.list.next,
				struct ishtp_cl_rb, list);
			list_del_init(&new_rb->list);
			spin_unlock_irqrestore(&cl->free_list_spinlock,
				flags);
			new_rb->cl = cl;
			new_rb->buf_idx = 0;
			INIT_LIST_HEAD(&new_rb->list);
			list_add_tail(&new_rb->list,
				&dev->read_list.list);

			ishtp_hbm_cl_flow_control_req(dev, cl);
		} else {
			spin_unlock_irqrestore(&cl->free_list_spinlock,
				flags);
		}

		/* One more fragment in message (this is always last) */
		++cl->recv_msg_num_frags;

		/*
		 * We can safely break here (and in BH too),
		 * a single input message can go only to a single request!
		 */
		break;
	}

	spin_unlock_irqrestore(&dev->read_list_spinlock, dev_flags);
	/* If it's nobody's message, just read and discard it */
	if (!buffer) {
		dev_err(dev->devc, "Dropped Rx (DMA) msg - no request\n");
		goto	eoi;
	}

	/* Looks like this is interrupt-safe */
	if (complete_rb) {
		struct timeval	tv;
		do_gettimeofday(&tv);
		cl->rx_sec = tv.tv_sec;
		cl->rx_usec = tv.tv_usec;
		++cl->recv_msg_cnt_dma;
		ishtp_cl_read_complete(complete_rb);
	}
eoi:
	return;
}
