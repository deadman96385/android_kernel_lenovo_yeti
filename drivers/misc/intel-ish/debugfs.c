/*
 * DebugFS for ISHTP driver
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
 *
 */
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/debugfs.h>
#include "ishtp_dev.h"
#include "client.h"

static ssize_t ishtp_dbgfs_read_host_clients(struct file *fp, char __user *ubuf,
					size_t cnt, loff_t *ppos)
{
	struct ishtp_device *ishtp_dev = fp->private_data;
	struct ishtp_cl *cl, *next;
	struct ishtp_cl_rb *rb, *next_rb;
	struct ishtp_cl_tx_ring *tx_rb, *next_tx_rb;
	int	ret, pos = 0;
	unsigned	count;
	unsigned long	flags, flags2, tx_flags, tx_free_flags;
	static const char * const cl_states[] = {"initializing", "connecting",
		"connected", "disconnecting", "disconnected"};
	const size_t bufsz = 1024;
	char *buf = kzalloc(bufsz, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	scnprintf(buf, PAGE_SIZE, "Host clients:\n ------------\n");
	spin_lock_irqsave(&ishtp_dev->device_lock, flags);
	list_for_each_entry_safe(cl, next, &ishtp_dev->cl_list, link) {
		scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
			"id: %d\n", cl->host_client_id);
		scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
			"state: %s\n", cl->state < 0 || cl->state >
			ISHTP_CL_DISCONNECTED ? "unknown" :
			cl_states[cl->state]);
		if (cl->state == ISHTP_CL_CONNECTED) {
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"FW client id: %d\n", cl->fw_client_id);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"Rx ring size: %u\n", cl->rx_ring_size);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"Tx ring size: %u\n", cl->tx_ring_size);

			count = 0;
			spin_lock_irqsave(&cl->in_process_spinlock, flags2);
			list_for_each_entry_safe(rb, next_rb,
					&cl->in_process_list.list, list)
				++count;
			spin_unlock_irqrestore(&cl->in_process_spinlock,
				flags2);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"Rx in work: %u\n", count);

			count = 0;
			spin_lock_irqsave(&cl->in_process_spinlock, flags2);
			list_for_each_entry_safe(rb, next_rb,
				&cl->free_rb_list.list, list)
					++count;
			spin_unlock_irqrestore(&cl->in_process_spinlock,
				flags2);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"Rx free: %u\n", count);

			count = 0;
			spin_lock_irqsave(&cl->tx_list_spinlock, tx_flags);
			list_for_each_entry_safe(tx_rb, next_tx_rb,
				&cl->tx_list.list, list)
				++count;
			spin_unlock_irqrestore(&cl->tx_list_spinlock, tx_flags);

			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"Tx pending: %u\n", count);
			count = 0;
			spin_lock_irqsave(&cl->tx_free_list_spinlock,
				tx_free_flags);
			list_for_each_entry_safe(tx_rb, next_tx_rb,
					&cl->tx_free_list.list, list)
				++count;
			spin_unlock_irqrestore(&cl->tx_free_list_spinlock,
				tx_free_flags);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"Tx free: %u\n", count);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"FC: %u\n",
				(unsigned)cl->ishtp_flow_ctrl_creds);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"out FC: %u\n",
				(unsigned)cl->out_flow_ctrl_creds);
			scnprintf(buf + strlen(buf),
				PAGE_SIZE - strlen(buf), "Err snd msg: %u\n",
				(unsigned)cl->err_send_msg);
			scnprintf(buf + strlen(buf),
				PAGE_SIZE - strlen(buf), "Err snd FC: %u\n",
				(unsigned)cl->err_send_fc);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"Tx IPC count: %u\n",
				(unsigned)cl->send_msg_cnt_ipc);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"Tx DMA count: %u\n",
				(unsigned)cl->send_msg_cnt_dma);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"Rx IPC count: %u\n",
				(unsigned)cl->recv_msg_cnt_ipc);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"Rx DMA count: %u\n",
				(unsigned)cl->recv_msg_cnt_dma);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"FC count: %u\n",
				(unsigned)cl->ishtp_flow_ctrl_cnt);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"out FC cnt: %u\n",
				(unsigned)cl->out_flow_ctrl_cnt);
			scnprintf(buf + strlen(buf), PAGE_SIZE - strlen(buf),
				"Max FC delay: %lu.%06lu\n",
				cl->max_fc_delay_sec, cl->max_fc_delay_usec);
		}
	}
	spin_unlock_irqrestore(&ishtp_dev->device_lock, flags);
	pos = strlen(buf);

	ret = simple_read_from_buffer(ubuf, cnt, ppos, buf, pos);
	kfree(buf);
	return ret;
}

static const struct file_operations ishtp_dbgfs_fops_host_clients = {
	.open = simple_open,
	.read = ishtp_dbgfs_read_host_clients,
	.llseek = generic_file_llseek,
};

static ssize_t ishtp_dbgfs_read_dev_stats(struct file *fp, char __user *ubuf,
					size_t cnt, loff_t *ppos)
{
	struct ishtp_device *ishtp_dev = fp->private_data;
	int ret, pos = 0;
	const size_t bufsz = 1024;
	char *buf = kzalloc(bufsz, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	pos = scnprintf(buf + pos, PAGE_SIZE - strlen(buf),
			"IPC Rx frames: %u; bytes: %llu\n",
			ishtp_dev->ipc_rx_cnt, ishtp_dev->ipc_rx_bytes_cnt);
	pos = scnprintf(buf + pos, PAGE_SIZE - strlen(buf),
			"IPC Tx frames: %u; bytes: %llu\n",
			ishtp_dev->ipc_tx_cnt, ishtp_dev->ipc_tx_bytes_cnt);

	ret = simple_read_from_buffer(ubuf, cnt, ppos, buf, pos);
	kfree(buf);
	return ret;
}
static const struct file_operations ishtp_dbgfs_fops_dev_stats = {
	.open = simple_open,
	.read = ishtp_dbgfs_read_dev_stats,
	.llseek = generic_file_llseek,
};

/**
 * Add the debugfs files
 */
int ishtp_dbgfs_register(struct ishtp_device *dev, const char *name)
{
	struct dentry *dir, *f;
	dir = debugfs_create_dir(name, NULL);
	if (!dir)
		return -ENOMEM;

	f = debugfs_create_file("host_clients", S_IRUSR, dir,
				dev, &ishtp_dbgfs_fops_host_clients);
	if (!f) {
		dev_err(dev->devc, "host_clients: registration failed\n");
		goto err;
	}
	f = debugfs_create_file("dev_stats", S_IRUSR, dir,
				dev, &ishtp_dbgfs_fops_dev_stats);
	if (!f) {
		dev_err(dev->devc, "dev_stats: registration failed\n");
		goto err;
	}
	dev->dbgfs_dir = dir;
	return 0;
err:
	ishtp_dbgfs_deregister(dev);
	return -ENODEV;
}

/**
 * ishtp_dbgfs_deregister - Remove the debugfs files and directories
 * @ishtp - pointer to ishtp device private data
 */
void ishtp_dbgfs_deregister(struct ishtp_device *dev)
{
	if (!dev->dbgfs_dir)
		return;
	debugfs_remove_recursive(dev->dbgfs_dir);
	dev->dbgfs_dir = NULL;
}
