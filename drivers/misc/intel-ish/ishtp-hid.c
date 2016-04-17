/*
 * ISHTP-HID glue driver.
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

#include <linux/hid.h>
#include <linux/sched.h>
#include "ishtp-hid.h"

static int ishtp_hid_parse(struct hid_device *hid)
{
	int	rv;

	rv = hid_parse_report(hid, report_descr[cur_hid_dev],
		report_descr_size[cur_hid_dev]);
	if (rv)
		return	rv;

	return 0;
}

static int ishtp_hid_start(struct hid_device *hid)
{
	return 0;
}

static void ishtp_hid_stop(struct hid_device *hid)
{
	return;
}

static int ishtp_hid_open(struct hid_device *hid)
{
	return 0;
}

static void ishtp_hid_close(struct hid_device *hid)
{
	return;
}

static int ishtp_hid_power(struct hid_device *hid, int lvl)
{
	return 0;
}

static void ishtp_hid_request(struct hid_device *hid, struct hid_report *rep,
	int reqtype)
{
	/* the specific report length, just HID part of it */
	unsigned len = ((rep->size - 1) >> 3) + 1 + (rep->id > 0);
	char *buf;
	unsigned header_size = sizeof(struct hostif_msg);

	len += header_size;

	switch (reqtype) {
	case HID_REQ_GET_REPORT:
		hid_ishtp_get_report(hid, rep->id, rep->type);
		break;
	case HID_REQ_SET_REPORT:
		/* Spare 7 bytes for 64b accesses through
		get/put_unaligned_le64() */
		buf = kzalloc(len + 7, GFP_KERNEL);
		if (!buf)
			return;
		hid_output_report(rep, buf + header_size);
		hid_ishtp_set_feature(hid, buf, len, rep->id);
		kfree(buf);
		break;
	}

	return;
}

static int ishtp_hid_hidinput_input_event(struct input_dev *dev,
		unsigned int type, unsigned int code, int value)
{
	return 0;
}

static int ishtp_wait_for_response(struct hid_device *hid)
{
	if (!get_report_done)
		wait_event_timeout(ishtp_hid_wait, get_report_done, 3 * HZ);

	if (!get_report_done) {
		hid_err(hid, "timeout waiting for response from ISHTP device\n");
		return -1;
	}

	get_report_done = 0;
	return 0;
}

static struct hid_ll_driver ishtp_hid_ll_driver = {
	.parse = ishtp_hid_parse,
	.start = ishtp_hid_start,
	.stop = ishtp_hid_stop,
	.open = ishtp_hid_open,
	.close = ishtp_hid_close,
	.power = ishtp_hid_power,
	.request = ishtp_hid_request,
	.hidinput_input_event = ishtp_hid_hidinput_input_event,
	.wait = ishtp_wait_for_response
};

static int ishtp_hid_get_raw_report(struct hid_device *hid,
	unsigned char report_number, uint8_t *buf, size_t count,
	unsigned char report_type)
{
	return	0;
}

static int ishtp_hid_output_raw_report(struct hid_device *hid, uint8_t *buf,
	size_t count, unsigned char report_type)
{
	return	0;
}

int	ishtp_hid_probe(unsigned cur_hid_dev)
{
	int rv;
	struct hid_device	*hid;

	hid = hid_allocate_device();
	if (IS_ERR(hid)) {
		rv = PTR_ERR(hid);
		return	-ENOMEM;
	}

	hid_sensor_hubs[cur_hid_dev] = hid;

	hid->ll_driver = &ishtp_hid_ll_driver;
	hid->hid_get_raw_report = ishtp_hid_get_raw_report;
	hid->hid_output_raw_report = ishtp_hid_output_raw_report;
	hid->bus = BUS_ISHTP;
	hid->version = le16_to_cpu(ISH_HID_VERSION);
	hid->vendor = le16_to_cpu(ISH_HID_VENDOR);
	hid->product = le16_to_cpu(ISH_HID_PRODUCT);

	snprintf(hid->name, sizeof(hid->name), "%s %04hX:%04hX", "hid-ishtp",
		hid->vendor, hid->product);

	rv = hid_add_device(hid);
	if (rv) {
		if (rv != -ENODEV)
			hid_err(hid, "[hid-ishtp]: can't add HID device: %d\n",
				rv);
		kfree(hid);
		return	rv;
	}

	return 0;
}

void	ishtp_hid_remove(void)
{
	int	i;

	for (i = 0; i < num_hid_devices; ++i)
		if (hid_sensor_hubs[i]) {
			hid_destroy_device(hid_sensor_hubs[i]);
			hid_sensor_hubs[i] = NULL;
		}
}

void register_flush_cb(void (*flush_cb_func)(void))
{
	flush_cb = flush_cb_func;
}
EXPORT_SYMBOL(register_flush_cb);

