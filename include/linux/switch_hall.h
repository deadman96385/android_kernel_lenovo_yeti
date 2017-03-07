
#ifndef __LINUX_SWITCH_HALL_H__
#define __LINUX_SWITCH_HALL_H__

#define HALL_WAKElOCK_TIMEOUT   (1 * HZ)

#define gpio_southwest_NUM 98
#define gpio_north_NUM 73
#define gpio_east_NUM 27
#define gpio_southeast_NUM 86

#define gpio_southwest_base (ARCH_NR_GPIOS-gpio_southwest_NUM) //414
#define gpio_north_base (gpio_southwest_base - gpio_north_NUM) //341
#define gpio_east_base (gpio_north_base - gpio_east_NUM) //314
#define gpio_southeast_base (gpio_east_base - gpio_southeast_NUM)//228

#define MF_ISH_GPIO_1 18//e18
#define MF_ISH_GPIO_5 19//e19

#define GPIO_HALL_NIRQ (gpio_east_base + MF_ISH_GPIO_5)
#define GPIO_SEC_HALL_NIRQ (gpio_east_base + MF_ISH_GPIO_1)


struct hall_switch_data {
	struct switch_dev sdev;
	unsigned gpio;
	bool irq_enabled;
	const char *name_on;
	const char *name_off;
	const char *state_on;
	const char *state_off;
	int irq;
	int wake_flag;
	struct work_struct work;
	struct wake_lock wakelock;
};
#endif /* __LINUX_SWITCH_HALL_H__ */
