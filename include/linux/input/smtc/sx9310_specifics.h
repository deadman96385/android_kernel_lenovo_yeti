/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */
#include <linux/input/smtc/sx9310_platform_data.h>
#include <linux/input/smtc/sx9310_i2c_reg.h>

/*GPIO define*/
#define	gpio_southwest_NUM	98
#define	gpio_north_NUM	73
#define	gpio_east_NUM	27
#define	gpio_southeast_NUM	86

#define	gpio_southwest_base	(ARCH_NR_GPIOS-gpio_southwest_NUM) //414
#define	gpio_north_base		(gpio_southwest_base - gpio_north_NUM) //341
#define	gpio_east_base		(gpio_north_base - gpio_east_NUM) //314
#define	gpio_southeast_base		(gpio_east_base - gpio_southeast_NUM)//228

#define	MF_ISH_GPIO_7	16
#define	MF_ISH_GPIO_2	24
#define	MF_ISH_GPIO_8	23
#define	MF_ISH_GPIO_0	21

/* IO Used for NIRQ */
#define GPIO_SX9310_NIRQ (gpio_east_base + MF_ISH_GPIO_7)
#define GPIO_SX9310_SARSTATE  (gpio_east_base + MF_ISH_GPIO_2)//tp3110
#define GPIO_SX9310_RST		(gpio_east_base + MF_ISH_GPIO_8) //337
#define GPIO_SX9310_2_NIRQ (gpio_east_base + MF_ISH_GPIO_0)

/* wifi sar HOOK */
typedef int (* sar_hook_fn)(int);
sar_hook_fn sar_wifi_fp = NULL;
void sx9310_hook_bind(sar_hook_fn hook_fn)
{
	sar_wifi_fp = hook_fn;
}
EXPORT_SYMBOL(sx9310_hook_bind);

static int sx9310_get_nirq_state(void)
{
	return !gpio_get_value(GPIO_SX9310_NIRQ);
}
static int sx9310_get_sar_state(void)
{
	return gpio_get_value(GPIO_SX9310_SARSTATE);
}

static int sx9310_init(void)
{
	if ((gpio_request(GPIO_SX9310_NIRQ, "SX9310_NIRQ") == 0) &&
		(gpio_direction_input(GPIO_SX9310_NIRQ) == 0)) {
		gpio_export(GPIO_SX9310_NIRQ, 0);
		pr_err("obtained gpio for SX9310_NIRQ\n");
	} else {
		pr_err("could not obtain gpio for SX9310_NIRQ\n");
		return -1;
	}

	if ((gpio_request(GPIO_SX9310_SARSTATE, "SX9310_SARSTATE") == 0) &&
		(gpio_direction_input(GPIO_SX9310_SARSTATE) == 0)) {
		gpio_export(GPIO_SX9310_SARSTATE, 0);
		pr_err("obtained gpio for SX9310_SARSTATE\n");
	} else {
		pr_err("could not obtain gpio for SX9310_SARSTATE\n");
		return -1;
	}

	return 0;
}

/* Define Registers that need to be initialized to values different than
 * default
 */
static struct smtc_reg_data sx9310_i2c_reg_setup[] = {
	{
		.reg = SX9310_IRQ_ENABLE_REG,
		.val = 0x70,
		//.val = 0x78,			//read raw data.
	},
	{
		.reg = SX9310_IRQFUNC_REG,
		.val = 0x00,
	},
	{
		.reg = SX9310_CPS_CTRL1_REG,
		.val = 0x00,
	},
	{
		.reg = SX9310_CPS_CTRL2_REG,
		.val = 0x04,		// 0x07 // 0x00
	},
	{
		.reg = SX9310_CPS_CTRL3_REG,
		.val = 0x0f,		// 0x0c
	},
	{
		.reg = SX9310_CPS_CTRL4_REG,
		.val = 0x07,		//0x0D,
	},
	{
		.reg = SX9310_CPS_CTRL5_REG,
		.val = 0xC1,
	},
	{
		.reg = SX9310_CPS_CTRL6_REG,
		.val = 0x58,	//0x20,
	},
	{
		.reg = SX9310_CPS_CTRL7_REG,
		.val = 0x74,	//0x4C,
	},
	{
		.reg = SX9310_CPS_CTRL8_REG,
		.val = 0x76,
	},
	{
		.reg = SX9310_CPS_CTRL9_REG,
		.val = 0x7E,	//0x7D,
	},
	{
		.reg = SX9310_CPS_CTRL10_REG,
		.val = 0x18,	//0x00
	},
	{
		.reg = SX9310_CPS_CTRL11_REG,
		.val = 0x00,
	},
	{
		.reg = SX9310_CPS_CTRL12_REG,
		.val = 0x00,
	},
	{
		.reg = SX9310_CPS_CTRL13_REG,
		.val = 0x00,
	},
	{
		.reg = SX9310_CPS_CTRL14_REG,
		.val = 0x00,
	},
	{
		.reg = SX9310_CPS_CTRL15_REG,
		.val = 0x00,
	},
	{
		.reg = SX9310_CPS_CTRL16_REG,
		.val = 0x00,
	},
	{
		.reg = SX9310_CPS_CTRL17_REG,
		.val = 0x00,
	},
	{
		.reg = SX9310_CPS_CTRL18_REG,
		.val = 0x00,
	},
	{
		.reg = SX9310_CPS_CTRL19_REG,
		.val = 0x00,
	},
	{
		.reg = SX9310_SAR_CTRL0_REG,
		.val = 0x01,
	},
	{
		.reg = SX9310_SAR_CTRL1_REG,
		.val = 0x80,
	},
	{
		.reg = SX9310_SAR_CTRL2_REG,
		.val = 0x0c,
	},
	{
		.reg = SX9310_CPS_CTRL0_REG,
		.val = 0x53,
	},
};

static struct _buttonInfo psmtcButtons[] = {
	{
		.keycode = KEY_0,
		.mask = SX9310_TCHCMPSTAT_TCHSTAT0_FLAG,
	},
	{
		.keycode = KEY_1,
		.mask = SX9310_TCHCMPSTAT_TCHSTAT1_FLAG,
	},
	{
		.keycode = KEY_2,
		.mask = SX9310_TCHCMPSTAT_TCHSTAT2_FLAG,
	},
	{
		.keycode = KEY_3/*KEY_COMB*/,
		.mask = SX9310_TCHCMPSTAT_TCHSTAT3_FLAG,
	},
};

static struct _totalButtonInformation smtcButtonInformation = {
	.buttons = psmtcButtons,
	.buttonSize = ARRAY_SIZE(psmtcButtons),
};

static void sx9310_set_sar_state(int value)
{
	pr_info("%s: %d\n", __func__, value);
	// inform wifi module
	if (sar_wifi_fp != NULL)
		sar_wifi_fp(!value);
	// inform modem
	gpio_direction_output(GPIO_SX9310_SARSTATE, value);
}

static sx9310_platform_data_t sx9310_config = {
	//set sar state on GPIO pin
	.set_sar_state = sx9310_set_sar_state,
	/* Function pointer to get the NIRQ state (1->NIRQ-low, 0->NIRQ-high) */
	.get_is_nirq_low = sx9310_get_nirq_state,
	/*  pointer to an initializer function. Here in case needed in the future */
	.get_sar_state = sx9310_get_sar_state,
	//.init_platform_hw = sx9310_init_ts,
	.init_platform_hw = sx9310_init,
	/*  pointer to an exit function. Here in case needed in the future */
	//.exit_platform_hw = sx9310_exit_ts,
	.exit_platform_hw = NULL,

	.pi2c_reg = sx9310_i2c_reg_setup,
	.i2c_reg_num = ARRAY_SIZE(sx9310_i2c_reg_setup),

	.pbuttonInformation = &smtcButtonInformation,
	.nirq_gpio = GPIO_SX9310_NIRQ,
	.nstate_gpio = GPIO_SX9310_SARSTATE,

};
