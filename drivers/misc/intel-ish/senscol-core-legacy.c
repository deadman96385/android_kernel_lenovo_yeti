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
#include <linux/senscol/senscol-core.h>

#if SENSCOL_1

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/kobject.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include "platform-config.h"
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include "ishtp-hid.h"

spinlock_t	senscol_data_lock;
uint8_t	*senscol_data_buf;
unsigned	senscol_data_head, senscol_data_tail;
int	flush_asked = 0;

wait_queue_head_t	senscol_read_wait;
wait_queue_head_t	senscol_init_wait;
int	senscol_init_done = 0;

struct task_struct	*user_task = NULL;

static struct platform_device	*sc_pdev;

void senscol_send_ready_event_legacy(void)
{
	if (waitqueue_active(&senscol_read_wait))
		wake_up_interruptible(&senscol_read_wait);
}

int senscol_reset_notify_legacy(void)
{

	struct siginfo si;
	int ret;

	memset(&si, 0, sizeof(struct siginfo));
	si.si_signo = SIGUSR1;
	si.si_code = SI_USER;

	if (user_task == NULL)
		return -EINVAL;

	ret = send_sig_info(SIGUSR1, &si, user_task);
	return ret;
}

/* data node definition */
static void	sc_data_release(struct kobject *k)
{
}
static ssize_t	sc_data_show(struct kobject *kobj, struct attribute *attr,
	char *buf)
{
	scnprintf(buf, PAGE_SIZE, "%s\n", attr->name);
	return	strlen(buf);
}

static ssize_t	sc_data_store(struct kobject *kobj, struct attribute *attr,
	const char *buf, size_t size)
{
	return	size;
}

const struct sysfs_ops	sc_data_sysfs_fops = {
	.show = sc_data_show,
	.store = sc_data_store
};

struct kobj_type	sc_data_kobj_type = {
	.release = sc_data_release,
	.sysfs_ops = &sc_data_sysfs_fops
};

struct kobject	sc_data_kobj;

static ssize_t	sensors_data_read(struct file *f, struct kobject *kobj,
	struct bin_attribute *bin_attr, char *buf, loff_t offs, size_t size);

static ssize_t	sensors_data_write(struct file *f, struct kobject *kobj,
	struct bin_attribute *bin_attr, char *buf, loff_t offs, size_t size);

struct bin_attribute	sensors_data_binattr = {
	.attr = {
		.name = "sensors_data",
		.mode = S_IRUGO
	},
	.size = 0,
	.read = sensors_data_read,
	.write = sensors_data_write
};
/************************/

/*
 * sensor_def kobject type and handlers
 */
static struct attribute	sc_sensdef_defattr_name = {
	.name = "name",
	.mode = (S_IRUGO)
};

static struct attribute sc_sensdef_defattr_id = {
	.name = "id",
	.mode = (S_IRUGO)
};

static struct attribute sc_sensdef_defattr_usage_id = {
	.name = "usage_id",
	.mode = (S_IRUGO)
};

static struct attribute sc_sensdef_defattr_sample_size = {
	.name = "sample_size",
	.mode = (S_IRUGO)
};

static struct attribute sc_sensdef_defattr_flush = {
	.name = "flush",
	.mode = (S_IRUGO)
};

static struct attribute sc_sensdef_defattr_get_sample = {
	.name = "get_sample",
	.mode = (S_IRUGO)
};

struct attribute	*sc_sensdef_defattrs[] = {
	&sc_sensdef_defattr_name,
	&sc_sensdef_defattr_id,
	&sc_sensdef_defattr_usage_id,
	&sc_sensdef_defattr_sample_size,
	&sc_sensdef_defattr_flush,
	&sc_sensdef_defattr_get_sample,
	NULL
};

static void	sc_sensdef_release(struct kobject *k)
{
}

static ssize_t	sc_sensdef_show(struct kobject *kobj, struct attribute *attr,
	char *buf)
{
	ssize_t	rv;
	struct sensor_def	*sensdef;
	static char	tmp_buf[0x1000];
	unsigned long flags;

	sensdef = container_of(kobj, struct sensor_def, kobj);
	buf[0] = '\0';
	if (!strcmp(attr->name, "id"))
		scnprintf(buf, PAGE_SIZE, "%08X\n", sensdef->id);
	else if (!strcmp(attr->name, "sample_size"))
		scnprintf(buf, PAGE_SIZE, "%u\n", sensdef->sample_size);
	else if (!strcmp(attr->name, "usage_id"))
		scnprintf(buf, PAGE_SIZE, "%08X\n", sensdef->usage_id);
	else if (!strcmp(attr->name, "name"))
		scnprintf(buf, PAGE_SIZE, "%s\n", sensdef->name);
	else if (!strcmp(attr->name, "flush")) {
		spin_lock_irqsave(&senscol_lock, flags);
		flush_asked = 1;
		sensdef->flush_req = 1;
		spin_unlock_irqrestore(&senscol_lock, flags);
		sensdef->impl->get_sens_property(sensdef,
			sensdef->properties, tmp_buf, 0x1000);
		scnprintf(buf, PAGE_SIZE, "1\n");
	} else if (!strcmp(attr->name, "get_sample")) {
		rv = sensdef->impl->get_sample(sensdef);
		/* The sample will arrive to hid "raw event" func,
		and will be pushed to user via "push_sample" method */

		scnprintf(buf, PAGE_SIZE, "%d\n", !rv);
	}
	rv = strlen(buf) + 1;
	return	rv;
}

static ssize_t	sc_sensdef_store(struct kobject *kobj, struct attribute *attr,
	const char *buf, size_t size)
{
	return	-EINVAL;
}
const struct sysfs_ops	sc_sensdef_sysfs_fops = {
	.show = sc_sensdef_show,
	.store = sc_sensdef_store
};

struct kobj_type	sc_sensdef_kobj_type = {
	.release = sc_sensdef_release,
	.sysfs_ops = &sc_sensdef_sysfs_fops,
	.default_attrs = sc_sensdef_defattrs
};
/*****************************************/

/*
 * kobject type for empty sub-directories
 */
static struct attribute	*sc_subdir_defattrs[] = {
	NULL
};

static void	sc_subdir_release(struct kobject *k)
{
}

static ssize_t	sc_subdir_show(struct kobject *kobj, struct attribute *attr,
	char *buf)
{
	return	-EINVAL;
}

static ssize_t	sc_subdir_store(struct kobject *kobj, struct attribute *attr,
	const char *buf, size_t size)
{
	return	-EINVAL;
}

const struct sysfs_ops	sc_subdir_sysfs_fops = {
	.show = sc_subdir_show,
	.store = sc_subdir_store
};

struct kobj_type	sc_subdir_kobj_type = {
	.release = sc_subdir_release,
	.sysfs_ops = &sc_subdir_sysfs_fops,
	.default_attrs = sc_subdir_defattrs
};
/*****************************************/

/*
 * sensors 'data_field's kobject type
 */
static struct attribute	sc_datafield_defattr_usage_id = {
	.name = "usage_id",
	.mode = (S_IRUGO)
};

static struct attribute	sc_datafield_defattr_exp = {
	.name = "exponent",
	.mode = (S_IRUGO)
};

static struct attribute sc_datafield_defattr_len = {
	.name = "length",
	.mode = (S_IRUGO)
};

static struct attribute sc_datafield_defattr_unit = {
	.name = "unit",
	.mode = (S_IRUGO)
};

static struct attribute sc_datafield_defattr_index = {
	.name = "index",
	.mode = (S_IRUGO)
};

static struct attribute	sc_datafield_defattr_is_numeric = {
	.name = "is_numeric",
	.mode = (S_IRUGO)
};

struct attribute	*sc_datafield_defattrs[] = {
	&sc_datafield_defattr_usage_id,
	&sc_datafield_defattr_exp,
	&sc_datafield_defattr_len,
	&sc_datafield_defattr_unit,
	&sc_datafield_defattr_index,
	&sc_datafield_defattr_is_numeric,
	NULL
};

static void	sc_datafield_release(struct kobject *k)
{
}

static ssize_t	sc_datafield_show(struct kobject *kobj, struct attribute *attr,
	char *buf)
{
	ssize_t	rv;
	struct data_field	*dfield;

	dfield = container_of(kobj, struct data_field, kobj);
	if (!strcmp(attr->name, "usage_id"))
		scnprintf(buf, PAGE_SIZE, "%08X\n", (unsigned)dfield->usage_id);
	else if (!strcmp(attr->name, "exponent"))
		scnprintf(buf, PAGE_SIZE, "%u\n", (unsigned)dfield->exp);
	else if (!strcmp(attr->name, "length"))
		scnprintf(buf, PAGE_SIZE, "%u\n", (unsigned)dfield->len);
	else if (!strcmp(attr->name, "unit"))
		scnprintf(buf, PAGE_SIZE, "%u\n", (unsigned)dfield->unit);
	else if (!strcmp(attr->name, "index"))
		scnprintf(buf, PAGE_SIZE, "%u\n", (unsigned)dfield->index);
	else if (!strcmp(attr->name, "is_numeric"))
		scnprintf(buf, PAGE_SIZE, "%u\n", (unsigned)dfield->is_numeric);

	rv = strlen(buf) + 1;
	return	rv;
}

static ssize_t	sc_datafield_store(struct kobject *kobj, struct attribute *attr,
	const char *buf, size_t size)
{
	return	-EINVAL;
}

const struct sysfs_ops	sc_datafield_sysfs_fops = {
	.show = sc_datafield_show,
	.store = sc_datafield_store
};

struct kobj_type	sc_datafield_kobj_type = {
	.release = sc_datafield_release,
	.sysfs_ops = &sc_datafield_sysfs_fops,
	.default_attrs = sc_datafield_defattrs
};
/*****************************************/

/*
 * sensors 'properties' kobject type
 */
static struct attribute sc_sensprop_defattr_value = {
	.name = "value",
	.mode = (S_IRUGO | S_IWUSR | S_IWGRP)
};

static struct attribute sc_sensprop_defattr_usage_id = {
	.name = "usage_id",
	.mode = (S_IRUGO | S_IWUSR | S_IWGRP)
};

struct attribute	*sc_sensprop_defattrs[] = {
	&sc_sensprop_defattr_value,
	&sc_sensprop_defattr_usage_id,
	NULL
};

static void	sc_sensprop_release(struct kobject *k)
{
}

static ssize_t	sc_sensprop_show(struct kobject *kobj, struct attribute *attr,
	char *buf)
{
	struct sens_property	*pfield;
	struct sensor_def	*sensor;
	int	rv = 0;

	/*
	 * We need "property_power_state" (=2), "property_reporting_state" (=2)
	 * and "property_report_interval" (in ms?)
	 */
	pfield = container_of(kobj, struct sens_property, kobj);
	sensor = pfield->sensor;

	if (!strcmp(attr->name, "value"))
		rv = sensor->impl->get_sens_property(sensor, pfield, buf,
			0x1000);
	else if (!strcmp(attr->name, "usage_id"))
		scnprintf(buf, PAGE_SIZE, "%08X\n", pfield->usage_id & 0xFFFF);
	if (rv)
		return	rv;
	return	strlen(buf);
}

static ssize_t	sc_sensprop_store(struct kobject *kobj, struct attribute *attr,
	const char *buf, size_t size)
{
	struct sens_property	*pfield;
	struct sensor_def	*sensor;
	int	rv;

	if (strcmp(attr->name, "value"))
		return -EINVAL;

	pfield = container_of(kobj, struct sens_property, kobj);
	sensor = pfield->sensor;
	rv = sensor->impl->set_sens_property(sensor, pfield, buf);

	if (rv)
		return	rv;
	return	size;
}

const struct sysfs_ops	sc_sensprop_sysfs_fops = {
	.show = sc_sensprop_show,
	.store = sc_sensprop_store
};

struct kobj_type	sc_sensprop_kobj_type = {
	.release = sc_sensprop_release,
	.sysfs_ops = &sc_sensprop_sysfs_fops,
	.default_attrs = sc_sensprop_defattrs
};
/*****************************************/

static ssize_t	sensors_data_read(struct file *f, struct kobject *kobj,
	struct bin_attribute *bin_attr, char *buf, loff_t offs, size_t size)
{
	size_t	count;
	unsigned	cur;
	struct senscol_sample	*sample;
	unsigned long	flags;

	if (size > PAGE_SIZE)
		size = PAGE_SIZE;

	spin_lock_irqsave(&senscol_data_lock, flags);

	/*
	 * Count how much we may copy, keeping whole samples.
	 * Copy samples along the way
	 */
	count = 0;
	cur = senscol_data_head;
	while (cur != senscol_data_tail) {
		sample = (struct senscol_sample *)(senscol_data_buf + cur);
		if (count + sample->size > size)
			break;
		memcpy(buf + count, sample, sample->size);
		count += sample->size;
		cur += sample->size;
		if (cur > SENSCOL_DATA_BUF_LAST)
			cur = 0;
	}
	senscol_data_head = cur;

	spin_unlock_irqrestore(&senscol_data_lock, flags);

	return	count;
}

static ssize_t	sensors_data_write(struct file *f, struct kobject *kobj,
	struct bin_attribute *bin_attr, char *buf, loff_t offs, size_t size)
{
	return	-EINVAL;
}

/* Init sensor (don't call for initialized sensors) */
void	init_senscol_sensor_legacy(struct sensor_def *sensor)
{
	if (!sensor)
		return;
	memset(sensor, 0, sizeof(*sensor));
	sensor->name = NULL;
	sensor->friendly_name = NULL;
	sensor->impl = NULL;
	sensor->data_fields = NULL;
	sensor->properties = NULL;
}

int remove_senscol_sensor_legacy(uint32_t id)
{
	unsigned long	flags;
	struct sensor_def	*sens, *next;
	int i;

	spin_lock_irqsave(&senscol_lock, flags);
	list_for_each_entry_safe(sens, next, &senscol_sensors_list, link) {
		if (sens->id == id) {
			list_del(&sens->link);
			spin_unlock_irqrestore(&senscol_lock, flags);

			for (i = 0; i < sens->num_properties; ++i)
				if (sens->properties[i].name) {
					kobject_put(&sens->properties[i].kobj);
					kobject_del(&sens->properties[i].kobj);
				}
			kfree(sens->properties);
			kobject_put(&sens->props_kobj);
			kobject_del(&sens->props_kobj);

			for (i = 0; i < sens->num_data_fields; ++i)
				if (sens->data_fields[i].name) {
					kobject_put(&sens->data_fields[i].kobj);
					kobject_del(&sens->data_fields[i].kobj);
				}
			kfree(sens->data_fields);
			kobject_put(&sens->data_fields_kobj);
			kobject_del(&sens->data_fields_kobj);
			kobject_put(&sens->kobj);
			kobject_del(&sens->kobj);

			kfree(sens);

			return 0;
		}
	}
	spin_unlock_irqrestore(&senscol_lock, flags);

	return -EINVAL;
}

/*
 * Exposed sensor via sysfs, structure may be static
 *
 * The caller is responsible for setting all meaningful fields
 * (may call add_data_field() and add_sens_property() as needed)
 * We'll consider hiding senscol framework-specific fields
 * into opaque structures
 */

int	add_senscol_sensor_legacy(struct sensor_def *sensor)
{
	unsigned long	flags;
	char	sensor_name[256];	/* Enough for name "sensor_<NN>_def",
					 * if convention changes array size
					 * should be reviewed */
	int	i;
	int	rv;
	int	j;

	if (!sensor->name || !sensor->impl || !sensor->usage_id || !sensor->id)
		return	-EINVAL;

	spin_lock_irqsave(&senscol_lock, flags);
	list_add_tail(&sensor->link, &senscol_sensors_list);
	spin_unlock_irqrestore(&senscol_lock, flags);

	/*
	 * Create sysfs entries for this sensor
	 */

	/* Init and add sensor_def kobject */
	snprintf(sensor_name, sizeof(sensor_name), "sensor_%X_def", sensor->id);
	rv = kobject_init_and_add(&sensor->kobj, &sc_sensdef_kobj_type,
		&sc_pdev->dev.kobj, sensor_name);
	if (rv) {
		rv = -EFAULT;
err_ret:
		kobject_put(&sensor->kobj);
		kobject_del(&sensor->kobj);
		return	rv;
	}

	/*
	 * Create kobjects without attributes for
	 * sensor_<NN>_def/data_fields and sensor_<NN>/properties
	 */
	rv = kobject_init_and_add(&sensor->data_fields_kobj,
		&sc_subdir_kobj_type, &sensor->kobj, "data_fields");
	if (rv) {
		rv = -EFAULT;
err_ret2:
		kobject_put(&sensor->data_fields_kobj);
		kobject_del(&sensor->data_fields_kobj);
		goto	err_ret;
	}

	rv = kobject_init_and_add(&sensor->props_kobj, &sc_subdir_kobj_type,
		&sensor->kobj, "properties");
	if (rv) {
		rv = -EFAULT;
		kobject_put(&sensor->props_kobj);
		kobject_del(&sensor->props_kobj);
		goto	err_ret2;
	}

	/*
	 * Create kobjects for data_fields
	 */
	for (i = 0; i < sensor->num_data_fields; ++i)
		if (sensor->data_fields[i].name)
			for (j = i-1; j >= 0; --j)	/* use index as a temp
							variable */
				if (sensor->data_fields[j].name &&
					!strcmp(sensor->data_fields[i].name,
						sensor->data_fields[j].name)) {
					if (!sensor->data_fields[j].index)
						sensor->data_fields[j].index++;
					sensor->data_fields[i].index =
						sensor->data_fields[j].index
									+ 1;
					break;
				}

	for (i = 0; i < sensor->num_data_fields; ++i) {
		if (sensor->data_fields[i].name) {
			if (sensor->data_fields[i].index) {
				char *p = kasprintf(GFP_KERNEL, "%s#%d",
					sensor->data_fields[i].name,
					sensor->data_fields[i].index-1);
				kfree(sensor->data_fields[i].name);
				sensor->data_fields[i].name = p;
			}

			/* Mark index */
			sensor->data_fields[i].index = i;

			rv = kobject_init_and_add(&sensor->data_fields[i].kobj,
				&sc_datafield_kobj_type,
				&sensor->data_fields_kobj,
				sensor->data_fields[i].name);
		}
	}

	/*
	 * Create kobjects for properties
	 */
	for (i = 0; i < sensor->num_properties; ++i) {
		if (sensor->properties[i].name) {
			rv = kobject_init_and_add(&sensor->properties[i].kobj,
				&sc_sensprop_kobj_type, &sensor->props_kobj,
				sensor->properties[i].name);
		}
	}

	/* Sample size should be set by the caller to size of raw data */
	sensor->sample_size += offsetof(struct senscol_sample, data);

	return	0;
}

/*
 * Push data sample in upstream buffer towards user-mode.
 * Sample's size is determined from the structure
 *
 * Samples are queued is a simple FIFO binary buffer with head and tail
 * pointers.
 * Additional fields if wanted to be communicated to user mode can be defined
 *
 * Returns 0 on success, negative error code on error
 */
int	push_sample_legacy(uint32_t id, void *sample)
{
	struct sensor_def	*sensor;
	unsigned long flags;
	unsigned char	sample_buf[1024];
	struct senscol_sample	*p_sample = (struct senscol_sample *)sample_buf;
	struct sensor_def pseudo_event_sensor;

	if (!senscol_data_buf)
		return	-ENOMEM;

	if (id & PSEUSO_EVENT_BIT) {
		pseudo_event_sensor.sample_size = sizeof(uint32_t) +
			offsetof(struct senscol_sample, data);
		sensor = &pseudo_event_sensor;
	} else
		sensor = get_senscol_sensor_by_id(id);

	if (!sensor)
		return	-ENODEV;

	spin_lock_irqsave(&senscol_data_lock, flags);

	/* When buffer overflows the new data is dropped */
	if (senscol_data_head != senscol_data_tail &&
			(senscol_data_head - senscol_data_tail) %
			SENSCOL_DATA_BUF_SIZE <= sensor->sample_size) {
		spin_unlock_irqrestore(&senscol_data_lock, flags);
		return	-ENOMEM;
	}

	p_sample->id = id;
	p_sample->size = sensor->sample_size;
	memcpy(p_sample->data, sample,
		sensor->sample_size - offsetof(struct senscol_sample, data));

	memcpy(senscol_data_buf + senscol_data_tail, p_sample, p_sample->size);
	senscol_data_tail += sensor->sample_size;
	if (senscol_data_tail > SENSCOL_DATA_BUF_LAST)
		senscol_data_tail = 0;

	spin_unlock_irqrestore(&senscol_data_lock, flags);

	/* Fire event through "data/event" */

	if (waitqueue_active(&senscol_read_wait))
		wake_up_interruptible(&senscol_read_wait);

	return	0;
}

static int senscol_open_legacy(struct inode *inode, struct file *file)
{
	user_task = current;
	return	0;
}

static int senscol_release_legacy(struct inode *inode, struct file *file)
{
	return	0;
}

static ssize_t senscol_read_legacy(struct file *file, char __user *ubuf,
	size_t length, loff_t *offset)
{
	return	length;
}

static ssize_t senscol_write_legacy(struct file *file, const char __user *ubuf,
	size_t length, loff_t *offset)
{
	return	length;
}

static long senscol_ioctl_legacy(struct file *file, unsigned int cmd,
	unsigned long data)
{
	return	0;
}

static unsigned int senscol_poll_legacy(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;
	unsigned long	flags;
	int	rd_ready = 0;

	poll_wait(file, &senscol_read_wait, wait);

	spin_lock_irqsave(&senscol_data_lock, flags);
	rd_ready = (senscol_data_head != senscol_data_tail);
	spin_unlock_irqrestore(&senscol_data_lock, flags);

	if (rd_ready)
		mask |= (POLLIN | POLLRDNORM);
	return	mask;
}

/* flush callback */
void senscol_flush_cb_legacy(unsigned sens_id)
{
	struct sensor_def	*sens, *next;
	unsigned long	flags;
	uint32_t pseudo_event_id;
	uint32_t pseudo_event_content = 0;

	spin_lock_irqsave(&senscol_lock, flags);
	if (!flush_asked) {
		spin_unlock_irqrestore(&senscol_lock, flags);
		return;
	}

	list_for_each_entry_safe(sens, next, &senscol_sensors_list, link) {
		if (sens->flush_req) {
			sens->flush_req = 0;
			pseudo_event_id = sens->id | PSEUSO_EVENT_BIT;
			pseudo_event_content |= FLUSH_CMPL_BIT;

			spin_unlock_irqrestore(&senscol_lock, flags);
			push_sample_legacy(pseudo_event_id,
				&pseudo_event_content);
			spin_lock_irqsave(&senscol_lock, flags);
		}
	}
	flush_asked = 0;
	spin_unlock_irqrestore(&senscol_lock, flags);
	return;
}

static const struct file_operations senscol_fops_legacy = {
	.owner = THIS_MODULE,
	.read = senscol_read_legacy,
	.unlocked_ioctl = senscol_ioctl_legacy,
	.open = senscol_open_legacy,
	.release = senscol_release_legacy,
	.write = senscol_write_legacy,
	.poll = senscol_poll_legacy,
	.llseek = no_llseek
};

/*
 * Misc Device Struct
 */
static struct miscdevice senscol_misc_device_legacy = {
		.name = "sensor-collection",
		.fops = &senscol_fops_legacy,
		.minor = MISC_DYNAMIC_MINOR,
};

int senscol_init_legacy(void)
{
	int	rv;

	INIT_LIST_HEAD(&senscol_sensors_list);
	spin_lock_init(&senscol_lock);
	spin_lock_init(&senscol_data_lock);
	init_waitqueue_head(&senscol_read_wait);

	/* Init data buffer */
	senscol_data_buf = kmalloc(SENSCOL_DATA_BUF_SIZE, GFP_KERNEL);
	if (!senscol_data_buf)
		return -ENOMEM;

	senscol_data_head = 0;
	senscol_data_tail = 0;

	/* Create sensor_collection platform device and default sysfs entries */
	sc_pdev = platform_device_register_simple("sensor_collection", -1,
		NULL, 0);
	if (IS_ERR(sc_pdev)) {
		kfree(senscol_data_buf);
		return -ENODEV;
	}

	senscol_misc_device_legacy.parent = &sc_pdev->dev;
	rv = misc_register(&senscol_misc_device_legacy);
	if (rv)
		return	rv;

	rv = kobject_init_and_add(&sc_data_kobj, &sc_data_kobj_type,
		&sc_pdev->dev.kobj, "data");

	rv = sysfs_create_bin_file(&sc_data_kobj, &sensors_data_binattr);
	if (rv)
		return rv;

	return	0;
}

void senscol_exit_legacy(void)
{
	kfree(senscol_data_buf);
}
#endif /* SENSCOL_1 */
