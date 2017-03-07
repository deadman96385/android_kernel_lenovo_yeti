#include <linux/platform_device.h>
#include <linux/switch.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/wakelock.h>
#include <linux/switch_hall.h>

static struct hall_switch_data hall_info = {
	.gpio = GPIO_HALL_NIRQ,
	.sdev = {
		.name = "hall",
	},
};
static struct hall_switch_data sec_hall_info = {
	.gpio = GPIO_SEC_HALL_NIRQ,
	.sdev = {
		.name = "sec_hall",
	},
};
static struct platform_device hall_1_device = {
	.name = "cht_hall_switch",
	.id = 0,
	.dev = {
		.platform_data = &hall_info,
	},
};
static struct platform_device hall_2_device = {
	.name = "cht_hall_switch",
	.id = 1,
	.dev = {
		.platform_data = &sec_hall_info,
	},
};
static struct platform_device *hall_devices[] __initdata = {
	&hall_1_device,
	&hall_2_device,
};
static int __init hall_switch_init(void)
{
	pr_info("%s\n",__func__);
	return platform_add_devices(hall_devices, ARRAY_SIZE(hall_devices));
}
device_initcall(hall_switch_init);

