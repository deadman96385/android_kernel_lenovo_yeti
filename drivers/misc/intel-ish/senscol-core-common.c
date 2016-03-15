/*
 * Sensor collection interface
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

#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/senscol/senscol-core.h>
#include "hid-strings-def.h"

struct list_head	senscol_sensors_list;
spinlock_t	senscol_lock;

const char	*senscol_usage_to_name(unsigned usage)
{
	int i;

	for (i = 0; code_msg_arr[i].msg && code_msg_arr[i].code != usage; i++)
		;
	return code_msg_arr[i].msg;
}

const char	*senscol_get_modifier(unsigned modif)
{
	uint32_t to4bits = modif >> 0xC;
	return	modifiers[to4bits];
}

char	*create_sensor_name(unsigned usage_id)
{
	const char	*usage_name;

	usage_name = senscol_usage_to_name(usage_id & 0xFFFF);
	if (usage_name)
		return kasprintf(GFP_KERNEL, "%s", usage_name);

	return kasprintf(GFP_KERNEL, "custom-%X", usage_id);
}

char	*create_sens_prop_name(unsigned usage_id)
{
	const char	*usage_name;
	uint32_t	modifier;
	uint32_t	data_hid;
	const char	*modif_name;

	usage_name = senscol_usage_to_name(usage_id & 0xFFFF);
	if (usage_name)
		/* a regular property */
		return kasprintf(GFP_KERNEL, "%s", usage_name);

	modifier = usage_id & 0xF000;
	data_hid = usage_id & 0x0FFF;
	modif_name = senscol_get_modifier(modifier);
	usage_name = senscol_usage_to_name(data_hid);
	if (!strcmp(modif_name, "custom"))
		/* a private (custom) property */
		return kasprintf(GFP_KERNEL, "custom-%X", usage_id & 0xFFFF);

	if (!usage_name)
		/* unknown property */
		return kasprintf(GFP_KERNEL, "unknown-%X", usage_id & 0xFFFF);

	/* a modifier related to another property */
	return kasprintf(GFP_KERNEL, "%s_%s", usage_name, modif_name);
}

/* Only allocates new sensor */
struct sensor_def	*alloc_senscol_sensor(void)
{
	struct sensor_def *sens;

	sens = kzalloc(sizeof(struct sensor_def), GFP_KERNEL);
	return	sens;
}

/* should be called under senscol_lock */
struct sensor_def	*get_senscol_sensor_by_id(uint32_t id)
{
	struct sensor_def	*sens, *next;

	list_for_each_entry_safe(sens, next, &senscol_sensors_list, link) {
		if (sens->id == id)
			return	sens;
	}
	return	NULL;
}

/* Add data field to existing sensor */
int	add_data_field(struct sensor_def *sensor, struct data_field *data)
{
	struct data_field	*temp;

	temp = krealloc(sensor->data_fields,
		(sensor->num_data_fields + 1) * sizeof(struct data_field),
		GFP_KERNEL);
	if (!temp)
		return	-ENOMEM;

	data->sensor = sensor;
	memcpy(&temp[sensor->num_data_fields++], data,
		sizeof(struct data_field));
	sensor->data_fields = temp;
	return	0;
}

/* Add property to existing sensor */
int	add_sens_property(struct sensor_def *sensor, struct sens_property *prop)
{
	struct sens_property	*temp;

	temp = krealloc(sensor->properties,
		(sensor->num_properties + 1) * sizeof(struct sens_property),
		GFP_KERNEL);
	if (!temp)
		return	-ENOMEM;

	prop->sensor = sensor;		/* The needed backlink */
	memcpy(&temp[sensor->num_properties++], prop,
		sizeof(struct sens_property));
	sensor->properties = temp;
	return	0;
}
