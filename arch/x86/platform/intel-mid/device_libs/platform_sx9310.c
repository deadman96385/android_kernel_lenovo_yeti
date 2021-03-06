#include <linux/i2c.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/types.h>
#include <linux/gpio.h>

#include <linux/input/smtc/sx86xx.h> /* main struct, interrupt,init,pointers */
#include <linux/input/smtc/sx9310_i2c_reg.h>
#include <linux/input/smtc/sx9310_platform_data.h>	/* platform data */
#include <linux/input/smtc/sx9310_specifics.h>
#define DRIVER_NAME "sx9310"

static int __init sx9310_main_dev_init(void){
	struct i2c_board_info i2c_info;
	struct i2c_adapter *adapter;
	int i2c_busnum = 1;
	pr_info("%s\n",__func__);

	memset(&i2c_info, 0, sizeof(i2c_info));
	strncpy(i2c_info.type, DRIVER_NAME, strlen(DRIVER_NAME));

	i2c_info.addr = 0x28;
	i2c_info.irq = gpio_to_irq(GPIO_SX9310_NIRQ);;

	pr_info("%s: I2C bus = %d, name = %s, irq = 0x%2x, addr = 0x%x\n",__func__,
		i2c_busnum, i2c_info.type, i2c_info.irq, i2c_info.addr);

	i2c_info.platform_data = &sx9310_config;
	i2c_info.flags = I2C_CLIENT_WAKE;

	adapter = i2c_get_adapter(i2c_busnum);
	if(adapter){
		if(i2c_new_device(adapter,&i2c_info)){
			pr_info("%s:add new i2c device %s , addr 0x%x\n", __func__, DRIVER_NAME,i2c_info.addr);
			return 0;
		}else{
			pr_err("%s:add new i2c device %s , addr 0x%x fail !!!\n", __func__, DRIVER_NAME,i2c_info.addr);
			return -ENODEV;
		}
	}else{
		pr_err("[%s]get adapter %d fail\n",__func__,i2c_busnum);
		return -EINVAL;
	}
}
static int __init sx9310_platform_init(void){
	int ret;
	ret = sx9310_main_dev_init();
	return ret;
}
device_initcall(sx9310_platform_init);
