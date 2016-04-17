/*
 * ISHTP client driver for HID (ISH)
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
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/uuid.h>
#include "ishtp_dev.h"
#include "client.h"
#include "ishtp-hid.h"

/* Rx ring buffer pool size */
#define HID_CL_RX_RING_SIZE	32
#define HID_CL_TX_RING_SIZE	16

/*
 * Currently this driver works as long as there is only a single ISHTP client
 * device
 */
struct ishtp_cl	*hid_ishtp_cl = NULL;			/* ISH ISHTP client */

/* Set when ISH ISHTP client is successfully probed */
int	hid_ishtp_client_found;
int	enum_devices_done;	/* Enum devices response complete flag */
int	hid_descr_done;		/* Get HID descriptor complete flag */
int	report_descr_done;	/* Get report descriptor complete flag */
int	get_report_done;	/* Get Feature/Input report complete flag */

struct device_info	*hid_devices;
struct hid_device	*hid_sensor_hubs[MAX_HID_DEVICES];
unsigned	cur_hid_dev;
unsigned	hid_dev_count;
unsigned	num_hid_devices;
unsigned char	*hid_descr[MAX_HID_DEVICES];
int		hid_descr_size[MAX_HID_DEVICES];
unsigned char	*report_descr[MAX_HID_DEVICES];
int		report_descr_size[MAX_HID_DEVICES];

static int	init_done;
static wait_queue_head_t	init_wait;
wait_queue_head_t	ishtp_hid_wait;
struct work_struct hid_init_work;
static unsigned	bad_recv_cnt;
static int	multi_packet_cnt;

void (*flush_cb)(void); /* flush notification */

static void	report_bad_packet(void *recv_buf, size_t cur_pos,
	size_t payload_len)
{
	struct hostif_msg	*recv_msg = recv_buf;

	dev_err(&hid_ishtp_cl->device->dev, "[hid-ish]: BAD packet %02X\n"
		"total_bad=%u cur_pos=%u\n"
		"[%02X %02X %02X %02X]\n"
		"payload_len=%u\n"
		"multi_packet_cnt=%u\n"
		"is_response=%02X\n",
		recv_msg->hdr.command, bad_recv_cnt, (unsigned)cur_pos,
		((unsigned char *)recv_msg)[0], ((unsigned char *)recv_msg)[1],
		((unsigned char *)recv_msg)[2], ((unsigned char *)recv_msg)[3],
		(unsigned)payload_len, multi_packet_cnt,
		recv_msg->hdr.command & ~CMD_MASK);
}

/* ISHTP client driver structures and API for bus interface */
static void	process_recv(void *recv_buf, size_t data_len)
{
	struct hostif_msg	*recv_msg;
	unsigned char	*payload;

	struct device_info	*dev_info;
	int	i, j;
	size_t	payload_len, total_len, cur_pos;
	int	report_type;

	struct report_list *reports_list;
	char *reports;
	size_t report_len;

	if (data_len < sizeof(struct hostif_msg_hdr)) {
		dev_err(&hid_ishtp_cl->device->dev,
			"[hid-ish]: error, received %u which is "
			"less than data header %u\n",
			(unsigned)data_len,
			(unsigned)sizeof(struct hostif_msg_hdr));
		++bad_recv_cnt;
		ish_hw_reset(hid_ishtp_cl->dev);
		return;
	}

	payload = recv_buf + sizeof(struct hostif_msg_hdr);
	total_len = data_len;
	cur_pos = 0;

	do {
		recv_msg = (struct hostif_msg *)(recv_buf + cur_pos);
		payload_len = recv_msg->hdr.size;

		/* Sanity checks */
		if (cur_pos + payload_len + sizeof(struct hostif_msg) >
				total_len) {
			++bad_recv_cnt;
			report_bad_packet(recv_msg, cur_pos, payload_len);
			ish_hw_reset(hid_ishtp_cl->dev);
			break;
		}


		switch (recv_msg->hdr.command & CMD_MASK) {
		default:
			++bad_recv_cnt;
			report_bad_packet(recv_msg, cur_pos, payload_len);
			ish_hw_reset(hid_ishtp_cl->dev);
			break;

		case HOSTIF_DM_ENUM_DEVICES:
			if ((!(recv_msg->hdr.command & ~CMD_MASK) ||
					init_done)) {
				++bad_recv_cnt;
				report_bad_packet(recv_msg, cur_pos,
					payload_len);
				ish_hw_reset(hid_ishtp_cl->dev);
				break;
			}
			hid_dev_count = (unsigned)*payload;
			hid_devices = kmalloc(hid_dev_count *
				sizeof(struct device_info), GFP_KERNEL);
			if (hid_devices)
				memset(hid_devices, 0, hid_dev_count *
					sizeof(struct device_info));

			for (i = 0; i < hid_dev_count; ++i) {
				if (1 + sizeof(struct device_info) * i >=
						payload_len) {
					dev_err(&hid_ishtp_cl->device->dev,
						"[hid-ish]: [ENUM_DEVICES]:"
						" content size %lu "
						"is bigger than "
						"payload_len %u\n",
						1 + sizeof(struct device_info)
						* i,
						(unsigned)payload_len);
				}

				if (1 + sizeof(struct device_info) * i >=
						data_len)
					break;

				dev_info = (struct device_info *)(payload + 1 +
					sizeof(struct device_info) * i);
				if (hid_devices)
					memcpy(hid_devices + i, dev_info,
						sizeof(struct device_info));
			}

			enum_devices_done = 1;
			if (waitqueue_active(&init_wait))
				wake_up(&init_wait);

			break;

		case HOSTIF_GET_HID_DESCRIPTOR:
			if ((!(recv_msg->hdr.command & ~CMD_MASK) ||
					init_done)) {
				++bad_recv_cnt;
				report_bad_packet(recv_msg, cur_pos,
					payload_len);
				ish_hw_reset(hid_ishtp_cl->dev);
				break;
			}
			hid_descr[cur_hid_dev] = kmalloc(payload_len,
				GFP_KERNEL);
			if (hid_descr[cur_hid_dev])
				memcpy(hid_descr[cur_hid_dev], payload,
					payload_len);
			hid_descr_size[cur_hid_dev] = payload_len;

			hid_descr_done = 1;
			if (waitqueue_active(&init_wait))
				wake_up(&init_wait);

			break;

		case HOSTIF_GET_REPORT_DESCRIPTOR:
			if ((!(recv_msg->hdr.command & ~CMD_MASK) ||
					init_done)) {
				++bad_recv_cnt;
				report_bad_packet(recv_msg, cur_pos,
					payload_len);
				ish_hw_reset(hid_ishtp_cl->dev);
				break;
			}
			report_descr[cur_hid_dev] = kmalloc(payload_len,
				GFP_KERNEL);
			if (report_descr[cur_hid_dev])
				memcpy(report_descr[cur_hid_dev], payload,
					payload_len);
			report_descr_size[cur_hid_dev] = payload_len;

			report_descr_done = 1;
			if (waitqueue_active(&init_wait))
				wake_up(&init_wait);

			break;

		case HOSTIF_GET_FEATURE_REPORT:
			report_type = HID_FEATURE_REPORT;
			flush_cb(); /* each "GET_FEATURE_REPORT" may indicate
					a flush complete event */
			goto	do_get_report;

		case HOSTIF_GET_INPUT_REPORT:
			report_type = HID_INPUT_REPORT;
do_get_report:
			/* Get index of device that matches this id */
			for (i = 0; i < num_hid_devices; ++i)
				if (recv_msg->hdr.device_id ==
						hid_devices[i].dev_id)
					if (hid_sensor_hubs[i] != NULL) {
						hid_input_report(
							hid_sensor_hubs[i],
							report_type, payload,
							payload_len, 0);
						break;
					}
			get_report_done = 1;
			if (waitqueue_active(&ishtp_hid_wait))
				wake_up(&ishtp_hid_wait);
			break;

		case HOSTIF_SET_FEATURE_REPORT:
			get_report_done = 1;
			if (waitqueue_active(&ishtp_hid_wait))
				wake_up(&ishtp_hid_wait);
			break;

		case HOSTIF_PUBLISH_INPUT_REPORT:
			report_type = HID_INPUT_REPORT;
			for (i = 0; i < num_hid_devices; ++i)
				if (recv_msg->hdr.device_id ==
						hid_devices[i].dev_id)
					if (hid_sensor_hubs[i] != NULL)
						hid_input_report(
							hid_sensor_hubs[i],
							report_type, payload,
							payload_len, 0);
			break;

		case HOSTIF_PUBLISH_INPUT_REPORT_LIST:
			report_type = HID_INPUT_REPORT;
			reports_list = (struct report_list *)payload;
			reports = (char *)reports_list->reports;

			for (j = 0; j < reports_list->num_of_reports; j++) {
				recv_msg = (struct hostif_msg *)(reports +
					sizeof(uint16_t));
				report_len = *(uint16_t *)reports;
				payload = reports + sizeof(uint16_t) +
					sizeof(struct hostif_msg_hdr);
				payload_len = report_len -
					sizeof(struct hostif_msg_hdr);

				for (i = 0; i < num_hid_devices; ++i)
					if (recv_msg->hdr.device_id ==
							hid_devices[i].dev_id &&
							hid_sensor_hubs[i] !=
							NULL) {
						hid_input_report(
							hid_sensor_hubs[i],
							report_type,
							payload, payload_len,
							0);
					}

				reports += sizeof(uint16_t) + report_len;
			}
			break;
		}

		if (!cur_pos && cur_pos + payload_len +
				sizeof(struct hostif_msg) < total_len)
			++multi_packet_cnt;

		cur_pos += payload_len + sizeof(struct hostif_msg);
		payload += payload_len + sizeof(struct hostif_msg);

	} while (cur_pos < total_len);
}

void ish_cl_event_cb(struct ishtp_cl_device *device)
{
	size_t r_length;
	struct ishtp_cl_rb *rb_in_proc;
	unsigned long	flags;

	if (!hid_ishtp_cl)
		return;

	spin_lock_irqsave(&hid_ishtp_cl->in_process_spinlock, flags);
	while (!list_empty(&hid_ishtp_cl->in_process_list.list)) {
		rb_in_proc = list_entry(hid_ishtp_cl->in_process_list.list.next,
			struct ishtp_cl_rb, list);
		list_del_init(&rb_in_proc->list);
		spin_unlock_irqrestore(&hid_ishtp_cl->in_process_spinlock,
			flags);

		if (!rb_in_proc->buffer.data)
			return;

		r_length = rb_in_proc->buf_idx;

		/* decide what to do with received data */
		process_recv(rb_in_proc->buffer.data, r_length);

		ishtp_io_rb_recycle(rb_in_proc);
		spin_lock_irqsave(&hid_ishtp_cl->in_process_spinlock, flags);
	}
	spin_unlock_irqrestore(&hid_ishtp_cl->in_process_spinlock, flags);
}

void hid_ishtp_set_feature(struct hid_device *hid, char *buf, unsigned len,
	int report_id)
{
	int	rv;
	struct hostif_msg *msg = (struct hostif_msg *)buf;
	int	i;

	memset(msg, 0, sizeof(struct hostif_msg));
	msg->hdr.command = HOSTIF_SET_FEATURE_REPORT;
	for (i = 0; i < num_hid_devices; ++i)
		if (hid == hid_sensor_hubs[i]) {
			msg->hdr.device_id = hid_devices[i].dev_id;
			break;
		}
	if (i == num_hid_devices)
		return;

	rv = ishtp_cl_send(hid_ishtp_cl, buf, len);
}

void hid_ishtp_get_report(struct hid_device *hid, int report_id,
	int report_type)
{
	int	rv;
	static unsigned char	buf[10];
	unsigned	len;
	struct hostif_msg_to_sensor *msg = (struct hostif_msg_to_sensor *)buf;
	int	i;

	len = sizeof(struct hostif_msg_to_sensor);

	memset(msg, 0, sizeof(struct hostif_msg_to_sensor));
	msg->hdr.command = (report_type == HID_FEATURE_REPORT) ?
		HOSTIF_GET_FEATURE_REPORT : HOSTIF_GET_INPUT_REPORT;
	for (i = 0; i < num_hid_devices; ++i)
		if (hid == hid_sensor_hubs[i]) {
			msg->hdr.device_id = hid_devices[i].dev_id;
			break;
		}
	if (i == num_hid_devices)
		return;

	msg->report_id = report_id;
	rv = ishtp_cl_send(hid_ishtp_cl, buf, len);
}

int	hid_ishtp_cl_probe(struct ishtp_cl_device *cl_device)
{
	int	rv;

	if (!cl_device)
		return	-ENODEV;

	if (uuid_le_cmp(hid_ishtp_guid,
			cl_device->fw_client->props.protocol_name) != 0)
		return	-ENODEV;

	hid_ishtp_cl = ishtp_cl_allocate(cl_device->ishtp_dev);
	if (!hid_ishtp_cl)
		return	-ENOMEM;

	rv = ishtp_cl_link(hid_ishtp_cl, ISHTP_HOST_CLIENT_ID_ANY);
	if (rv)
		return	-ENOMEM;

	hid_ishtp_client_found = 1;
	if (waitqueue_active(&init_wait))
		wake_up(&init_wait);

	schedule_work(&hid_init_work);

	return	0;
}

int	hid_ishtp_cl_remove(struct ishtp_cl_device *dev)
{
	int i;

	ishtp_hid_remove();
	hid_ishtp_client_found = 0;

	ishtp_cl_unlink(hid_ishtp_cl);
	ishtp_cl_flush_queues(hid_ishtp_cl);

	/* disband and free all Tx and Rx client-level rings */
	ishtp_cl_free(hid_ishtp_cl);
	hid_ishtp_cl = NULL;

	for (i = 0; i < num_hid_devices ; ++i) {
		kfree(hid_descr[i]);
		kfree(report_descr[i]);
	}
	num_hid_devices = 0;
	return 0;
}


struct ishtp_cl_driver	hid_ishtp_cl_driver = {
	.name = "ish",
	.probe = hid_ishtp_cl_probe,
	.remove = hid_ishtp_cl_remove,
};

static void workqueue_init_function(struct work_struct *work)
{
	int	rv;
	static unsigned char	buf[512];
	unsigned	len;
	struct hostif_msg	*msg = (struct hostif_msg *)buf;
	int	i;
	struct ishtp_device	*dev;
	int	retry_count;
	unsigned long	flags;

	init_done = 0;

	if (!hid_ishtp_client_found)
		wait_event_timeout(init_wait, hid_ishtp_client_found, 30 * HZ);

	if (!hid_ishtp_client_found) {
		dev_err(NULL,
			"[hid-ish]: timed out waiting for hid-ishtp-client\n");
		rv = -ENODEV;
		goto	ret;
	}

	dev = hid_ishtp_cl->dev;

	/* Connect to FW client */
	hid_ishtp_cl->rx_ring_size = HID_CL_RX_RING_SIZE;
	hid_ishtp_cl->tx_ring_size = HID_CL_TX_RING_SIZE;

	spin_lock_irqsave(&dev->fw_clients_lock, flags);
	i = ishtp_fw_cl_by_uuid(dev, &hid_ishtp_guid);
	hid_ishtp_cl->fw_client_id = dev->fw_clients[i].client_id;
	spin_unlock_irqrestore(&dev->fw_clients_lock, flags);
	hid_ishtp_cl->state = ISHTP_CL_CONNECTING;

	rv = ishtp_cl_connect(hid_ishtp_cl);
	if (rv)
		goto	ret;

	/* Register read callback */
	ishtp_register_event_cb(hid_ishtp_cl->device, ish_cl_event_cb);

	/* Send HOSTIF_DM_ENUM_DEVICES */
	memset(msg, 0, sizeof(struct hostif_msg));
	msg->hdr.command = HOSTIF_DM_ENUM_DEVICES;
	len = sizeof(struct hostif_msg);
	rv = ishtp_cl_send(hid_ishtp_cl, buf, len);
	if (rv)
		goto	ret;

	rv = 0;

	retry_count = 0;
	while (!enum_devices_done && retry_count < 10) {
		wait_event_timeout(init_wait, enum_devices_done, 3 * HZ);
		++retry_count;
		if (!enum_devices_done)
			/* Send HOSTIF_DM_ENUM_DEVICES */
			rv = ishtp_cl_send(hid_ishtp_cl, buf, len);
	}
	if (!enum_devices_done) {
		dev_err(&hid_ishtp_cl->device->dev,
			"[hid-ish]: timed out waiting for enum_devices_done\n");
		rv = -ETIMEDOUT;
		goto	ret;
	}
	if (!hid_devices) {
		dev_err(&hid_ishtp_cl->device->dev,
			"[hid-ish]: failed to allocate HID dev structures\n");
		rv = -ENOMEM;
		goto	ret;
	}

	/* Send GET_HID_DESCRIPTOR for each device */
	num_hid_devices = hid_dev_count;
	dev_warn(&hid_ishtp_cl->device->dev,
		"[hid-ish]: enum_devices_done OK, num_hid_devices=%d\n",
		num_hid_devices);

	for (i = 0; i < num_hid_devices; ++i) {
		cur_hid_dev = i;

		/* Get HID descriptor */
		hid_descr_done = 0;
		memset(msg, 0, sizeof(struct hostif_msg));
		msg->hdr.command = HOSTIF_GET_HID_DESCRIPTOR;
		msg->hdr.device_id = hid_devices[i].dev_id;
		len = sizeof(struct hostif_msg);
		rv = ishtp_cl_send(hid_ishtp_cl, buf, len);
		rv = 0;

		if (!hid_descr_done)
			wait_event_timeout(init_wait, hid_descr_done, 30 * HZ);
		if (!hid_descr_done) {
			dev_err(&hid_ishtp_cl->device->dev,
				"[hid-ish]: timed out waiting for hid_descr_done\n");
			continue;
		}

		if (!hid_descr[i]) {
			dev_err(&hid_ishtp_cl->device->dev,
				"[hid-ish]: failed to allocate HID descriptor buffer\n");
			continue;
		}

		/* Get report descriptor */
		report_descr_done = 0;
		memset(msg, 0, sizeof(struct hostif_msg));
		msg->hdr.command = HOSTIF_GET_REPORT_DESCRIPTOR;
		msg->hdr.device_id = hid_devices[i].dev_id;
		len = sizeof(struct hostif_msg);
		rv = ishtp_cl_send(hid_ishtp_cl, buf, len);

		rv = 0;

		if (!report_descr_done)
			wait_event_timeout(init_wait, report_descr_done,
				30 * HZ);
		if (!report_descr_done) {
			dev_err(&hid_ishtp_cl->device->dev,
				"[hid-ish]: timed out wait for report descr\n");
			continue;
		}

		if (!report_descr[i]) {
			dev_err(&hid_ishtp_cl->device->dev,
				"[hid-ish]: failed to alloc report descr\n");
			continue;
		}

		rv = ishtp_hid_probe(i);
		if (rv) {
			dev_err(&hid_ishtp_cl->device->dev,
				"[hid-ish]: HID probe for #%u failed: %d\n",
				i, rv);
			continue;
		}
	} /* for() on all hid devices */

ret:
	init_done = 1;
}

static int __init ish_init(void)
{
	int	rv;

	ISH_INFO_PRINT(KERN_ERR "[hid-ish]: Build " BUILD_ID "\n");
	g_ish_print_log(
		"[hid-ish]: %s():+++ [Build " BUILD_ID "]\n", __func__);

	init_waitqueue_head(&init_wait);
	init_waitqueue_head(&ishtp_hid_wait);
	INIT_WORK(&hid_init_work, workqueue_init_function);

	/* Register ISHTP client device driver - ISH */
	rv = ishtp_cl_driver_register(&hid_ishtp_cl_driver);

	return rv;

}
late_initcall(ish_init);

static void __exit ish_exit(void)
{
	ishtp_cl_driver_unregister(&hid_ishtp_cl_driver);
}
module_exit(ish_exit);

MODULE_DESCRIPTION("ISH ISHTP client driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL");

