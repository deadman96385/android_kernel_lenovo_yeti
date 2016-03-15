/*
 * HID Sensors Driver
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
 * You should have received a copy of the GNU General Public License along with
 * this program;
 *
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/sched.h>
#include "hid-sens-ids.h"
#include <linux/senscol/senscol-core.h>

/* TODO: remove everything but what ISH exposes */
#define USB_VENDOR_ID_INTEL_0		0x8086
#define USB_VENDOR_ID_INTEL_1		0x8087
#define USB_DEVICE_ID_INTEL_HID_SENSOR	0x09fa
#define USB_VENDOR_ID_STM_0		0x0483
#define USB_DEVICE_ID_STM_HID_SENSOR	0x91d1
#define USB_DEVICE_ID_STM_HID_SENSOR_1	0x9100

struct hid_sens_hub_device {
	struct hid_device *hdev;
	uint32_t vendor_id;
	uint32_t product_id;
};

/* struct ish_data - Holds a instance data for a HID hub device */
struct ish_data {
	struct hid_sens_hub_device *hsdev;
	struct mutex mutex;
	int ish_index;	/* Needed to identify sensor in a collection */
};

#define	MAX_HID_SENSOR_HUBS	32
static struct hid_device	*hid_sens_hubs[MAX_HID_SENSOR_HUBS];
static int	ish_cur_count;
static int	ish_max_count;

static struct hid_report *ish_report(int id, struct hid_device *hdev, int dir)
{
	struct hid_report *report;

	list_for_each_entry(report, &hdev->report_enum[dir].report_list, list) {
		if (report->id == id)
			return report;
	}
	hid_warn(hdev, "No report with id 0x%x found\n", id);
	return NULL;
}

static int ish_get_physical_device_count(struct hid_report_enum *report_enum)
{
	struct hid_report *report;
	struct hid_field *field;
	int cnt = 0;

	list_for_each_entry(report, &report_enum->report_list, list) {
		field = report->field[0];
		if (report->maxfield && field && field->physical)
			cnt++;
	}
	return cnt;
}

/* Get sensor hub device by index */
static struct ish_data	*get_ish_by_index(unsigned idx)
{
	int	i;
	struct ish_data	*sd;

	for (i = 0; i < ish_cur_count; ++i) {
		if (!hid_sens_hubs[i])
			continue;
		sd = hid_get_drvdata(hid_sens_hubs[i]);
		if (!sd)
			continue;
		if (sd->ish_index == idx)
			return	sd;
	}
	return	NULL;
}

/* returns field_index with the given hid usage on success, (-1) on failure */
static int get_field_index(struct hid_device *hdev, unsigned report_id,
	unsigned usage, int report_type)
{
	int i = 0;
	struct hid_report *report;

	report = ish_report(report_id, hdev,
		report_type /* HID_FEATURE_REPORT or HID_INPUT_REPORT */);
	if (!report)
		return -1;

	for (i = 0; i < report->maxfield; ++i)
		if (report->field[i]->usage->hid == usage)
			return i;
	return -1;
}

#ifdef CONFIG_PM
static int ish_suspend(struct hid_device *hdev, pm_message_t message)
{
	return	0;
}

static int ish_resume(struct hid_device *hdev)
{
	return 0;
}

static int ish_reset_resume(struct hid_device *hdev)
{
	return 0;
}
#endif /* CONFIG_PM */

#if SENSCOL_1
static int ish_set_feature(struct hid_sens_hub_device *hsdev, uint32_t report_id,
				uint32_t field_index, int32_t value)
{
	struct hid_report *report;
	struct ish_data *data = hid_get_drvdata(hsdev->hdev);
	int ret = 0;

	mutex_lock(&data->mutex);
	report = ish_report(report_id, hsdev->hdev, HID_FEATURE_REPORT);
	if (!report || (field_index >= report->maxfield)) {
		ret = -EINVAL;
		goto done_proc;
	}
	hid_set_field(report->field[field_index], 0, value);
	hid_hw_request(hsdev->hdev, report, HID_REQ_SET_REPORT);
	hid_hw_wait(hsdev->hdev);

done_proc:
	mutex_unlock(&data->mutex);

	return ret;
}

/*
 * The reason for this _ex() function is broken semantics and existing
 * usage of ish_get_feature() that
 * doesn't allow anything with ->report_count > 1 to be delivered.
 * If that was fixed, existing callers would immediately buffer-overflow
 * if such feature was delivered
 * NOTES:
 * - if ret != 0, contents of pvalue and count are undefined.
 * - upon success, count is in int32_t values (not in bytes)
 */
static int ish_get_feature_ex(struct hid_sens_hub_device *hsdev,
	uint32_t report_id, uint32_t field_index, uint32_t *usage_id, int32_t **pvalue,
	size_t *count, unsigned *is_string)
{
	struct hid_report *report;
	struct ish_data *data = hid_get_drvdata(hsdev->hdev);
	int ret = 0;

	mutex_lock(&data->mutex);
	report = ish_report(report_id, hsdev->hdev, HID_FEATURE_REPORT);
	if (!report || (field_index >= report->maxfield) ||
			report->field[field_index]->report_count < 1) {
		ret = -EINVAL;
		goto done_proc;
	}
	hid_hw_request(hsdev->hdev, report, HID_REQ_GET_REPORT);
	hid_hw_wait(hsdev->hdev);
	*pvalue = report->field[field_index]->value;
	*count = report->field[field_index]->report_count;
	*usage_id = report->field[field_index]->usage->hid;
	*is_string = (report->field[field_index]->report_size == 16) &&
			(*count > 1);
done_proc:
	mutex_unlock(&data->mutex);

	return ret;
}

static int	hid_get_sens_property(struct sensor_def *sensor,
	const struct sens_property *prop, char *value, size_t val_buf_size)
{
	unsigned	idx;
	struct ish_data	*sd;
	char	buf[1024];		/* Enough for single property */
	unsigned	report_id;
	int	field;
	uint32_t	usage_id;
	int32_t	*pval;
	size_t	count;
	unsigned	is_string;
	int	rv = 0;

	if (!sensor || !prop)
		return	-EINVAL;	/* input is invalid */

	/* sensor hub device */
	idx = sensor->id >> 16 & 0xFFFF;
	sd = get_ish_by_index(idx);
	if (!sd)
		return	-EINVAL;	/* sensor->id is erroneous */

	/* Report ID */
	report_id = sensor->id & 0xFFFF;

	/* Field index */
	field = get_field_index(sd->hsdev->hdev, report_id, prop->usage_id,
		HID_FEATURE_REPORT);
	if (field == -1)
		return	-EINVAL;	/* Something is still wrong */

	/* Get value */
	rv = ish_get_feature_ex(sd->hsdev, report_id, field, &usage_id,
		&pval, &count, &is_string);
	if (rv)
		return	rv;

	if (is_string) {
		int	i;

		for (i = 0; i < count; ++i)
			buf[i] = (char)pval[i];
		buf[i] = '\0';
	} else {
		/* Verify output length */
		sprintf(buf, "%d", *pval);
	}

	if (strlen(buf) >= val_buf_size)
		return	-EMSGSIZE;
	strcpy(value, buf);
	return	0;
}

static int	hid_set_sens_property(struct sensor_def *sensor,
	const struct sens_property *prop, const char *value)
{
	unsigned	idx;
	struct ish_data	*sd;
	unsigned	report_id;
	int	field;
	int32_t	val;
	int	rv;

	if (!sensor || !prop)
		return	-EINVAL;	/* input is invalid */

	/* Value */
	rv = sscanf(value, " %d ", &val);
	if (rv != 1)
		return	-EINVAL;	/* Bad value */

	/* sensor hub device */
	idx = sensor->id >> 16 & 0xFFFF;
	sd = get_ish_by_index(idx);
	if (!sd)
		return	-EINVAL;	/* sensor->id is erroneous */

	/* Report ID */
	report_id = sensor->id & 0xFFFF;

	/* Field index */
	field = get_field_index(sd->hsdev->hdev, report_id, prop->usage_id,
		HID_FEATURE_REPORT);
	if (field == -1)
		return	-EINVAL;	/* Something is still wrong */

	/* Get value */
	rv = ish_set_feature(sd->hsdev, report_id, field, val);
	return	rv;
}
#endif

static struct hid_report *ish_get_feature(struct hid_sens_hub_device *hsdev,
	uint32_t report_id)
{
	struct ish_data *data = hid_get_drvdata(hsdev->hdev);
	struct hid_report *report;

	mutex_lock(&data->mutex);
	 report = ish_report(report_id, hsdev->hdev, HID_FEATURE_REPORT);
	if (!report)
		goto done_proc;

	hid_hw_request(hsdev->hdev, report, HID_REQ_GET_REPORT);
	hid_hw_wait(hsdev->hdev);

done_proc:
	mutex_unlock(&data->mutex);

	return report;
}

static int update_sensor_properties(struct sensor_def *sensor,
		struct hid_report *freport)
{
	uint32_t	usage_id;
	uint32_t	*pval;
	char	prop_val[MAX_PROP_VAL_LEN];
	int	i, j, is_string = 0;

	if (!freport)
		return -EINVAL;

	for (i = 0; i < freport->maxfield; ++i) {
		usage_id = freport->field[i]->usage->hid;

		is_string = (freport->field[i]->report_count > 1 &&
			freport->field[i]->report_size == 16);
		pval = freport->field[i]->value;
		memset(prop_val, 0, MAX_PROP_VAL_LEN);
		if (is_string) {
			for (j = 0; j < freport->field[i]->report_count; ++j)
				prop_val[j] = (char)pval[j];
			prop_val[j] = '\0';
		} else {
			sprintf(prop_val, "%d", *pval);/* null terminated */
		}

		for (j = 0; j < sensor->num_properties; ++j)
			if (sensor->properties[j].usage_id == usage_id) {
				kfree(sensor->properties[j].value);
				sensor->properties[j].value =
					kasprintf(GFP_KERNEL, "%s", prop_val);
				break;
			}
	}
	return 0;
}

static int hid_get_sens_properties(struct sensor_def *sensor)
{
	struct ish_data *sd;
	struct hid_report *freport;
	unsigned	idx;
	unsigned	report_id;
	int	rv;

	if (!sensor)
		return -EINVAL;	/* input is invalid */

	/* sensor hub device */
	idx = sensor->id >> 16 & 0xFFFF;
	sd = get_ish_by_index(idx);
	if (!sd)
		return	-EINVAL;	/* sensor->id is erroneous */

	/* Report ID */
	report_id = sensor->id & 0xFFFF;
	freport = ish_get_feature(sd->hsdev, report_id);
	if (!freport)
		return -EINVAL;
	rv = update_sensor_properties(sensor, freport);
	return	rv;
}

static int hid_set_sens_properties(struct list_head *set_prop_list,
		uint32_t sensor_id)
{
	unsigned	idx;
	struct ish_data	*sd;
	unsigned	report_id;
	int32_t val;
	int	field, rv = 0;
	struct hid_report *report;
	struct ish_data *data;
	struct sens_property *prop, *next;

	/* sensor hub device */
	idx = sensor_id >> 16 & 0xFFFF;
	sd = get_ish_by_index(idx);
	if (!sd)
		return -EINVAL;	/* sensor->id is erroneous */
	/* Report ID */
	report_id = sensor_id & 0xFFFF;

	data = hid_get_drvdata(sd->hsdev->hdev);
	mutex_lock(&data->mutex);
	report = ish_report(report_id, sd->hsdev->hdev, HID_FEATURE_REPORT);
	if (!report) {
		rv = -EINVAL;
		goto done_proc;
	}

	/* set all fields values */
	list_for_each_entry_safe(prop, next, set_prop_list, link) {
		field = get_field_index(sd->hsdev->hdev,
			report_id, prop->usage_id, HID_FEATURE_REPORT);
		if (field == -1 || field >= report->maxfield) {
			rv = -EINVAL;
			goto done_proc;
		}
		/* all values are numeric */
		rv = sscanf(prop->value, " %d ", &val);
		if (rv != 1) {
			rv = -EINVAL;
			goto done_proc;
		}
		hid_set_field(report->field[field], 0, val);
	}

	hid_hw_request(sd->hsdev->hdev, report, HID_REQ_SET_REPORT);
	hid_hw_wait(sd->hsdev->hdev);

	hid_hw_request(sd->hsdev->hdev, report, HID_REQ_GET_REPORT);
	hid_hw_wait(sd->hsdev->hdev);

	rv = 0;
done_proc:
	mutex_unlock(&data->mutex);

	return rv;
}

static int	hid_get_sample(struct sensor_def *sensor)
{
	unsigned	idx;
	struct ish_data	*sd;
	unsigned	report_id;
	struct	hid_report *report;
	int	ret = 0;

	if (!sensor)
		return -EINVAL;

	/* sensor hub device */
	idx = sensor->id >> 16 & 0xFFFF;
	sd = get_ish_by_index(idx);
	if (!sd)
		return	-EINVAL;	/* sensor->id is erroneous */

	/* Report ID */
	report_id = sensor->id & 0xFFFF;

	mutex_lock(&sd->mutex);
	report = ish_report(report_id, sd->hsdev->hdev,	HID_INPUT_REPORT);
	if (!report) {
		ret = -EINVAL;
		goto done_proc;
	}
	hid_hw_request(sd->hsdev->hdev, report, HID_REQ_GET_REPORT);

	/*
	 * The sample will arrive to "raw event" func,
	 * and will be pushed to user via "push_sample" method
	 */

done_proc:
	mutex_unlock(&sd->mutex);

	return	ret;
}

/*
 * Disable the given sensor.
 * set:
 * property_power_state =	3
 * property_reporting_state =	1
 * property_report_interval_resolution = 0
 */
static int	hid_disable_sensor(struct sensor_def *sensor)
{
	unsigned idx;
	struct ish_data *sd;
	unsigned report_id;
	struct hid_report *report;
	int field_idx, rv = 0;
	struct ish_data *data;

	idx = sensor->id >> 16 & 0xFFFF;
	sd = get_ish_by_index(idx);
	if (!sd)
		return -EINVAL;
	report_id = sensor->id & 0xFFFF;
	data = hid_get_drvdata(sd->hsdev->hdev);
	mutex_lock(&data->mutex);
	report = ish_report(report_id, sd->hsdev->hdev, HID_FEATURE_REPORT);
	if (!report) {
		rv = -EINVAL;
		goto done_proc;
	}

	/* property_power_state */
	field_idx = get_field_index(sd->hsdev->hdev, report_id,
		HID_USAGE_SENSOR_PROP_POWER_STATE, HID_FEATURE_REPORT);
	if (field_idx < 0) {
		rv = -EINVAL;
		goto done_proc;
	}
	hid_set_field(report->field[field_idx], 0, 3);

	/* property_reporting_state */
	field_idx = get_field_index(sd->hsdev->hdev, report_id,
		HID_USAGE_SENSOR_PROP_REPORT_STATE, HID_FEATURE_REPORT);
	if (field_idx < 0) {
		rv = -EINVAL;
		goto done_proc;
	}
	hid_set_field(report->field[field_idx], 0, 1);

	/* property_report_interval_resolution */
	field_idx = get_field_index(sd->hsdev->hdev, report_id,
		(HID_USAGE_SENSOR_PROP_REPORT_INTERVAL |
		HID_USAGE_SENSOR_MODIFIER_RESOLUTION), HID_FEATURE_REPORT);
	if (field_idx < 0) {
		rv = -EINVAL;
		goto done_proc;
	}
	hid_set_field(report->field[field_idx], 0, 0);

	hid_hw_request(sd->hsdev->hdev, report, HID_REQ_SET_REPORT);
	hid_hw_wait(sd->hsdev->hdev);

	hid_hw_request(sd->hsdev->hdev, report, HID_REQ_GET_REPORT);
	hid_hw_wait(sd->hsdev->hdev);

	rv = 0;
done_proc:
	mutex_unlock(&data->mutex);
	return rv;
}

/* ask for "get properties" but do not wait for FW response */
static int hid_ask_flush(struct sensor_def *sensor)
{
	struct ish_data *sd;
	struct hid_report *report;
	unsigned	idx;
	unsigned	report_id;
	int	ret = 0;

	if (!sensor)
		return -EINVAL;	/* input is invalid */

	/* sensor hub device */
	idx = sensor->id >> 16 & 0xFFFF;
	sd = get_ish_by_index(idx);
	if (!sd)
		return	-EINVAL;	/* sensor->id is erroneous */

	/* Report ID */
	report_id = sensor->id & 0xFFFF;

	mutex_lock(&sd->mutex);
	report = ish_report(report_id, sd->hsdev->hdev, HID_FEATURE_REPORT);
	if (!report) {
		ret = -EINVAL;
		goto done_proc;
	}

	hid_hw_request(sd->hsdev->hdev, report, HID_REQ_GET_REPORT);

done_proc:
	mutex_unlock(&sd->mutex);
	return ret;
}

struct senscol_impl	hid_senscol_impl = {
	.get_sample = hid_get_sample,
#if SENSCOL_1
	.get_sens_property = hid_get_sens_property,
	.set_sens_property = hid_set_sens_property,
#endif
	.get_sens_properties = hid_get_sens_properties,
	.set_sens_properties = hid_set_sens_properties,
	.disable_sensor = hid_disable_sensor,
	.ask_flush = hid_ask_flush
};

static int	is_sens_data_field(unsigned usage)
{
	if ((usage >= 0x400 && usage <= 0x49F) ||
			(usage >= 0x4B0 && usage <= 0x4DF) ||
			(usage >= 0x4F0 && usage <= 0x4F7) ||
			(usage >= 0x500 && usage <= 0x52F) ||
			(usage >= 0x540 && usage <= 0x57F) ||
			(usage >= 590 && usage <= 0x7FF))
		return	1;
	return	0;
}

/* Handle raw report as sent by device */
static int ish_raw_event(struct hid_device *hdev,
		struct hid_report *report, uint8_t *raw_data, int size)
{
	struct ish_data *pdata = hid_get_drvdata(hdev);
	int	i, sz;
	uint8_t	*ptr;
	uint32_t	sensor_id;
	static unsigned char	*data_buf;
	unsigned	sample_size;

	/* make up senscol id */
	sensor_id = pdata->ish_index << 16 | (report->id & 0xFFFF);

	if (report->type == HID_FEATURE_REPORT) { /* it indicates a flush
							complete event */
#if SENSCOL_1
		senscol_flush_cb_legacy(sensor_id);
#else
		senscol_flush_cb(sensor_id);
#endif
		return 0;
	}

	data_buf = kmalloc(size, GFP_KERNEL);
	if (!data_buf)
		return -ENOMEM;

	ptr = raw_data;
	ptr++; /* Skip report id */

	sample_size = 0;

	for (i = 0; i < report->maxfield; ++i) {
		sz = (report->field[i]->report_size *
					report->field[i]->report_count)/8;

		/* Prepare data for senscol sample */
		if (is_sens_data_field(report->field[i]->usage->hid & 0xFFFF)) {
			memcpy(data_buf + sample_size, ptr, sz);
			sample_size += sz;
		}
		ptr += sz;
	}

	/* Upstream sample to sensor collection framework */
#if SENSCOL_1
	push_sample_legacy(sensor_id, data_buf);
#else
	push_sample(sensor_id, data_buf);
#endif
	kfree(data_buf);
	return 1;
}

static uint8_t *ish_report_fixup(struct hid_device *hdev, uint8_t *rdesc,
		unsigned int *rsize)
{
	int index;
	unsigned char report_block[] = {
				0x0a, 0x16, 0x03, 0x15, 0x00, 0x25, 0x05};
	unsigned char power_block[] = {
				0x0a, 0x19, 0x03, 0x15, 0x00, 0x25, 0x05};

	/* Looks for power and report state usage id and force to 1 */
	for (index = 0; index < *rsize; ++index) {
		if (((*rsize - index) > sizeof(report_block)) &&
			!memcmp(&rdesc[index], report_block,
						sizeof(report_block))) {
			rdesc[index + 4] = 0x01;
			index += sizeof(report_block);
		}
		if (((*rsize - index) > sizeof(power_block)) &&
			!memcmp(&rdesc[index], power_block,
						sizeof(power_block))) {
			rdesc[index + 4] = 0x01;
			index += sizeof(power_block);
		}
	}

	return rdesc;
}

static void translate_logical_physical_usage(struct hid_report_enum *rep_enum)
{
	struct hid_report	*report;
	struct hid_field	*field;
	int	j, i;

	list_for_each_entry(report, &rep_enum->report_list, list) {
		for (j = 0; j < report->maxfield; ++j) {
			field = report->field[j];
			if (!(field->flags & HID_MAIN_ITEM_VARIABLE))
				for (i = 0; i < field->maxusage; ++i)
					field->usage[i].hid = field->logical;
		}
	}
}

static int	add_data_fields(struct hid_report *report,
				struct sensor_def *senscol_sensor)
{
	struct hid_field	*field;
	struct data_field	data_field;
	const char	*usage_name;
	char	*name;
	unsigned	usage;
	int	i, rv = 0;

	for (i = 0; i < report->maxfield; ++i) {
		field = report->field[i];
		usage = field->usage[0].hid;
		if (!is_sens_data_field(usage & 0xFFFF))
			continue;

		memset(&data_field, 0, sizeof(struct data_field));
		usage_name = senscol_usage_to_name(usage & 0xFFFF);
		if (usage_name)
			name = kasprintf(GFP_KERNEL, "%s", usage_name);
		else
			name = kasprintf(GFP_KERNEL, "data-%X", usage);
		if (!name)
			return	-ENOMEM;

		data_field.name = name;
		data_field.usage_id = usage;
		data_field.len = (field->report_size >> 3) *
			field->report_count;
		data_field.is_numeric = (field->flags & HID_MAIN_ITEM_VARIABLE);
		if (data_field.is_numeric) {
			if (field->unit_exponent > 7 ||
					field->unit_exponent < -8)
				data_field.exp = 0xFF;
			else if (field->unit_exponent >= 0)
				data_field.exp = field->unit_exponent;
			else
				data_field.exp = 0x10 - field->unit_exponent;
			data_field.unit = field->unit;
		}

		rv = add_data_field(senscol_sensor, &data_field);
		if (rv)
			return rv;
		senscol_sensor->sample_size += (field->report_size >> 3) *
			field->report_count;
	}
	return	0;
}

static int	add_properties(struct hid_report *report,
				struct sensor_def *senscol_sensor)
{
	struct hid_field	*field;
	struct sens_property	prop_field;
	unsigned	usage;
	int	i, rv;

	for (i = 0; i < report->maxfield; ++i) {
		field = report->field[i];
		usage = field->usage[0].hid;

		memset(&prop_field, 0, sizeof(struct sens_property));
		prop_field.name = create_sens_prop_name(usage & 0xFFFF);
		if (!prop_field.name)
			return -ENOMEM;

		prop_field.usage_id = usage;
		prop_field.is_string = (field->flags & HID_MAIN_ITEM_VARIABLE)
			&& field->report_count > 1 && field->report_size == 16;

		rv = add_sens_property(senscol_sensor,	&prop_field);
		if (rv)
			return rv;
	}
	return 0;
}

/*
 * release allocated memory of senscol_sensor.
 * cannot use the senscol-core remove function because the sensor
 * was not yet added to sensors list
 */
static void free_senscol_sensor(struct sensor_def *senscol_sensor)
{
	int	i;

	for (i = 0; i < senscol_sensor->num_properties; ++i) {
		kfree(senscol_sensor->properties[i].name);
		kfree(senscol_sensor->properties[i].value);
	}
	kfree(senscol_sensor->properties);

	for (i = 0; i < senscol_sensor->num_data_fields; ++i)
		kfree(senscol_sensor->data_fields[i].name);
	kfree(senscol_sensor->data_fields);

	kfree(senscol_sensor->name);
	kfree(senscol_sensor);
}

static struct sensor_def	*create_sensor_from_report(int dev_idx,
		struct hid_report *inp_report, struct hid_report *feat_report)
{
	struct sensor_def	*senscol_sensor;
	struct hid_field	*inp_field = inp_report->field[0];
	int	ret;

	senscol_sensor = alloc_senscol_sensor();
	if (!senscol_sensor)
		return NULL;
#if SENSCOL_1
	init_senscol_sensor_legacy(senscol_sensor);
#else
	init_senscol_sensor(senscol_sensor);
#endif
	senscol_sensor->name = create_sensor_name(inp_field->physical & 0xFFFF);
	if (!senscol_sensor->name)
		goto err_free;

	senscol_sensor->usage_id = inp_field->physical;
	senscol_sensor->id = dev_idx << 16 | (inp_report->id & 0xFFFF);
	senscol_sensor->impl = &hid_senscol_impl;
	senscol_sensor->sample_size = 0;

	ret = add_data_fields(inp_report, senscol_sensor);
	if (ret)
		goto err_free;

	ret = add_properties(feat_report, senscol_sensor);
	if (ret)
		goto err_free;

	return senscol_sensor;
err_free:
	free_senscol_sensor(senscol_sensor);
	return NULL;
}

static int ish_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct ish_data	*sd;
	struct hid_report_enum	*inp_report_enum, *feat_report_enum;
	struct hid_report	*inp_report, *feat_report;
	struct hid_field	*inp_field, *feat_field;
	struct sensor_def	*senscol_sensor;
	int	dev_cnt, ret = 0;

	sd = devm_kzalloc(&hdev->dev, sizeof(*sd), GFP_KERNEL);
	if (!sd) {
		hid_err(hdev, "cannot allocate sensor data structure\n");
		return -ENOMEM;
	}
	sd->hsdev = devm_kzalloc(&hdev->dev, sizeof(*sd->hsdev), GFP_KERNEL);
	if (!sd->hsdev) {
		hid_err(hdev, "cannot allocate hid_sens_hub_device\n");
		ret = -ENOMEM;
		goto err_free_hub;
	}
	hid_set_drvdata(hdev, sd);
	/* Keep array of HID sensor hubs for senscol_impl usage */
	hid_sens_hubs[ish_cur_count] = hdev;
	/* Need to count sensor hub devices for senscol ids */
	sd->ish_index = ish_cur_count++;

	sd->hsdev->hdev = hdev;
	sd->hsdev->vendor_id = hdev->vendor;
	sd->hsdev->product_id = hdev->product;
	mutex_init(&sd->mutex);
	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed\n");
		goto err_free;
	}
	INIT_LIST_HEAD(&hdev->inputs);

	inp_report_enum = &hdev->report_enum[HID_INPUT_REPORT];
	feat_report_enum = &hdev->report_enum[HID_FEATURE_REPORT];

	dev_cnt = ish_get_physical_device_count(inp_report_enum);
	if (dev_cnt > HID_MAX_PHY_DEVICES) {
		hid_err(hdev, "Invalid physical device count\n");
		ret = -EINVAL;
		goto err_stop_hw;
	}

	translate_logical_physical_usage(feat_report_enum);
	translate_logical_physical_usage(inp_report_enum);

	list_for_each_entry(inp_report, &inp_report_enum->report_list, list) {
		hid_dbg(hdev, "Report id:%x\n", inp_report->id);
		inp_field = inp_report->field[0];

		/* find matching feature report to add properties */
		list_for_each_entry(feat_report, &feat_report_enum->report_list,
				list) {
			feat_field = feat_report->field[0];
			if (feat_report->maxfield && feat_field &&
					feat_field->physical &&
					(feat_field->physical ==
					inp_field->physical) &&
					feat_report->id == inp_report->id)
				break;
		}
		senscol_sensor = create_sensor_from_report(sd->ish_index,
			inp_report, feat_report);
		if (!senscol_sensor) {
			dev_err(&hdev->dev, "failed to create sensor\n");
			ret = -ENOMEM;
			goto err_stop_hw;
		}

		/* Add the filled senscol_sensor */
#if SENSCOL_1
		ret = add_senscol_sensor_legacy(senscol_sensor);
#else
		ret = add_senscol_sensor(senscol_sensor);
#endif
		if (ret) {
			dev_err(&hdev->dev, "Failed to add senscol sensor\n");
			goto err_stop_hw;
		}
	}

	if (ish_cur_count >= ish_max_count)
#if SENSCOL_1
		senscol_send_ready_event_legacy();
#else
		senscol_send_ready_event();
#endif
	if (ish_cur_count > ish_max_count)
		ish_max_count = ish_cur_count;

	return 0;

err_stop_hw:
	hid_hw_stop(hdev);
err_free:
	kfree(sd->hsdev);
err_free_hub:
	kfree(sd);

	return ret;
}

static void ish_remove(struct hid_device *hdev)
{
	struct ish_data	*data = hid_get_drvdata(hdev);
	uint32_t	hid_device_id;
	struct hid_report	*report;
	int	i;

	/* use max count since we can't know the remove order of hid devices */
	for (i = 0; i < ish_max_count; ++i)
		if (hid_sens_hubs[i] == hdev) {
			hid_sens_hubs[i] = NULL;
			break;
		}

	hid_hw_close(hdev);
	hid_hw_stop(hdev);

	hid_device_id = data->ish_index;
	list_for_each_entry(report,
			&hdev->report_enum[HID_INPUT_REPORT].report_list,
			list) {
#if SENSCOL_1
		remove_senscol_sensor_legacy(data->ish_index << 16 |
			(report->id & 0xFFFF));
#else
		remove_senscol_sensor(data->ish_index << 16 |
			(report->id & 0xFFFF));
#endif
	}

	hid_set_drvdata(hdev, NULL);
	mutex_destroy(&data->mutex);
	ish_cur_count--;
	if (ish_cur_count == ish_max_count - 1)
#if SENSCOL_1
		senscol_reset_notify_legacy();
#else
		senscol_reset_notify();
#endif
}

static const struct hid_device_id ish_devices[] = {
	{ HID_DEVICE(HID_BUS_ANY, HID_GROUP_SENSOR_HUB, USB_VENDOR_ID_INTEL_0,
			USB_DEVICE_ID_INTEL_HID_SENSOR)},
	{ HID_DEVICE(HID_BUS_ANY, HID_GROUP_SENSOR_HUB, USB_VENDOR_ID_INTEL_1,
			USB_DEVICE_ID_INTEL_HID_SENSOR)},
	{ HID_DEVICE(HID_BUS_ANY, HID_GROUP_SENSOR_HUB, USB_VENDOR_ID_STM_0,
			USB_DEVICE_ID_STM_HID_SENSOR)},
	{ HID_DEVICE(HID_BUS_ANY, HID_GROUP_SENSOR_HUB, USB_VENDOR_ID_STM_0,
			USB_DEVICE_ID_STM_HID_SENSOR_1)},
	{ HID_DEVICE(HID_BUS_ANY, HID_GROUP_SENSOR_HUB, HID_ANY_ID,
			HID_ANY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, ish_devices);

static struct hid_driver ish_driver = {
	.name = "ish-hid-dev",
	.id_table = ish_devices,
	.probe = ish_probe,
	.remove = ish_remove,
	.raw_event = ish_raw_event,
	.report_fixup = ish_report_fixup,
#ifdef CONFIG_PM
	.suspend = ish_suspend,
	.resume = ish_resume,
	.reset_resume = ish_reset_resume,
#endif /* CONFIG_PM */
};

static int __init ish_driver_init(void)
{
	int ret;

#if SENSCOL_1
	ret = senscol_init_legacy();
#else
	ret = senscol_init();
#endif
	if (ret)
		return ret;

	return hid_register_driver(&ish_driver);
}
late_initcall(ish_driver_init);

static void __exit ish_driver_exit(void)
{
	hid_unregister_driver(&ish_driver);
#if SENSCOL_1
	senscol_exit_legacy();
#endif
}
module_exit(ish_driver_exit);

MODULE_DESCRIPTION("ISH HID driver");
MODULE_LICENSE("GPL");
