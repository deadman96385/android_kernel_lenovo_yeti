/*
 *	drivers/switch/switch_gpio.c
 *
 * Copyright (C) 2008 Google, Inc.
 * Author: Mike Lockwood <lockwood@android.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/switch.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/wakelock.h>
#include <linux/switch_hall.h>
#include <linux/power_hal_sysfs.h>

#define HALL_WAKElOCK_TIMEOUT	(1 * HZ)

static DEFINE_MUTEX(mutex);

static void hall_switch_work(struct work_struct *work)
{
	int state;
	struct hall_switch_data	*data =
		container_of(work, struct hall_switch_data, work);

	state = gpio_get_value(data->gpio);
	pr_info("%s:%d name is %s, state is %d\n",__func__,__LINE__, data->sdev.name, state);
	switch_set_state(&data->sdev, state);
}

static irqreturn_t hall_irq_handler(int irq, void *dev_id)
{
	struct hall_switch_data *switch_data =
		(struct hall_switch_data *)dev_id;

	schedule_work(&switch_data->work);
	wake_lock_timeout(&switch_data->wakelock, HALL_WAKElOCK_TIMEOUT);

	return IRQ_HANDLED;
}

static ssize_t switch_hall_print_state(struct switch_dev *sdev, char *buf)
{
	struct hall_switch_data	*switch_data =
		container_of(sdev, struct hall_switch_data, sdev);
	const char *state;
	if (switch_get_state(sdev))
		state = switch_data->state_on;
	else
		state = switch_data->state_off;

	if (state)
		return sprintf(buf, "%s\n", state);
	return -1;
}
static ssize_t hall_switch_suspend_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int flag = 0;
	struct hall_switch_data *switch_data;
	switch_data = dev_get_drvdata(dev);
	pr_info("%s\n",__func__);
	mutex_lock(&mutex);
	flag = switch_data->wake_flag;
	mutex_unlock(&mutex);
	return sprintf(buf,"%s\n", flag ?"suspend":"resume");
}
static ssize_t hall_switch_suspend_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	struct hall_switch_data *switch_data;
	switch_data = dev_get_drvdata(dev);
	pr_info("%s: %s\n",__func__,buf);
	mutex_lock(&mutex);
	if(!strncmp(buf,"1",1)){
		switch_data->wake_flag = 1;
		if(switch_data->irq_enabled == true){
			pr_info("%s:implement suspend request\n",__func__);
#ifdef CONFIG_SWITCH_HALL_IRQ_WAKE
			disable_irq_wake(switch_data->irq);
#else
			disable_irq(switch_data->irq);
#endif
			switch_data->irq_enabled = false;
		}else{
			pr_info("%s:bypass the suspend request\n",__func__);
		}
	} else {
		switch_data->wake_flag = 0;
		if(switch_data->irq_enabled == false){
			pr_info("%s:implement resume request\n",__func__);
#ifdef CONFIG_SWITCH_HALL_IRQ_WAKE
			enable_irq_wake(switch_data->irq);
#else
			enable_irq(switch_data->irq);
#endif
			switch_data->irq_enabled = true;
		}else{
			pr_info("%s:bypass the resume request\n",__func__);
		}
	}
	mutex_unlock(&mutex);
	return count;
}

static DEVICE_ATTR(power_HAL_suspend, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP, hall_switch_suspend_show,hall_switch_suspend_store);
/*------------------------------------------------------------------------------
 *
 *-----------------------------------------------------------------------------*/
static struct attribute *hall_switch_sysfs_entries[] =
{
#ifdef CONFIG_PM_SLEEP
	&dev_attr_power_HAL_suspend.attr,
#endif
	NULL
};

static struct attribute_group hall_switch_attr_group =
{
	.attrs	= hall_switch_sysfs_entries,
};


static int hall_switch_probe(struct platform_device *pdev)
{
	struct hall_switch_data *switch_data;
	struct hall_switch_data *pdata = pdev->dev.platform_data;
	int ret = 0;
	pr_info("%s+\n",__func__);
	if(!pdata)
		return -EBUSY;
	pr_info("%s:name is %s, gpio is %d\n",__func__, pdata->sdev.name, pdata->gpio);

	switch_data = kzalloc(sizeof(struct hall_switch_data), GFP_KERNEL);
	if (!switch_data)
		return -ENOMEM;

	switch_data->sdev.name = pdata->sdev.name;
	switch_data->gpio = pdata->gpio;
	switch_data->sdev.print_state = switch_hall_print_state;
	switch_data->wake_flag = 0;

	ret = switch_dev_register(&switch_data->sdev);
	if (ret < 0)
		goto err_switch_dev_register;

	ret = gpio_request(switch_data->gpio, switch_data->sdev.name);
	if (ret < 0)
		goto err_request_gpio;

	ret = gpio_direction_input(switch_data->gpio);
	if (ret < 0)
		goto err_set_gpio_input;

	INIT_WORK(&switch_data->work, hall_switch_work);

	switch_data->irq = gpio_to_irq(switch_data->gpio);
	if (switch_data->irq < 0) {
		ret = switch_data->irq;
		goto err_detect_irq_num_failed;
	}

	ret = request_irq(switch_data->irq, hall_irq_handler,
			  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_NO_SUSPEND, switch_data->sdev.name, switch_data);
	if (ret < 0)
		goto err_request_irq;

#ifdef CONFIG_SWITCH_HALL_IRQ_WAKE
	enable_irq_wake(switch_data->irq);
#else
	enable_irq(switch_data->irq);
#endif
	switch_data->irq_enabled = true;
	/* Perform initial detection */
	hall_switch_work(&switch_data->work);
	wake_lock_init(&switch_data->wakelock, WAKE_LOCK_SUSPEND, switch_data->sdev.name);

	dev_set_drvdata(&pdev->dev,switch_data);
	/* Create the files associated with this kobject */
	ret = sysfs_create_group(&pdev->dev.kobj, &hall_switch_attr_group);
	if(ret < 0){
		pr_err("%s:Create sysfs group error\n",__func__);
		goto err_request_irq;
	}
	register_power_hal_suspend_device(&pdev->dev);
	pr_info("%s-\n",__func__);
	return 0;

err_request_irq:
err_detect_irq_num_failed:
err_set_gpio_input:
	gpio_free(switch_data->gpio);
err_request_gpio:
	switch_dev_unregister(&switch_data->sdev);
err_switch_dev_register:
	kfree(switch_data);

	return ret;
}

static struct platform_driver cht_hall_driver = {
	.probe = hall_switch_probe,
	.driver = {
		.name = "cht_hall_switch",
		.owner = THIS_MODULE,
	},
};

static int __init hall_init(void)
{
	return platform_driver_register(&cht_hall_driver);
}

fs_initcall(hall_init);
MODULE_DESCRIPTION("Hall switch sensor driver");
MODULE_LICENSE("GPL");
