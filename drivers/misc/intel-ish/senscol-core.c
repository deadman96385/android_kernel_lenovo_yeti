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

#if !SENSCOL_1

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/compat.h>
#include "ishtp-hid.h"

int sensor_num = 0;

struct list_head	fds_list;
spinlock_t	fds_list_lock;

int sens_prop_to_usage(struct sensor_def *sensor, const char *prop_name)
{
	int i;
	for (i = 0; i < sensor->num_properties; i++)
		if (strcmp(sensor->properties[i].name, prop_name) == 0)
			return sensor->properties[i].usage_id;
	return 0;
}

void senscol_send_ready_event(void)
{
	struct using_fd *fd, *next;
	unsigned long flags;

	spin_lock_irqsave(&fds_list_lock, flags);
	list_for_each_entry_safe(fd, next, &fds_list, link) {
		if (waitqueue_active(&fd->read_wait))
			wake_up_interruptible(&fd->read_wait);
	}
	spin_unlock_irqrestore(&fds_list_lock, flags);
}

int senscol_reset_notify(void)
{
	struct using_fd *fd, *next;
	struct siginfo si;
	unsigned long flags;

	memset(&si, 0, sizeof(struct siginfo));
	si.si_signo = SIGUSR1;
	si.si_code = SI_USER;

	spin_lock_irqsave(&fds_list_lock, flags);
	list_for_each_entry_safe(fd, next, &fds_list, link) {
		if (fd->user_task)
			send_sig_info(SIGUSR1, &si, fd->user_task);
	}
	spin_unlock_irqrestore(&fds_list_lock, flags);

	return 0;
}

int add_set_list_property(struct sensor_def *sensor,
		struct list_head *set_prop_list, const char *prop_name,
		const char *value)
{
	struct sens_property *temp;
	uint32_t hid_usage_id = sens_prop_to_usage(sensor, prop_name);

	if (!hid_usage_id)
		return -EINVAL;

	temp = kmalloc(sizeof(struct sens_property), GFP_KERNEL);

	if (!temp)
		return -ENOMEM;

	temp->usage_id = hid_usage_id;
	temp->name = kasprintf(GFP_KERNEL, "%s", prop_name);
	temp->value = kasprintf(GFP_KERNEL, "%s", value);
	temp->sensor = sensor;
	list_add_tail(&temp->link, set_prop_list);
	return 0;
}

int remove_set_list_properties(struct list_head *set_props_list)
{
	struct sens_property *prop, *next;

	list_for_each_entry_safe(prop, next, set_props_list, link) {
		list_del(&prop->link);

		kfree(prop->name);
		kfree(prop->value);
		kfree(prop);
	}
	return 0;
}

/* Init sensor (don't call for initialized sensors) */
void init_senscol_sensor(struct sensor_def *sensor)
{
	if (!sensor)
		return;
	memset(sensor, 0, sizeof(*sensor));
	sensor->name = NULL;
	sensor->friendly_name = NULL;
	sensor->impl = NULL;
	sensor->data_fields = NULL;
	sensor->properties = NULL;
	sensor->acquire_fd = NULL;
	sensor->ask_flush_count = 0;
	sensor->sens_in_use = 0;
	init_waitqueue_head(&sensor->wait_sens_free);
}

/* return pointer to sensor if exists in fd sensors list, NULL otherwise */
struct sensor_def *get_sensor_from_fd_list(struct using_fd *cur_fd, uint32_t id)
{
	struct sens_link *sensor_link, *next;

	list_for_each_entry_safe(sensor_link, next, &cur_fd->sensors_list,
			link) {
		if (sensor_link->sensor->id == id)
			return sensor_link->sensor;
	}
	return NULL;
}

int remove_senscol_sensor(uint32_t id)
{
	unsigned long	flags;
	struct sensor_def	*sens, *next;
	struct sens_link *sensor_link, *next_sens_link;
	struct using_fd *cur_fd;
	int i;

	spin_lock_irqsave(&senscol_lock, flags);
	list_for_each_entry_safe(sens, next, &senscol_sensors_list, link) {
		if (sens->id == id) {
			/* mark the sensor as "in use" */
			if (sens->sens_in_use) {
				/* the sensor is currently in use,
					wait for it to be free */
				spin_unlock_irqrestore(&senscol_lock, flags);
				wait_event(sens->wait_sens_free,
					(!sens->sens_in_use));
				spin_lock_irqsave(&senscol_lock, flags);
			}
			sens->sens_in_use = 1;

			/* remove sensor from the fd using it */
			if (sens->acquire_fd) {
				cur_fd = sens->acquire_fd;
				list_for_each_entry_safe(sensor_link,
						next_sens_link,
						&cur_fd->sensors_list, link) {
					if (sensor_link->sensor == sens) {
						list_del(&sensor_link->link);
						kfree(sensor_link);
					}
				}
			}

			list_del(&sens->link);
			sensor_num--;

			/*
			 * senscol_lock protects sensors list
			 * of each fd as well
			 */
			spin_unlock_irqrestore(&senscol_lock, flags);

			/* remove sens props */
			for (i = 0; i < sens->num_properties; ++i) {
				kfree(sens->properties[i].name);
				kfree(sens->properties[i].value);
			}
			kfree(sens->properties);

			/* remove sens data fields */
			for (i = 0; i < sens->num_data_fields; ++i)
				kfree(sens->data_fields[i].name);
			kfree(sens->data_fields);
			kfree(sens->name);
			kfree(sens);

			return 0;
		}
	}
	/* the sensor to remove was not found */
	spin_unlock_irqrestore(&senscol_lock, flags);
	return -EINVAL;
}

/*
 * The caller is responsible for setting all meaningful fields
 * (may call add_data_field() and add_sens_property() as needed)
 * We'll consider hiding senscol framework-specific fields
 * into opaque structures
 */
int add_senscol_sensor(struct sensor_def *sensor)
{
	unsigned long	flags;
	int	i;

	if (!sensor->name || !sensor->impl || !sensor->usage_id || !sensor->id)
		return	-EINVAL;

	/* Sample size should be set by the caller to size of raw data */
	sensor->sample_size += offsetof(struct senscol_sample, data);
	/* Mark index of data_field */
	for (i = 0; i < sensor->num_data_fields; ++i)
		if (sensor->data_fields[i].name)
			sensor->data_fields[i].index = i;

	spin_lock_irqsave(&senscol_lock, flags);
	list_add_tail(&sensor->link, &senscol_sensors_list);
	sensor_num++;
	spin_unlock_irqrestore(&senscol_lock, flags);

	return 0;
}

/*
 * Push data sample in upstream buffer towards user-mode.
 * Sample's size is determined from the structure
 *
 * Samples are queued is a simple FIFO binary buffer with head and tail
 * pointers (buffer per opened fd).
 * Additional fields if wanted to be communicated to user mode can be defined
 *
 * Returns 0 on success, negative error code on error
 */
int push_sample(uint32_t id, void *sample)
{
	struct using_fd	*cur_fd;
	unsigned long	flags, buf_flags, fd_flags;
	unsigned char	*sample_buf;
	struct senscol_sample	*p_sample;
	struct sensor_def	*sensor;
	int	ret = 0;

	spin_lock_irqsave(&senscol_lock, flags);
	sensor = get_senscol_sensor_by_id(id);
	if (!sensor) {
		spin_unlock_irqrestore(&senscol_lock, flags);
		return	-ENODEV;
	}

	sample_buf = kmalloc(sensor->sample_size, GFP_ATOMIC);
	if (!sample_buf) {
		spin_unlock_irqrestore(&senscol_lock, flags);
		return -ENOMEM;
	}
	p_sample = (struct senscol_sample *)sample_buf;

	p_sample->id = id;
	p_sample->size = sensor->sample_size;
	memcpy(p_sample->data, sample,
		sensor->sample_size - offsetof(struct senscol_sample, data));

	/* When buffer overflows the new data is dropped */
	if (sensor->acquire_fd) {
		cur_fd = sensor->acquire_fd;

		spin_lock_irqsave(&fds_list_lock, fd_flags);
		if (cur_fd->fd_used_cnt == FD_BEING_REMOVED) {
			/* the fd is in remove process */
			spin_unlock_irqrestore(&fds_list_lock, fd_flags);
			spin_unlock_irqrestore(&senscol_lock, flags);
			return -EINVAL;
		} else
			cur_fd->fd_used_cnt++; /* increase fd in-use cnt.
						other usages than release fd
						won't block each other */
		spin_unlock_irqrestore(&fds_list_lock, fd_flags);
		spin_unlock_irqrestore(&senscol_lock, flags);

		if (!cur_fd->data_buf) {
			ret = -EINVAL;
			goto free_fd;
		}

		/* buffer is OK. copy data */
		spin_lock_irqsave(&cur_fd->buf_lock, buf_flags);
		if (cur_fd->head != cur_fd->tail &&
				(cur_fd->head - cur_fd->tail) %
				SENSCOL_DATA_BUF_SIZE <= p_sample->size) {
			/* drop sample */
			spin_unlock_irqrestore(&cur_fd->buf_lock, buf_flags);
		} else {
			memcpy(cur_fd->data_buf + cur_fd->tail, p_sample,
				p_sample->size);
			cur_fd->tail += p_sample->size;
			if (cur_fd->tail > SENSCOL_DATA_BUF_LAST)
				cur_fd->tail = 0;
			if (waitqueue_active(&cur_fd->read_wait))
				wake_up_interruptible(&cur_fd->read_wait);
			spin_unlock_irqrestore(&cur_fd->buf_lock, buf_flags);
		}
	} else {
		spin_unlock_irqrestore(&senscol_lock, flags);
		return 0;
	}
free_fd:
	spin_lock_irqsave(&fds_list_lock, fd_flags);
	cur_fd->fd_used_cnt--;
	if (!cur_fd->fd_used_cnt && waitqueue_active(&cur_fd->wait_fd_free))
		wake_up_interruptible(&cur_fd->wait_fd_free);
	spin_unlock_irqrestore(&fds_list_lock, fd_flags);

	return ret;
}

static int senscol_open(struct inode *inode, struct file *file)
{
	unsigned long flags;
	struct using_fd *cur_fd = kmalloc(sizeof(struct using_fd), GFP_KERNEL);
	if (!cur_fd)
		return -ENOMEM;

	cur_fd->user_task = current;/* save task to send reset notify */
	cur_fd->data_buf = NULL;
	cur_fd->head = 0;
	cur_fd->tail = 0;
	cur_fd->fd_used_cnt = 0;
	INIT_LIST_HEAD(&cur_fd->sensors_list);
	spin_lock_init(&cur_fd->buf_lock);
	init_waitqueue_head(&cur_fd->read_wait);
	init_waitqueue_head(&cur_fd->wait_fd_free);

	file->private_data = cur_fd;

	spin_lock_irqsave(&fds_list_lock, flags);
	list_add_tail(&cur_fd->link, &fds_list);
	spin_unlock_irqrestore(&fds_list_lock, flags);

	return	0;
}

static int senscol_release(struct inode *inode, struct file *file)
{
	struct using_fd *cur_fd = file->private_data;
	struct sensor_def *sensor;
	struct sens_link *sensor_link, *next;
	unsigned long	flags, sens_flags;

	/*
	 * No need to mark fd as in used in all fd functions (ioctl, read
	 * and write), because POSIX avoid them from being called in parallel
	 * to the release function.
	 * the fd_used_cnt is used to avoid race condition between the
	 * release function and the push_sample or flush callback.
	 */

	spin_lock_irqsave(&fds_list_lock, flags);
	/* mark the fd as "in use" */
	if (cur_fd->fd_used_cnt) {
		/* the fd is currently in use, wait for it to be free */
		spin_unlock_irqrestore(&fds_list_lock, flags);
		wait_event(cur_fd->wait_fd_free,
			(!cur_fd->fd_used_cnt));
		spin_lock_irqsave(&fds_list_lock, flags);
	}
	cur_fd->fd_used_cnt = FD_BEING_REMOVED; /* mark fd in remove process */
	list_del(&cur_fd->link);
	spin_unlock_irqrestore(&fds_list_lock, flags);

	spin_lock_irqsave(&senscol_lock, sens_flags);
	list_for_each_entry_safe(sensor_link, next, &cur_fd->sensors_list,
			link) {
		sensor = sensor_link->sensor;
		/* mark the sensor as "in use" */
		if (sensor->sens_in_use) {
			/*
			 * the sensor is currently in use, wait for it to be
			 * free. it is NOT at the middle of being removed,
			 * because it is still in the fd sensors list.
			 */
			spin_unlock_irqrestore(&senscol_lock, sens_flags);
			wait_event(sensor->wait_sens_free,
				(!sensor->sens_in_use));
			spin_lock_irqsave(&senscol_lock, sens_flags);
		}
		sensor->sens_in_use = 1;
		sensor->acquire_fd = NULL;
		list_del(&sensor_link->link);
		kfree(sensor_link);
		spin_unlock_irqrestore(&senscol_lock, sens_flags);

		sensor->impl->disable_sensor(sensor);

		spin_lock_irqsave(&senscol_lock, sens_flags);
		sensor->ask_flush_count = 0;
		sensor->sens_in_use = 0;
		if (waitqueue_active(&sensor->wait_sens_free))
			wake_up_interruptible(&sensor->wait_sens_free);
	}
	spin_unlock_irqrestore(&senscol_lock, sens_flags);

	kfree(cur_fd->data_buf);
	kfree(cur_fd);

	file->private_data = NULL;
	return	0;
}

static ssize_t senscol_read(struct file *file, char __user *ubuf,
	size_t length, loff_t *offset)
{
	size_t	count;
	unsigned	cur;
	struct senscol_sample	*sample;
	unsigned long	flags;
	struct using_fd *cur_fd = file->private_data;
	char *tmp_buf = kzalloc(length, GFP_KERNEL);
	if (!tmp_buf)
		return -ENOMEM;

	spin_lock_irqsave(&cur_fd->buf_lock, flags);
	/*
	 * Count how much we may copy, keeping whole samples.
	 * Copy samples along the way
	 */
	count = 0;
	cur = cur_fd->head;
	while (cur != cur_fd->tail) {
		sample = (struct senscol_sample *)(cur_fd->data_buf + cur);
		if (count + sample->size > length)
			break;
		memcpy(tmp_buf + count, sample, sample->size);
		count += sample->size;
		cur += sample->size;
		if (cur > SENSCOL_DATA_BUF_LAST)
			cur = 0;
	}
	cur_fd->head = cur;
	spin_unlock_irqrestore(&cur_fd->buf_lock, flags);

	if (copy_to_user(ubuf, tmp_buf, count)) {
		kfree(tmp_buf);
		return -EINVAL;
	}
	kfree(tmp_buf);
	return count;
}

static ssize_t senscol_write(struct file *file, const char __user *ubuf,
	size_t length, loff_t *offset)
{
	return	length;
}

/* IOCTL functions */
static long ioctl_get_sens_num(unsigned long param)
{
	if (copy_to_user((char __user *)param, &sensor_num, sizeof(int)))
		return -EFAULT;
	return 0;
}

static long ioctl_get_sens_list(unsigned long param, int compat_called)
{
	struct sensor_def *sensor, *next;
	struct ioctl_res ioctl_result;
	struct sensor_details sens_details;
	char *tmp_buf;
	unsigned long	flags;
	int buf_off = 0;
	int ret = 0;

	if (copy_from_user(&ioctl_result, (char __user *)param,
			sizeof(struct ioctl_res)))
		return -EFAULT;
	ioctl_result.response_size = sensor_num * sizeof(struct sensor_details);
	/* write back to user, with response_size filled */
	if (copy_to_user((char __user *)param, &ioctl_result,
			sizeof(struct ioctl_res)))
		return -EFAULT;
	if (ioctl_result.buffer_size < ioctl_result.response_size)
		return -ENOMEM;
	if (!ioctl_result.buffer_ptr)
		return -EINVAL;
	tmp_buf = kmalloc(ioctl_result.response_size, GFP_KERNEL);
	if (!tmp_buf)
		return -ENOMEM;

#ifdef CONFIG_COMPAT
	if (compat_called)
		ioctl_result.buffer_ptr =
			(uint64_t)compat_ptr(ioctl_result.buffer_ptr);
#endif
	spin_lock_irqsave(&senscol_lock, flags);
	list_for_each_entry_safe(sensor, next, &senscol_sensors_list, link) {
		if (ioctl_result.buffer_size - buf_off <
				sizeof(struct sensor_details)) {
			spin_unlock_irqrestore(&senscol_lock, flags);
			ret = -ENOMEM;
			goto out;
		}
		sens_details.id = sensor->id;
		sens_details.usage_id = sensor->usage_id;
		sens_details.sample_size = sensor->sample_size;
		sens_details.properties_num = sensor->num_properties;
		sens_details.data_fields_num = sensor->num_data_fields;
		strncpy(sens_details.name, sensor->name, MAX_NAME_LENGTH);
		memcpy(tmp_buf + buf_off, &sens_details,
				sizeof(struct sensor_details));
		buf_off += sizeof(struct sensor_details);
	}
	spin_unlock_irqrestore(&senscol_lock, flags);
	if (buf_off != ioctl_result.response_size) {
		ret = -EFAULT;
		goto out;
	}

	if (copy_to_user((char __user *)ioctl_result.buffer_ptr, tmp_buf,
			buf_off)) {
		ret = -EFAULT;
		goto out;
	}
out:
	kfree(tmp_buf);
	return ret;
}

static long ioctl_get_prop(unsigned long param, int compat_called)
{
	struct sensor_def *sensor;
	struct ioctl_res ioctl_result;
	struct property_details prop_details;
	int buf_off = 0;
	unsigned long	flags;
	int i, ret = 0;

	if (copy_from_user(&ioctl_result, (char __user *)param,
			sizeof(struct ioctl_res)))
		return -EFAULT;
	spin_lock_irqsave(&senscol_lock, flags);
	sensor = get_senscol_sensor_by_id(ioctl_result.sens_id);
	if (!sensor) {
		spin_unlock_irqrestore(&senscol_lock, flags);
		return -EINVAL;
	}
	/* mark sensor as in-use */
	if (sensor->sens_in_use) {
		spin_unlock_irqrestore(&senscol_lock, flags);
		return -EBUSY;
	}
	sensor->sens_in_use = 1;
	spin_unlock_irqrestore(&senscol_lock, flags);

	ioctl_result.response_size = sensor->num_properties *
		sizeof(struct property_details);
	/* write back to user, with response_size filled */
	if (copy_to_user((char __user *)param, &ioctl_result,
			sizeof(struct ioctl_res))) {
		ret = -EFAULT;
		goto free_sens;
	}
	if (ioctl_result.buffer_size < ioctl_result.response_size) {
		ret = -ENOMEM;
		goto free_sens;
	}
	if (!ioctl_result.buffer_ptr) {
		ret = -EINVAL;
		goto free_sens;
	}

#ifdef CONFIG_COMPAT
	if (compat_called)
		ioctl_result.buffer_ptr =
			(uint64_t)compat_ptr(ioctl_result.buffer_ptr);
#endif
	if (sensor->impl->get_sens_properties(sensor)) {
		ret = -EINVAL;
		goto free_sens;
	}

	for (i = 0; i < sensor->num_properties; ++i) {
		if (ioctl_result.buffer_size - buf_off <
				sizeof(struct property_details)) {
			ret = -ENOMEM;
			goto free_sens;
		}

		strncpy(prop_details.prop_name, sensor->properties[i].name,
			MAX_PROP_NAME_LEN);
		strncpy(prop_details.value, sensor->properties[i].value,
			MAX_PROP_VAL_LEN);
		if (copy_to_user((char __user *)ioctl_result.buffer_ptr +
				buf_off, &prop_details,
				sizeof(struct property_details))) {
			ret = -EFAULT;
			goto free_sens;
		}
		buf_off += sizeof(struct property_details);
	}
	if (buf_off != ioctl_result.response_size)
		ret = -EFAULT;
free_sens:
	spin_lock_irqsave(&senscol_lock, flags);
	sensor->sens_in_use = 0;
	if (waitqueue_active(&sensor->wait_sens_free))
		wake_up_interruptible(&sensor->wait_sens_free);
	spin_unlock_irqrestore(&senscol_lock, flags);

	return ret;
}

static long ioctl_set_prop(struct using_fd *cur_fd, unsigned long param,
	int compat_called)
{
	struct sens_link *sensor_link = NULL;
	struct sensor_def *sensor;
	struct ioctl_res ioctl_result;
	struct list_head set_props;
	char *prop_array_buf;
	struct property_details *prop_array;
	int prop_array_size;
	unsigned long flags;
	int ret = 0, i;
	/* flag indicates recover from error when occurs */
	int sens_aquired_now = 0;

	if (copy_from_user(&ioctl_result, (char __user *)param,
			sizeof(struct ioctl_res)))
		return -EFAULT;
	if (ioctl_result.buffer_size <= 0)
		return -EINVAL;
	if (!ioctl_result.buffer_ptr)
		return -EINVAL;
	if (ioctl_result.buffer_size % sizeof(struct property_details))
		return -EINVAL; /* error format of user buffer -
					size doesn't fit */

	/* check if this fd already acquired this sensor*/
	spin_lock_irqsave(&senscol_lock, flags);
	sensor = get_sensor_from_fd_list(cur_fd, ioctl_result.sens_id);
	if (!sensor) {
		sensor = get_senscol_sensor_by_id(ioctl_result.sens_id);
		if (!sensor) {
			spin_unlock_irqrestore(&senscol_lock, flags);
			return -EINVAL;
		}
		/* mark sensor as in-use */
		if (sensor->sens_in_use) {
			spin_unlock_irqrestore(&senscol_lock, flags);
			return -EBUSY;
		}
		sensor->sens_in_use = 1;

		if (sensor->acquire_fd) { /* sensor is caught by another fd */
			spin_unlock_irqrestore(&senscol_lock, flags);
			ret = -EINVAL;
			goto out;
		}
		spin_unlock_irqrestore(&senscol_lock, flags);

		spin_lock_irqsave(&cur_fd->buf_lock, flags);
		if (!cur_fd->data_buf) { /* this is the first sensor this fd
						using. allocate buffer */
			cur_fd->data_buf = kzalloc(SENSCOL_DATA_BUF_SIZE,
				GFP_ATOMIC);
		}
		if (!cur_fd->data_buf) {
			spin_unlock_irqrestore(&cur_fd->buf_lock, flags);
			ret = -ENOMEM;
			goto out;
		}
		spin_unlock_irqrestore(&cur_fd->buf_lock, flags);

		sensor_link = kmalloc(sizeof(struct sens_link), GFP_KERNEL);
		if (!sensor_link) {
			ret = -ENOMEM;
			goto out;
		}

		spin_lock_irqsave(&senscol_lock, flags);
		sensor_link->sensor = sensor;
		list_add_tail(&sensor_link->link, &cur_fd->sensors_list);
		sensor->acquire_fd = cur_fd;
		spin_unlock_irqrestore(&senscol_lock, flags);

		sens_aquired_now = 1;
	} else {
		/* mark sensor as in-use */
		if (sensor->sens_in_use) {
			spin_unlock_irqrestore(&senscol_lock, flags);
			return -EBUSY;
		}
		sensor->sens_in_use = 1;
		spin_unlock_irqrestore(&senscol_lock, flags);
	}

	prop_array_buf = kmalloc(ioctl_result.buffer_size, GFP_KERNEL);
	if (!prop_array_buf) {
		ret = -ENOMEM;
		goto out;
	}

#ifdef CONFIG_COMPAT
	if (compat_called)
		ioctl_result.buffer_ptr =
			(uint64_t)compat_ptr(ioctl_result.buffer_ptr);
#endif
	if (copy_from_user(prop_array_buf,
			(char __user *)ioctl_result.buffer_ptr,
			ioctl_result.buffer_size)) {
		ret = -EFAULT;
		goto free;
	}
	prop_array = (struct property_details *)prop_array_buf;
	prop_array_size = ioctl_result.buffer_size /
		sizeof(struct property_details);

	/* call hid set properties */
	INIT_LIST_HEAD(&set_props);
	for (i = 0; i < prop_array_size; i++) {
		if (add_set_list_property(sensor, &set_props,
				prop_array[i].prop_name, prop_array[i].value)) {
			ret = -EINVAL;
			goto free_prop_list;
		}
	}
	ret = sensor->impl->set_sens_properties(&set_props, sensor->id);
free_prop_list:
	remove_set_list_properties(&set_props);
free:
	kfree(prop_array_buf);
out:
	/* in error: release the acquired sensor, if it was acquired now */
	if (ret && sens_aquired_now) {
		spin_lock_irqsave(&senscol_lock, flags);
		list_del(&sensor_link->link);
		sensor->acquire_fd = NULL;
		spin_unlock_irqrestore(&senscol_lock, flags);
		kfree(sensor_link);
	}

	/* free sensor "in-use" flag */
	spin_lock_irqsave(&senscol_lock, flags);
	sensor->sens_in_use = 0;
	if (waitqueue_active(&sensor->wait_sens_free))
		wake_up_interruptible(&sensor->wait_sens_free);
	spin_unlock_irqrestore(&senscol_lock, flags);

	return ret;
}

static long ioctl_get_data_fields(unsigned long param, int compat_called)
{
	struct sensor_def *sensor;
	struct ioctl_res ioctl_result;
	struct data_fields_details data_fld_details;
	unsigned long flags;
	int buf_off = 0;
	int i, ret = 0;

	if (copy_from_user(&ioctl_result, (char __user *)param,
			sizeof(struct ioctl_res)))
		return -EFAULT;
	spin_lock_irqsave(&senscol_lock, flags);
	sensor = get_senscol_sensor_by_id(ioctl_result.sens_id);
	if (!sensor) {
		spin_unlock_irqrestore(&senscol_lock, flags);
		return -EINVAL;
	}

	/* mark sensor as in-use */
	if (sensor->sens_in_use) {
		spin_unlock_irqrestore(&senscol_lock, flags);
		return -EBUSY;
	}
	sensor->sens_in_use = 1;
	spin_unlock_irqrestore(&senscol_lock, flags);
	ioctl_result.response_size = sensor->num_data_fields *
		sizeof(struct data_fields_details);
	/* write back to user, with response_size filled */
	if (copy_to_user((char __user *)param, &ioctl_result,
			sizeof(struct ioctl_res))) {
		ret = -EFAULT;
		goto free_sens;
	}
	if (ioctl_result.buffer_size < ioctl_result.response_size) {
		ret = -ENOMEM;
		goto free_sens;
	}
	if (!ioctl_result.buffer_ptr) {
		ret = -EINVAL;
		goto free_sens;
	}

#ifdef CONFIG_COMPAT
	if (compat_called)
		ioctl_result.buffer_ptr =
			(uint64_t)compat_ptr(ioctl_result.buffer_ptr);
#endif
	for (i = 0; i < sensor->num_data_fields; ++i) {
		if (ioctl_result.buffer_size - buf_off <
				sizeof(struct data_fields_details)) {
			ret = -ENOMEM;
			goto free_sens;
		}

		strncpy(data_fld_details.data_field_name,
			sensor->data_fields[i].name, MAX_DATA_FIELD_NAME_LEN);
		data_fld_details.usage_id = sensor->data_fields[i].usage_id;
		data_fld_details.exponent = sensor->data_fields[i].exp;
		data_fld_details.len = sensor->data_fields[i].len;
		data_fld_details.unit = sensor->data_fields[i].unit;
		data_fld_details.index = sensor->data_fields[i].index;
		data_fld_details.is_numeric = sensor->data_fields[i].is_numeric;

		if (copy_to_user((char __user *)ioctl_result.buffer_ptr +
				buf_off, &data_fld_details,
				sizeof(struct data_fields_details))) {
			ret = -EFAULT;
			goto free_sens;
		}
		buf_off += sizeof(struct data_fields_details);
	}
	if (buf_off != ioctl_result.response_size)
		ret = -EFAULT;
free_sens:
	spin_lock_irqsave(&senscol_lock, flags);
	sensor->sens_in_use = 0;
	if (waitqueue_active(&sensor->wait_sens_free))
		wake_up_interruptible(&sensor->wait_sens_free);
	spin_unlock_irqrestore(&senscol_lock, flags);

	return ret;
}

static long ioctl_flush(struct using_fd *cur_fd, unsigned long param)
{
	struct sensor_def *sensor;
	unsigned long flags;
	uint32_t sens_id;
	long	ret;

	if (copy_from_user(&sens_id, (char __user *)param,
			sizeof(uint32_t)))
		return -EFAULT;
	spin_lock_irqsave(&senscol_lock, flags);
	sensor = get_sensor_from_fd_list(cur_fd, sens_id);
	if (!sensor) {
		spin_unlock_irqrestore(&senscol_lock, flags);
		return -EINVAL;
	}
	/* mark sensor as in-use */
	if (sensor->sens_in_use) {
		spin_unlock_irqrestore(&senscol_lock, flags);
		return -EBUSY;
	}
	sensor->sens_in_use = 1;
	sensor->ask_flush_count++;
	spin_unlock_irqrestore(&senscol_lock, flags);

	ret = sensor->impl->ask_flush(sensor);

	/* release sensor "in use" flag */
	spin_lock_irqsave(&senscol_lock, flags);
	sensor->sens_in_use = 0;
	if (waitqueue_active(&sensor->wait_sens_free))
		wake_up_interruptible(&sensor->wait_sens_free);
	spin_unlock_irqrestore(&senscol_lock, flags);

	return ret;
}

static long ioctl_get_sample(struct using_fd *cur_fd, unsigned long param)
{
	struct sensor_def *sensor;
	uint32_t sens_id;
	unsigned long flags;
	long ret;

	if (copy_from_user(&sens_id, (char __user *)param,
			sizeof(uint32_t)))
		return -EFAULT;
	spin_lock_irqsave(&senscol_lock, flags);
	sensor = get_sensor_from_fd_list(cur_fd, sens_id);
	if (!sensor) {
		spin_unlock_irqrestore(&senscol_lock, flags);
		return -EINVAL;
	}

	/* mark sensor as in-use */
	if (sensor->sens_in_use) {
		spin_unlock_irqrestore(&senscol_lock, flags);
		return -EBUSY;
	}
	sensor->sens_in_use = 1;
	spin_unlock_irqrestore(&senscol_lock, flags);

	ret = sensor->impl->get_sample(sensor);

	/* release sensor "in use" flag */
	spin_lock_irqsave(&senscol_lock, flags);
	sensor->sens_in_use = 0;
	if (waitqueue_active(&sensor->wait_sens_free))
		wake_up_interruptible(&sensor->wait_sens_free);
	spin_unlock_irqrestore(&senscol_lock, flags);

	return ret;
}

static long senscol_ioctl(struct file *file, unsigned int cmd,
	unsigned long param)
{
	struct using_fd *cur_fd = file->private_data;
	int compat_called = cur_fd->comapt_ioctl;
	cur_fd->comapt_ioctl = 0;

	if (!param)
		return -EINVAL;

	switch (cmd) {
	case IOCTL_GET_SENSORS_NUM:
		return ioctl_get_sens_num(param);
	case IOCTL_GET_SENSORS_LIST:
		return ioctl_get_sens_list(param, compat_called);
	case IOCTL_GET_PROP:
		return ioctl_get_prop(param, compat_called);
	case IOCTL_SET_PROP:
		return ioctl_set_prop((struct using_fd *)file->private_data,
			param, compat_called);
	case IOCTL_GET_DATA_FIELDS:
		return ioctl_get_data_fields(param, compat_called);
	case IOCTL_FLUSH:
		return ioctl_flush((struct using_fd *)file->private_data,
			param);
	case IOCTL_GET_SAMPLE:
		return ioctl_get_sample((struct using_fd *)file->private_data,
			param);
	default: return -EINVAL;
	}

	return	0;
}

static unsigned int senscol_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;
	unsigned long	flags;
	int	rd_ready = 0;
	struct using_fd *cur_fd = file->private_data;

	poll_wait(file, &cur_fd->read_wait, wait);

	spin_lock_irqsave(&cur_fd->buf_lock, flags);
	rd_ready = (cur_fd->head != cur_fd->tail);
	spin_unlock_irqrestore(&cur_fd->buf_lock, flags);

	if (rd_ready)
		mask |= (POLLIN | POLLRDNORM);
	return	mask;
}

/* flush callback */
void senscol_flush_cb(unsigned sens_id)
{
	struct sensor_def *sensor;
	unsigned long	buf_flags, sens_flags, flags;
	unsigned char	sample_buf[50];
	struct senscol_sample	*p_sample = (struct senscol_sample *)sample_buf;
	uint32_t pseudo_event_content = 0;
	struct using_fd *cur_fd;

	p_sample->id = sens_id | PSEUSO_EVENT_BIT;
	p_sample->size = sizeof(uint32_t) +
		offsetof(struct senscol_sample, data);
	pseudo_event_content |= FLUSH_CMPL_BIT;
	memcpy(p_sample->data, &pseudo_event_content, sizeof(uint32_t));

	spin_lock_irqsave(&senscol_lock, sens_flags);
	sensor = get_senscol_sensor_by_id(sens_id);
	if (!sensor) {
		spin_unlock_irqrestore(&senscol_lock, sens_flags);
		return;
	}

	if (sensor->acquire_fd) {
		cur_fd = sensor->acquire_fd;

		spin_lock_irqsave(&fds_list_lock, flags);
		if (cur_fd->fd_used_cnt == FD_BEING_REMOVED) {
			/* the fd is in remove process */
			spin_unlock_irqrestore(&fds_list_lock, flags);
			spin_unlock_irqrestore(&senscol_lock, sens_flags);
			return;
		} else
			cur_fd->fd_used_cnt++; /* increase fd in-use cnt.
						other usages than release fd
						won't block each other */
		spin_unlock_irqrestore(&fds_list_lock, flags);

		/* buffer is ruined or no one asked for flush */
		if (!cur_fd->data_buf || !sensor->ask_flush_count) {
			spin_unlock_irqrestore(&senscol_lock, sens_flags);
			goto free_fd;
		}
		sensor->ask_flush_count--;
		spin_unlock_irqrestore(&senscol_lock, sens_flags);

		/* buffer is OK. copy data */
		spin_lock_irqsave(&cur_fd->buf_lock, buf_flags);
		if (cur_fd->head != cur_fd->tail &&
				(cur_fd->head - cur_fd->tail) %
				SENSCOL_DATA_BUF_SIZE <= p_sample->size) {
			/* drop sample */
			spin_unlock_irqrestore(&cur_fd->buf_lock, buf_flags);
		} else {
			memcpy(cur_fd->data_buf + cur_fd->tail,	p_sample,
				p_sample->size);
			cur_fd->tail += p_sample->size;
			if (cur_fd->tail > SENSCOL_DATA_BUF_LAST)
				cur_fd->tail = 0;
			spin_unlock_irqrestore(&cur_fd->buf_lock, buf_flags);
		}
		if (waitqueue_active(&cur_fd->read_wait))
			wake_up_interruptible(&cur_fd->read_wait);
	} else {
		spin_unlock_irqrestore(&senscol_lock, sens_flags);
		return;
	}
free_fd:
	spin_lock_irqsave(&fds_list_lock, flags);
	cur_fd->fd_used_cnt--;
	if (!cur_fd->fd_used_cnt && waitqueue_active(&cur_fd->wait_fd_free))
		wake_up_interruptible(&cur_fd->wait_fd_free);
	spin_unlock_irqrestore(&fds_list_lock, flags);
}

#ifdef CONFIG_COMPAT
static long senscol_compat_ioctl(struct file *file,
			unsigned int cmd, unsigned long data)
{
	struct using_fd *cur_fd = file->private_data;
	if (!data)
		return -EINVAL;

	cur_fd->comapt_ioctl = 1;
	return senscol_ioctl(file, cmd, (unsigned long)compat_ptr(data));
}
#endif /* CONFIG_COMPAT */

static const struct file_operations senscol_fops = {
	.owner = THIS_MODULE,
	.read = senscol_read,
	.unlocked_ioctl = senscol_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = senscol_compat_ioctl,
#endif
	.open = senscol_open,
	.release = senscol_release,
	.write = senscol_write,
	.poll = senscol_poll,
	.llseek = no_llseek
};

/*
 * Misc Device Struct
 */
static struct miscdevice senscol_misc_device = {
		.name = "senscol2",
		.fops = &senscol_fops,
		.minor = MISC_DYNAMIC_MINOR,
};

int senscol_init(void)
{
	int	rv;

	INIT_LIST_HEAD(&senscol_sensors_list);
	INIT_LIST_HEAD(&fds_list);
	spin_lock_init(&senscol_lock);
	spin_lock_init(&fds_list_lock);

	rv = misc_register(&senscol_misc_device);
	if (rv)
		return	rv;

	return 0;
}

#endif /* !SENSCOL_1 */
