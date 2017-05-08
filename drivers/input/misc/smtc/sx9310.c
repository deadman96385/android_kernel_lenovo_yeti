/*! \file sx9310.c
 * \brief	 SX9310 Driver
 *
 * Driver for the SX9310
 * Copyright (c) 2011 Semtech Corp
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 */
#define DRIVER_NAME "sx9310"

#define MAX_WRITE_ARRAY_SIZE 32
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/gpio.h>

#include <linux/input/smtc/sx86xx.h> /* main struct, interrupt,init,pointers */
#include <linux/input/smtc/sx9310_i2c_reg.h>
#include <linux/input/smtc/sx9310_platform_data.h>	  /* platform data */

#define IDLE 0
#define ACTIVE 1

#define LOOP_COUNT		5
#define INTERVAL		20 * 1000
#define INTERVAL_FAR	5	 * 1000
#define CALI_LOOP

#ifdef CALI_LOOP
struct workqueue_struct *cali_workqueue = NULL;
#endif

static u8 get_default_value_of_reg(psx86xx_t this, u8 reg);
static void set_default_value_of_reg(psx86xx_t this, u8 reg, u8 val);

/*! \struct sx9310
 * Specialized struct containing input event data, platform data, and
 * last cap state read if needed.
 */
typedef struct sx9310
{
	pbuttonInformation_t pbuttonInformation;
	psx9310_platform_data_t hw; /* specific platform data settings */
} sx9310_t, *psx9310_t;


/*! \fn static int write_register(psx86xx_t this, u8 address, u8 value)
 * \brief Sends a write register to the device
 * \param this Pointer to main parent struct
 * \param address 8-bit register address
 * \param value		8-bit register value to write to address
 * \return Value from i2c_master_send
 */
static int write_register(psx86xx_t this, u8 address, u8 value)
{
	struct i2c_client *i2c = 0;
	char buffer[2];
	int returnValue = 0;
	buffer[0] = address;
	buffer[1] = value;
	returnValue = -ENOMEM;
	if (this && this->bus) {
		i2c = this->bus;
		returnValue = i2c_master_send(i2c,buffer,2);
		pr_debug("%s:write_register Address: 0x%x Value: 0x%x Return: %d\n",
		__func__, address, value, returnValue);
	}
	return returnValue;
}

/*! \fn static int read_register(psx86xx_t this, u8 address, u8 *value)
* \brief Reads a register's value from the device
* \param this Pointer to main parent struct
* \param address 8-Bit address to read from
* \param value Pointer to 8-bit value to save register value to
* \return Value from i2c_smbus_read_byte_data if < 0. else 0
*/
static int read_register(psx86xx_t this, u8 address, u8 *value)
{
	struct i2c_client *i2c = 0;
	s32 returnValue = 0;
	if (this && value && this->bus) {
		i2c = this->bus;
		returnValue = i2c_smbus_read_byte_data(i2c,address);
		pr_debug("%s:read_register Address: 0x%x Return: 0x%x\n",__func__,address,returnValue);
		if (returnValue >= 0) {
			*value = returnValue;
			return 0;
		} else {
			return returnValue;
		}
	}
	return -ENOMEM;
}

/*********************************************************************/
/*! \brief Perform a manual offset calibration
* \param this Pointer to main parent struct
* \return Value return value from the write register
 */
static int manual_offset_calibration(psx86xx_t this)
{
	s32 returnValue = 0;
	pr_info("%s+\n",__func__);
	returnValue = write_register(this,SX9310_IRQSTAT_REG,0xFF);
	return returnValue;
}
/*! \brief sysfs show function for manual calibration which currently just
 * returns register value.
 */
static ssize_t sx9310_manual_offs_calib_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	u8 reg_value = 0;
	psx86xx_t this = dev_get_drvdata(dev);

	pr_debug("Reading IRQSTAT_REG\n");
	read_register(this,SX9310_IRQSTAT_REG,&reg_value);
	return sprintf(buf, "%d\n", reg_value);
}

/*! \brief sysfs store function for manual calibration
 */
static ssize_t sx9310_manual_offs_calib_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	psx86xx_t this = dev_get_drvdata(dev);
	unsigned long val;
	s32 returnValue = 0;

	if (strict_strtoul(buf, 0, &val))
		return -EINVAL;

	if (val) {
		// if the previous state is touched, then need to keep touched state
		if (this->pre_state == ACTIVE) {
			this->ignore_once_info_modem= true;
			pr_info("we should ignore this change state\n");
		}
		returnValue = manual_offset_calibration(this);
		if (returnValue >0) {
			count = returnValue;
		}
		else
			pr_err("Performing manual_offset_calibration() FAIL!!!\n");
	}
	return count;
}

static ssize_t sx9310_reg_dump_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	u8 reg_value = 0;
	psx86xx_t this = dev_get_drvdata(dev);
	char * p = buf;
	if (this->read_flag) {
		this->read_flag = 0;
		read_register(this, this->read_reg, &reg_value);
		p += sprintf(p, "%02x\n", reg_value);
		return (p-buf);
	}

	read_register(this, SX9310_IRQ_ENABLE_REG, &reg_value);
	p += sprintf(p, "ENABLE(0x%02x)=0x%02x\n", SX9310_IRQ_ENABLE_REG, reg_value);

	read_register(this, SX9310_IRQFUNC_REG, &reg_value);
	p += sprintf(p, "IRQFUNC(0x%02x)=0x%02x\n", SX9310_IRQFUNC_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL0_REG, &reg_value);
	p += sprintf(p, "CTRL0(0x%02x)=0x%02x\n", SX9310_CPS_CTRL0_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL1_REG, &reg_value);
	p += sprintf(p, "CTRL1(0x%02x)=0x%02x\n", SX9310_CPS_CTRL1_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL2_REG, &reg_value);
	p += sprintf(p, "CTRL2(0x%02x)=0x%02x\n", SX9310_CPS_CTRL2_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL3_REG, &reg_value);
	p += sprintf(p, "CTRL3(0x%02x)=0x%02x\n", SX9310_CPS_CTRL3_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL4_REG, &reg_value);
	p += sprintf(p, "CTRL4(0x%02x)=0x%02x\n", SX9310_CPS_CTRL4_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL5_REG, &reg_value);
	p += sprintf(p, "CTRL5(0x%02x)=0x%02x\n", SX9310_CPS_CTRL5_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL6_REG, &reg_value);
	p += sprintf(p, "CTRL6(0x%02x)=0x%02x\n", SX9310_CPS_CTRL6_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL7_REG, &reg_value);
	p += sprintf(p, "CTRL7(0x%02x)=0x%02x\n", SX9310_CPS_CTRL7_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL8_REG, &reg_value);
	p += sprintf(p, "CTRL8(0x%02x)=0x%02x\n", SX9310_CPS_CTRL8_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL9_REG, &reg_value);
	p += sprintf(p, "CTRL9(0x%02x)=0x%02x\n", SX9310_CPS_CTRL9_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL10_REG, &reg_value);
	p += sprintf(p, "CTRL10(0x%02x)=0x%02x\n", SX9310_CPS_CTRL10_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL11_REG, &reg_value);
	p += sprintf(p, "CTRL11(0x%02x)=0x%02x\n", SX9310_CPS_CTRL11_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL12_REG, &reg_value);
	p += sprintf(p, "CTRL12(0x%02x)=0x%02x\n", SX9310_CPS_CTRL12_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL13_REG, &reg_value);
	p += sprintf(p, "CTRL13(0x%02x)=0x%02x\n", SX9310_CPS_CTRL13_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL14_REG, &reg_value);
	p += sprintf(p, "CTRL14(0x%02x)=0x%02x\n", SX9310_CPS_CTRL14_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL15_REG, &reg_value);
	p += sprintf(p, "CTRL15(0x%02x)=0x%02x\n", SX9310_CPS_CTRL15_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL16_REG, &reg_value);
	p += sprintf(p, "CTRL16(0x%02x)=0x%02x\n", SX9310_CPS_CTRL16_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL17_REG, &reg_value);
	p += sprintf(p, "CTRL17(0x%02x)=0x%02x\n", SX9310_CPS_CTRL17_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL18_REG, &reg_value);
	p += sprintf(p, "CTRL18(0x%02x)=0x%02x\n", SX9310_CPS_CTRL18_REG, reg_value);

	read_register(this, SX9310_CPS_CTRL19_REG, &reg_value);
	p += sprintf(p, "CTRL19(0x%02x)=0x%02x\n", SX9310_CPS_CTRL19_REG, reg_value);

	read_register(this, SX9310_SAR_CTRL0_REG, &reg_value);
	p += sprintf(p, "SCTRL0(0x%02x)=0x%02x\n", SX9310_SAR_CTRL0_REG, reg_value);

	read_register(this, SX9310_SAR_CTRL1_REG, &reg_value);
	p += sprintf(p, "SCTRL1(0x%02x)=0x%02x\n", SX9310_SAR_CTRL1_REG, reg_value);

	read_register(this, SX9310_SAR_CTRL2_REG, &reg_value);
	p += sprintf(p, "SCTRL2(0x%02x)=0x%02x\n", SX9310_SAR_CTRL2_REG, reg_value);

	reg_value = gpio_get_value(this->nirq_gpio);
	p += sprintf(p, "NIRQ=%d\n", reg_value);

	return (p-buf);
}

static ssize_t sx9310_reg_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	psx86xx_t this = dev_get_drvdata(dev);
	unsigned int val,reg,opt;

	if (sscanf(buf, "%x,%x,%x", &reg, &val, &opt) == 3) {
		this->read_reg = *((u8*)&reg);
		this->read_flag = 1;
	} else if (sscanf(buf, "%x,%x", &reg, &val) == 2) {
		pr_info("%s:reg = 0x%02x, val = 0x%02x\n", __func__, *(u8*)&reg, *(u8*)&val);
		write_register(this, *((u8*)&reg), *((u8*)&val));
		set_default_value_of_reg(this, *((u8*)&reg), *((u8*)&val));
	}

	return count;
}

static ssize_t sx9310_touch_status_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	u8 reg_value = 0;
	psx86xx_t this = dev_get_drvdata(dev);
	read_register(this, SX9310_STAT0_REG, &reg_value);
	pr_debug("Reading SX9310_STAT0_REG = %d\n", reg_value);
	reg_value &= 0x0f;
	return sprintf(buf, "0x%02x\n", reg_value);
}

static ssize_t sx9310_enable_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	psx86xx_t this = dev_get_drvdata(dev);
	return sprintf(buf, "%s\n", this->sar_enable ? "enable" : "disable" );
}

static ssize_t sx9310_enable_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	unsigned long val;
	psx86xx_t this = dev_get_drvdata(dev);
	u8 reg_val = 0x50;

	if (strict_strtoul(buf, 0, &val))
		return -EINVAL;

	if (val<0 || val>7)
		return -EINVAL;

	if (val == 0) {
		pr_info("sx9310 manual disable\n");
		if (timer_pending(&this->cali_timer))
			del_timer_sync(&this->cali_timer);
		write_register(this, SX9310_CPS_CTRL0_REG, 0x50);
		this->sar_enable = false;
	} else {
		pr_info("sx9310 manual enable\n");
		reg_val = get_default_value_of_reg(this, SX9310_CPS_CTRL0_REG);
		set_default_value_of_reg(this, SX9310_CPS_CTRL0_REG, reg_val);
		write_register(this, SX9310_CPS_CTRL0_REG, reg_val);
		manual_offset_calibration(this);
		this->sar_enable = true;
	}

	this->set_sar_state(1);

	return count;
}
static ssize_t sx9310_sarconfig_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	psx86xx_t this = dev_get_drvdata(dev);

	return sprintf(buf, "%s\n", this->contry_sar_config ? "true" : "false" );
}

static ssize_t sx9310_sarconfig_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	unsigned long val;
	psx86xx_t this = dev_get_drvdata(dev);

	if (strict_strtoul(buf, 0, &val))
		return -EINVAL;
	if (val<0 || val>1)
		return -EINVAL;

	if (val == 0) {
		dev_info( this->pdev, "sx9310 not in contry code change\n");

		write_register(this, SX9310_CPS_CTRL8_REG, 0x76);
		write_register(this, SX9310_CPS_CTRL9_REG, 0x7E);
		this->contry_sar_config= false;
	} else {
		dev_info( this->pdev, "sx9310 in contry code change\n");

		write_register(this, SX9310_CPS_CTRL8_REG, 0x56);
		write_register(this, SX9310_CPS_CTRL9_REG, 0x56);
		this->contry_sar_config= true;
	}

	return count;
}

static DEVICE_ATTR(calibrate, 0644, sx9310_manual_offs_calib_show,
								sx9310_manual_offs_calib_store);
static DEVICE_ATTR(reg, 0644, sx9310_reg_dump_show, sx9310_reg_store);
static DEVICE_ATTR(status, 0644, sx9310_touch_status_show, NULL);
static DEVICE_ATTR(enable, 0666, sx9310_enable_show, sx9310_enable_store);
static DEVICE_ATTR(sarconfig, 0666, sx9310_sarconfig_show, sx9310_sarconfig_store);
static struct attribute *sx9310_attributes[] = {
	&dev_attr_calibrate.attr,
	&dev_attr_reg.attr,
	&dev_attr_status.attr,
	&dev_attr_enable.attr,
	&dev_attr_sarconfig.attr,
	NULL,
};
static struct attribute_group sx9310_attr_group = {
	.attrs = sx9310_attributes,
};
/*********************************************************************/

/*! \fn static int read_regStat(psx86xx_t this)
 * \brief Shortcut to read what caused interrupt.
 * \details This is to keep the drivers a unified
 * function that will read whatever register(s)
 * provide information on why the interrupt was caused.
 * \param this Pointer to main parent struct
 * \return If successful, Value of bit(s) that cause interrupt, else 0
 */
static int read_regStat(psx86xx_t this)
{
	u8 data = 0;
	if (this) {
		if (read_register(this,SX9310_IRQSTAT_REG,&data) == 0)
			return (data & 0x00FF);
	}
	return 0;
}

static void read_rawData(psx86xx_t this)
{
	u8 msb=0, lsb=0;
	if(this){
		write_register(this,SX9310_CPSRD,1);//here to check the CS1, also can read other channel
		msleep(100);
		read_register(this,SX9310_USEMSB,&msb);
		read_register(this,SX9310_USELSB,&lsb);
		pr_info("sx9310 cs1 raw data USEFUL msb = 0x%x, lsb = 0x%x\n",msb,lsb);
		read_register(this,SX9310_AVGMSB,&msb);
		read_register(this,SX9310_AVGLSB,&lsb);
		pr_info("sx9310 cs1 raw data AVERAGE msb = 0x%x, lsb = 0x%x\n",msb,lsb);
		read_register(this,SX9310_DIFFMSB,&msb);
		read_register(this,SX9310_DIFFLSB,&lsb);
		pr_info("sx9310 cs1 raw data DIFF msb = 0x%x, lsb = 0x%x\n",msb,lsb);
		read_register(this,SX9310_OFFSETMSB,&msb);
		read_register(this,SX9310_OFFSETLSB,&lsb);
		pr_info("sx9310 cs1 raw data OFFSET msb = 0x%x, lsb = 0x%x\n",msb,lsb);

		write_register(this,SX9310_CPSRD,2);//here to check the CS1, also can read other channel
		msleep(100);
		read_register(this,SX9310_USEMSB,&msb);
		read_register(this,SX9310_USELSB,&lsb);
		pr_info("sx9310 cs2 raw data USEFUL msb = 0x%x, lsb = 0x%x\n",msb,lsb);
		read_register(this,SX9310_AVGMSB,&msb);
		read_register(this,SX9310_AVGLSB,&lsb);
		pr_info("sx9310 cs2 raw data AVERAGE msb = 0x%x, lsb = 0x%x\n",msb,lsb);
		read_register(this,SX9310_DIFFMSB,&msb);
		read_register(this,SX9310_DIFFLSB,&lsb);
		pr_info("sx9310 cs2 raw data DIFF msb = 0x%x, lsb = 0x%x\n",msb,lsb);
		read_register(this,SX9310_OFFSETMSB,&msb);
		read_register(this,SX9310_OFFSETLSB,&lsb);
		pr_info("sx9310 cs2 raw data OFFSET msb = 0x%x, lsb = 0x%x\n",msb,lsb);
	}
}

/*! \brief	Initialize I2C config from platform data
 * \param this Pointer to main parent struct
 */
static void hw_init(psx86xx_t this)
{
	psx9310_t pDevice = 0;
	psx9310_platform_data_t pdata = 0;
	int i = 0;
	int k = 0;
	/* configure device */
	if (this && (pDevice = this->pDevice) && (pdata = pDevice->hw))
	{
		while ( i < pdata->i2c_reg_num) {
			/* Write all registers/values contained in i2c_reg */
			pr_debug("Going to Write Reg: 0x%x Value: 0x%x\n",
				pdata->pi2c_reg[i].reg,pdata->pi2c_reg[i].val);
			write_register(this, pdata->pi2c_reg[i].reg,pdata->pi2c_reg[i].val);
			i++;
			if (this->contry_sar_config)
			{
				read_register(this, SX9310_CPS_CTRL8_REG, &k);
				dev_info(this->pdev, "-----Read Reg[0x%02x] = 0x%02x\n", SX9310_CPS_CTRL8_REG, k);

				write_register(this, SX9310_CPS_CTRL8_REG, 0x56);
				write_register(this, SX9310_CPS_CTRL9_REG, 0x56);
			}
		}
	} else {
		pr_err("ERROR! platform data 0x%p\n",pDevice->hw);
	}

	this->set_sar_state(1);//dli set sar state as 1, no touch when init.
}
/*********************************************************************/
/*! \fn static int initialize(psx86xx_t this)
 * \brief Performs all initialization needed to configure the device
 * \param this Pointer to main parent struct
 * \return Last used command's return value (negative if error)
 */
static int initialize(psx86xx_t this)
{
	if (this) {
		/* prepare reset by disabling any irq handling */
		this->irq_disabled = 1;
		disable_irq(this->irq);
		/* perform a reset */
		write_register(this,SX9310_SOFTRESET_REG,SX9310_SOFTRESET);
		/* wait until the reset has finished by monitoring NIRQ */
		pr_debug("Sent Software Reset. Waiting until device is back from reset to continue.\n");
		/* just sleep for awhile instead of using a loop with reading irq status */
		msleep(300);
		pr_debug("Device is back from the reset, continuing. NIRQ = %d\n",this->get_nirq_low());
		hw_init(this);
		msleep(100); /* make sure everything is running */
		manual_offset_calibration(this);

		/* re-enable interrupt handling */
		enable_irq(this->irq);
		this->irq_disabled = 0;

		/* make sure no interrupts are pending since enabling irq will only
		 * work on next falling edge */
		read_regStat(this);
		pr_debug("Exiting initialize(). NIRQ = %d\n",this->get_nirq_low());
		return 0;
	}
	return -ENOMEM;
}

#ifdef CALI_LOOP

static u8 get_default_value_of_reg(psx86xx_t this, u8 reg)
{
	psx9310_t pDevice = 0;
	psx9310_platform_data_t pdata = 0;
	int i = 0;
	int k = 0;
	u8 val = 0;

	if (this && (pDevice = this->pDevice) && (pdata = pDevice->hw)) {
		while ( i < pdata->i2c_reg_num) {
			if (reg == pdata->pi2c_reg[i].reg)
				return pdata->pi2c_reg[i].val;
			write_register(this, pdata->pi2c_reg[i].reg,pdata->pi2c_reg[i].val);
			i++;
			if (this->contry_sar_config)
			{
				read_register(this, SX9310_CPS_CTRL8_REG, &k);
				dev_info(this->pdev, "-----Read Reg[0x%02x] = 0x%02x\n", SX9310_CPS_CTRL8_REG, k);

				write_register(this, SX9310_CPS_CTRL8_REG, 0x56);
				write_register(this, SX9310_CPS_CTRL9_REG, 0x56);
			}
		}
	} else {
		pr_err("ERROR! platform data 0x%p\n",pDevice->hw);
	}

	read_register(this, reg, &val);
	return val;
}

static void set_default_value_of_reg(psx86xx_t this, u8 reg, u8 val)
{
	psx9310_t pDevice = 0;
	psx9310_platform_data_t pdata = 0;
	int i = 0;

	if (this && (pDevice = this->pDevice) && (pdata = pDevice->hw)) {
		while ( i < pdata->i2c_reg_num) {
			if (reg == pdata->pi2c_reg[i].reg) {
				pdata->pi2c_reg[i].val = val;
				break;
			}
			i++;
		}
	} else {
		pr_err("ERROR! platform data 0x%p\n",pDevice->hw);
	}
}

static void calibration_work(struct work_struct *work)
{
	psx86xx_t this = container_of(work, struct sx86xx , cali_work);
	pr_info("%s\n",__func__);
	dev_info(this->pdev,"this->contry_sar_config = %d\n",this->contry_sar_config);
	manual_offset_calibration(this);
	if (this->sar_enable && (this->cali_count == LOOP_COUNT))
		write_register(this, SX9310_CPS_CTRL0_REG, get_default_value_of_reg(this, SX9310_CPS_CTRL0_REG));
}

static void calibration_loop(unsigned long p)
{
	psx86xx_t this = (psx86xx_t)p;
	pr_info("%s %d \n", __func__, this->cali_count);
	queue_work(cali_workqueue, &this->cali_work);
	if (this->cali_count++ < LOOP_COUNT) {
		if (timer_pending(&this->cali_timer))
			del_timer_sync(&this->cali_timer);
		this->cali_timer.expires = jiffies + INTERVAL;
		add_timer(&this->cali_timer);
	}
}
#endif

/*!
 * \brief Handle what to do when a touch occurs
 * \param this Pointer to main parent struct
 */
static void touchProcess(psx86xx_t this)
{
	int counter = 0;
	u8 i = 0;
	u8 stat1 = 0;
	int numberOfButtons = 0;
	psx9310_t pDevice = NULL;
	struct _buttonInfo *buttons = NULL;
	int state = IDLE;

	struct _buttonInfo *pCurrentButton	  = NULL;

	if (this && (pDevice = this->pDevice))
	{
		pr_debug("Inside touchProcess()\n");
		read_register(this, SX9310_STAT0_REG, &i);
		read_register(this, SX9310_STAT1_REG, &stat1);
		pr_debug("Read Reg[0x%02x] = 0x%02x\n", SX9310_STAT0_REG, i);
		pr_debug("Read Reg[0x%02x] = 0x%02x\n", SX9310_STAT1_REG, stat1);

		buttons = pDevice->pbuttonInformation->buttons;
		numberOfButtons = pDevice->pbuttonInformation->buttonSize;

		for (counter = 0; counter < numberOfButtons; counter++) {
			pCurrentButton = &buttons[counter];
			if (pCurrentButton==NULL) {
				pr_err("ERROR!! current button at index: %d NULL!!!\n", counter);
				return; // ERRORR!!!!
			}
			switch (pCurrentButton->state) {
			case IDLE: /* Button is not being touched! */
				if (((i & pCurrentButton->mask) == pCurrentButton->mask)) {
					/* User pressed button */
					pr_info("cap button %d touched\n", counter);
					pCurrentButton->state = ACTIVE;
				} else {
					pr_debug("Button %d already released.\n",counter);
				}
				break;
			case ACTIVE: /* Button is being touched! */
				if (((i & pCurrentButton->mask) != pCurrentButton->mask)) {
					/* User released button */
					pr_info("cap button %d released\n",counter);
					pCurrentButton->state = IDLE;
				} else {
					pr_debug("Button %d still touched.\n",counter);
				}
				break;
			default: /* Shouldn't be here, device only allowed ACTIVE or IDLE */
				break;
		};
	}

	for (counter = 0; counter < numberOfButtons; counter++) {
		pCurrentButton = &buttons[counter];
		if (pCurrentButton && pCurrentButton->state == ACTIVE) {
			state = ACTIVE;
			break;
		}
	}

	// touched to released, need to calibration
	if (this->pre_state == ACTIVE && state == IDLE) {
	if (this->ignore_once_info_modem == false) {//no need to report state if we have just conducted a calibration
			this->set_sar_state(1);

			/* Loop calibration start */
			if (timer_pending(&this->cali_timer))
				del_timer_sync(&this->cali_timer);

			this->cali_timer.expires = jiffies + INTERVAL_FAR;
			add_timer(&this->cali_timer);
		}
	} else if (this->pre_state == IDLE && state == ACTIVE) {// released to touched
		this->set_sar_state(0);
		del_timer_sync(&this->cali_timer);
	}

	this->ignore_once_info_modem = false;
	this->pre_state = state;

	pr_debug("Leaving touchProcess()\n");
	}
}

static int sx9310_check_id(psx86xx_t this)
{
	u8 id;
	if(read_register(this, SX9310_WHOAMI, &id)) {
		pr_err("sx9310 check id fail\n");
		return -1;
	}

	if(id !=0x01) {
		pr_err("sx9310 id not match:0x%02x\n", id);
		return -2;
	}

	pr_info("sx9310 check id ok\n");
	return 0;
}

/*! \fn static int sx9310_probe(struct i2c_client *client, const struct i2c_device_id *id)
 * \brief Probe function
 * \param client pointer to i2c_client
 * \param id pointer to i2c_device_id
 * \return Whether probe was successful
 */
static int sx9310_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int i = 0;
	int ret;
	psx9310_t pDevice = 0;
	psx9310_platform_data_t pplatData = 0;
	psx86xx_t this;

	pr_info("%s+\n",__func__);

	pplatData = client->dev.platform_data;
	if (!pplatData) {
		pr_err("platform data is required!\n");
		return -EINVAL;
	}

	if (!i2c_check_functionality(client->adapter,
					 I2C_FUNC_SMBUS_READ_WORD_DATA))
		return -EIO;

	this = kzalloc(sizeof(sx86xx_t), GFP_KERNEL); /* create memory for main struct */

	/* SARSTATE gpio config */

	if (this)
	{
		/* In case we need to reinitialize data
		 * (e.q. if suspend reset device) */
		this->init = initialize;
		/* shortcut to read status of interrupt */
		this->refreshStatus = read_regStat;
		/* pointer to function from platform data to get pendown
		 * (1->NIRQ=0, 0->NIRQ=1) */
		this->get_nirq_low = pplatData->get_is_nirq_low;
		this->set_sar_state = pplatData->set_sar_state;//dli
		this->get_sar_state = pplatData->get_sar_state;
		this->nirq_gpio = pplatData->nirq_gpio;
		this->nstate_gpio = pplatData->nstate_gpio;

		/* save irq in case we need to reference it */
		this->irq = client->irq;
		pr_info("%s:irq: %d \n", __func__, this->irq);
		/* do we need to create an irq timer after interrupt ? */
		this->useIrqTimer = 0;
		this->ignore_once_info_modem= false;
		this->pre_state = IDLE;
		/* Setup function to call on corresponding reg irq source bit */
		if (MAX_NUM_STATUS_BITS>= 8)
		{
			this->statusFunc[0] = 0; /* TXEN_STAT */
			this->statusFunc[1] = 0; /* UNUSED */
			this->statusFunc[2] = 0; /* UNUSED */
			this->statusFunc[3] = read_rawData; /* CONV_STAT */
			this->statusFunc[4] = 0; /* COMP_STAT */
			this->statusFunc[5] = touchProcess; /* RELEASE_STAT */
			this->statusFunc[6] = touchProcess; /* TOUCH_STAT	 */
			this->statusFunc[7] = 0; /* RESET_STAT */
		}

		/* setup i2c communication */
		this->bus = client;
		i2c_set_clientdata(client, this);

		/* record device struct */
		this->pdev = &client->dev;

		if(sx9310_check_id(this)) {
			pr_err("%s failed\n",__func__);
			kfree(this);
			return -EINVAL;
		}

		/* create memory for device specific struct */
		this->pDevice = pDevice = kzalloc(sizeof(sx9310_t), GFP_KERNEL);

		if (pDevice)
		{
			/* for accessing items in user data (e.g. calibrate) */
			ret = sysfs_create_group(&client->dev.kobj, &sx9310_attr_group);
			if(ret < 0){
				pr_err("%s:failed to create sx9310 attr group\n", __func__);
				return ret;
			}

			/* Check if we hava a platform initialization function to call*/
			if (pplatData->init_platform_hw)
				pplatData->init_platform_hw();

			/* Add Pointer to main platform data struct */
			pDevice->hw = pplatData;

			/* Initialize the button information initialized with keycodes */
			pDevice->pbuttonInformation = pplatData->pbuttonInformation;

			/* Set all the keycodes */
			for (i = 0; i < pDevice->pbuttonInformation->buttonSize; i++) {
				pDevice->pbuttonInformation->buttons[i].state = IDLE;
			}
			/* save the input pointer and finish initialization */
		}
		sx86xx_init(this);

	// add calibration loop.
#ifdef CALI_LOOP
		if (!cali_workqueue)
			cali_workqueue = create_workqueue("cali_workqueue");
		INIT_WORK(&this->cali_work, calibration_work);

		init_timer(&this->cali_timer);
		this->cali_count = 0;
		this->cali_timer.function = calibration_loop;
		this->cali_timer.data = (unsigned long)this;
		this->cali_timer.expires = jiffies + INTERVAL;
		add_timer(&this->cali_timer);
#endif
		this->read_flag = 0;
		this->read_reg = SX9310_CPS_CTRL0_REG;
		this->sar_enable = true;
		this->contry_sar_config= false;
		pr_info("%s-\n",__func__);
		return	0;
	}else{
		return -1;
	}
}

/*! \fn static int sx9310_remove(struct i2c_client *client)
 * \brief Called when device is to be removed
 * \param client Pointer to i2c_client struct
 * \return Value from sx86xx_remove()
 */
static int sx9310_remove(struct i2c_client *client)
{
	psx9310_platform_data_t pplatData =0;
	psx9310_t pDevice = 0;
	psx86xx_t this = i2c_get_clientdata(client);
	if (this && (pDevice = this->pDevice))
	{
		sysfs_remove_group(&client->dev.kobj, &sx9310_attr_group);
		pplatData = client->dev.platform_data;
		if (pplatData && pplatData->exit_platform_hw)
			pplatData->exit_platform_hw();
		kfree(this->pDevice);
	}

#ifdef CALI_LOOP
	del_timer(&this->cali_timer);
	cancel_work_sync(&this->cali_work);
	flush_workqueue(cali_workqueue);
	destroy_workqueue(cali_workqueue);
#endif

	return sx86xx_remove(this);
}

#if CONFIG_PM
static int sx9310_suspend(struct device *dev)
{
	struct i2c_client *client = (struct i2c_client *)to_i2c_client(dev);
	psx86xx_t this = (psx86xx_t)i2c_get_clientdata(client);

	if (this->cali_count++ < LOOP_COUNT) {
		if (timer_pending(&this->cali_timer))
			del_timer_sync(&this->cali_timer);
	}

	pr_info("%s\n",__func__);
	write_register(this, SX9310_CPS_CTRL0_REG, 0x50);
	sx86xx_suspend(this);
	read_regStat(this);
	return 0;
}

static int sx9310_resume(struct device *dev)
{
	struct i2c_client *client = (struct i2c_client *)to_i2c_client(dev);
	psx86xx_t this = (psx86xx_t)i2c_get_clientdata(client);
	pr_info("%s\n",__func__);
	sx86xx_resume(this);

	//resume calibration.
	if (this->sar_enable == true) {
		manual_offset_calibration(this);
		write_register(this, SX9310_CPS_CTRL0_REG, get_default_value_of_reg(this, SX9310_CPS_CTRL0_REG));
		pr_info("sx9310 resume calibration.\n");
	}

	if (this->cali_count++ < LOOP_COUNT) {
		if (timer_pending(&this->cali_timer))
			del_timer_sync(&this->cali_timer);

		this->cali_timer.expires = jiffies + INTERVAL;
		add_timer(&this->cali_timer);
	}

	return 0;
}
static void sx9310_complete(struct device *dev)
{
	pr_info("%s\n",__func__);
	sx9310_resume(dev);
}
static const struct dev_pm_ops sx9310_pm = {
	.prepare = sx9310_suspend,
	.complete = sx9310_complete,
};
#endif

/*====================================================*/
static struct i2c_device_id sx9310_idtable[] = {
	{ DRIVER_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sx9310_idtable);
static struct i2c_driver sx9310_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= DRIVER_NAME,
#if CONFIG_PM
		.pm	= &sx9310_pm,
#endif
	},
	.id_table = sx9310_idtable,
	.probe		= sx9310_probe,
	.remove		= /*__devexit_p*/(sx9310_remove),
};
static int __init sx9310_init(void)
{
	return i2c_add_driver(&sx9310_driver);
}
static void __exit sx9310_exit(void)
{
	i2c_del_driver(&sx9310_driver);
}

module_init(sx9310_init);
module_exit(sx9310_exit);

MODULE_AUTHOR("Semtech Corp. (http://www.semtech.com/)");
MODULE_DESCRIPTION("SX9310 Capacitive Touch Controller Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");
