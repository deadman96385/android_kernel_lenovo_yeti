/*
 * Support for OmniVision OV7251 VGA IR camera sensor.
 *
 * Copyright (c) 2016 Intel Corporation. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/input.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/moduleparam.h>
#include <media/v4l2-device.h>
#include <linux/atomisp_platform.h>

#include <linux/acpi.h>

#include "ov7251.h"

/* i2c reg read/write */
static int ov7251_read_reg(struct i2c_client *client,
			   u16 data_length, u16 reg, u16 *val)
{
	int err;
	struct i2c_msg msg[2];
	unsigned char data[6];

	if (!client->adapter) {
		dev_err(&client->dev, "%s error, no client->adapter\n",
			__func__);
		return -ENODEV;
	}

	if (data_length != OV7251_8BIT && data_length != OV7251_16BIT
					&& data_length != OV7251_32BIT) {
		dev_err(&client->dev, "%s error, invalid data length\n",
			__func__);
		return -EINVAL;
	}

	memset(msg, 0 , sizeof(msg));

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = I2C_MSG_LENGTH;
	msg[0].buf = data;

	/* high byte goes out first */
	data[0] = (u8)(reg >> 8);
	data[1] = (u8)(reg & 0xff);

	msg[1].addr = client->addr;
	msg[1].len = data_length;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = data;

	err = i2c_transfer(client->adapter, msg, 2);
	if (err != 2) {
		if (err >= 0)
			err = -EIO;
		dev_err(&client->dev,
			"read from offset 0x%x error %d", reg, err);
		return err;
	}

	*val = 0;
	/* high byte comes first */
	if (data_length == OV7251_8BIT)
		*val = (u8)data[0];
	else if (data_length == OV7251_16BIT)
		*val = be16_to_cpu(*(u16 *)&data[0]);
	else
		*val = be32_to_cpu(*(u32 *)&data[0]);

	return 0;
}

static int ov7251_i2c_write(struct i2c_client *client, u16 len, u8 *data)
{
	struct i2c_msg msg;
	const int num_msg = 1;
	int ret;

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = len;
	msg.buf = data;
	ret = i2c_transfer(client->adapter, &msg, 1);

	return ret == num_msg ? 0 : -EIO;
}

static int ov7251_write_reg(struct i2c_client *client, u16 data_length,
							u16 reg, u16 val)
{
	int ret;
	unsigned char data[4] = {0};
	u16 *wreg = (u16 *)data;
	const u16 len = data_length + sizeof(u16); /* 16-bit address + data */

	if (data_length != OV7251_8BIT && data_length != OV7251_16BIT) {
		dev_err(&client->dev,
			"%s error, invalid data_length\n", __func__);
		return -EINVAL;
	}

	/* high byte goes out first */
	*wreg = cpu_to_be16(reg);

	if (data_length == OV7251_8BIT) {
		data[2] = (u8)(val);
	} else {
		/* OV7251_16BIT */
		u16 *wdata = (u16 *)&data[2];
		*wdata = cpu_to_be16(val);
	}

	ret = ov7251_i2c_write(client, len, data);
	if (ret)
		dev_err(&client->dev,
			"write error: wrote 0x%x to offset 0x%x error %d",
			val, reg, ret);

	return ret;
}

/*
 * ov7251_write_reg_array - Initializes a list of OV7251 registers
 * @client: i2c driver client structure
 * @reglist: list of registers to be written
 *
 * This function initializes a list of registers. When consecutive addresses
 * are found in a row on the list, this function creates a buffer and sends
 * consecutive data in a single i2c_transfer().
 *
 * __ov7251_flush_reg_array, __ov7251_buf_reg_array() and
 * __ov7251_write_reg_is_consecutive() are internal functions to
 * ov7251_write_reg_array_fast() and should be not used anywhere else.
 *
 */

static int __ov7251_flush_reg_array(struct i2c_client *client,
				    struct ov7251_write_ctrl *ctrl)
{
	u16 size;

	if (ctrl->index == 0)
		return 0;

	size = sizeof(u16) + ctrl->index; /* 16-bit address + data */
	ctrl->buffer.addr = cpu_to_be16(ctrl->buffer.addr);
	ctrl->index = 0;

	return ov7251_i2c_write(client, size, (u8 *)&ctrl->buffer);
}

static int __ov7251_buf_reg_array(struct i2c_client *client,
				  struct ov7251_write_ctrl *ctrl,
				  const struct ov7251_reg *next)
{
	int size;
	u16 *data16;

	switch (next->type) {
	case OV7251_8BIT:
		size = 1;
		ctrl->buffer.data[ctrl->index] = (u8)next->val;
		break;
	case OV7251_16BIT:
		size = 2;
		data16 = (u16 *)&ctrl->buffer.data[ctrl->index];
		*data16 = cpu_to_be16((u16)next->val);
		break;
	default:
		return -EINVAL;
	}

	/* When first item is added, we need to store its starting address */
	if (ctrl->index == 0)
		ctrl->buffer.addr = next->reg;

	ctrl->index += size;

	/*
	 * Buffer cannot guarantee free space for u32? Better flush it to avoid
	 * possible lack of memory for next item.
	 */
	if (ctrl->index + sizeof(u16) >= OV7251_MAX_WRITE_BUF_SIZE)
		return __ov7251_flush_reg_array(client, ctrl);

	return 0;
}

static int __ov7251_write_reg_is_consecutive(struct i2c_client *client,
					     struct ov7251_write_ctrl *ctrl,
					     const struct ov7251_reg *next)
{
	if (ctrl->index == 0)
		return 1;

	return ctrl->buffer.addr + ctrl->index == next->reg;
}

static int ov7251_write_reg_array(struct i2c_client *client,
				  const struct ov7251_reg *reglist)
{
	const struct ov7251_reg *next = reglist;
	struct ov7251_write_ctrl ctrl;
	int err;

	ctrl.index = 0;
	for (; next->type != OV7251_TOK_TERM; next++) {
		switch (next->type & OV7251_TOK_MASK) {
		case OV7251_TOK_DELAY:
			err = __ov7251_flush_reg_array(client, &ctrl);
			if (err)
				return err;
			msleep(next->val);
			break;
		default:
			/*
			 * If next address is not consecutive, data needs to be
			 * flushed before proceed.
			 */
			if (!__ov7251_write_reg_is_consecutive(client, &ctrl,
								next)) {
				err = __ov7251_flush_reg_array(client, &ctrl);
				if (err)
					return err;
			}
			err = __ov7251_buf_reg_array(client, &ctrl, next);
			if (err) {
				dev_err(&client->dev,
					"%s: write error, aborted\n", __func__);
				return err;
			}
			break;
		}
	}

	return __ov7251_flush_reg_array(client, &ctrl);
}
static int ov7251_g_focal(struct v4l2_subdev *sd, s32 *val)
{
	*val = (OV7251_FOCAL_LENGTH_NUM << 16) | OV7251_FOCAL_LENGTH_DEM;
	return 0;
}

static int ov7251_g_fnumber(struct v4l2_subdev *sd, s32 *val)
{
	*val = (OV7251_F_NUMBER_DEFAULT_NUM << 16) | OV7251_F_NUMBER_DEM;
	return 0;
}

static int ov7251_g_fnumber_range(struct v4l2_subdev *sd, s32 *val)
{
	*val = (OV7251_F_NUMBER_DEFAULT_NUM << 24) |
		(OV7251_F_NUMBER_DEM << 16) |
		(OV7251_F_NUMBER_DEFAULT_NUM << 8) | OV7251_F_NUMBER_DEM;
	return 0;
}

static int ov7251_get_intg_factor(struct i2c_client *client,
				struct camera_mipi_info *info,
				const struct ov7251_resolution *res)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	struct atomisp_sensor_mode_data *buf = &info->data;
	const unsigned int ext_clk_freq_hz = 19200000;
	const unsigned int pll_invariant_div = 10;
	unsigned int pix_clk_freq_hz;
	u16 pre_pll_clk_div;
	u16 pll_multiplier;
	u16 op_pix_clk_div;
	u16 reg_val;
	int ret;

	if (info == NULL)
		return -EINVAL;

	/* pixel clock calculattion */
	ret = ov7251_read_reg(client, OV7251_8BIT,
				OV7251_SC_CMMN_PLL_CTRL3, &pre_pll_clk_div);
	if (ret)
		return ret;

	ret = ov7251_read_reg(client, OV7251_8BIT,
				OV7251_SC_CMMN_PLL_MULTIPLIER, &pll_multiplier);
	if (ret)
		return ret;

	ret = ov7251_read_reg(client, OV7251_8BIT,
				OV7251_SC_CMMN_PLL_DEBUG_OPT, &op_pix_clk_div);
	if (ret)
		return ret;

	pre_pll_clk_div = (pre_pll_clk_div & 0x70) >> 4;
	if (0 == pre_pll_clk_div)
		return -EINVAL;

	pll_multiplier = pll_multiplier & 0x7f;
	op_pix_clk_div = op_pix_clk_div & 0x03;
	pix_clk_freq_hz = ext_clk_freq_hz / pre_pll_clk_div * pll_multiplier
				* op_pix_clk_div/pll_invariant_div;

	dev->vt_pix_clk_freq_mhz = pix_clk_freq_hz;
	buf->vt_pix_clk_freq_mhz = pix_clk_freq_hz;

	/* get integration time */
	buf->coarse_integration_time_min = OV7251_COARSE_INTG_TIME_MIN;
	buf->coarse_integration_time_max_margin =
					OV7251_COARSE_INTG_TIME_MAX_MARGIN;

	buf->fine_integration_time_min = OV7251_FINE_INTG_TIME_MIN;
	buf->fine_integration_time_max_margin =
					OV7251_FINE_INTG_TIME_MAX_MARGIN;

	buf->fine_integration_time_def = OV7251_FINE_INTG_TIME_MIN;
	buf->frame_length_lines = res->lines_per_frame;
	buf->line_length_pck = res->pixels_per_line;
	buf->read_mode = res->bin_mode;

	/* get the cropping and output resolution to ISP for this mode. */
	ret =  ov7251_read_reg(client, OV7251_16BIT,
					OV7251_H_CROP_START_H, &reg_val);
	if (ret)
		return ret;
	buf->crop_horizontal_start = reg_val;

	ret =  ov7251_read_reg(client, OV7251_16BIT,
					OV7251_V_CROP_START_H, &reg_val);
	if (ret)
		return ret;
	buf->crop_vertical_start = reg_val;

	ret = ov7251_read_reg(client, OV7251_16BIT,
					OV7251_H_CROP_END_H, &reg_val);
	if (ret)
		return ret;
	buf->crop_horizontal_end = reg_val;

	ret = ov7251_read_reg(client, OV7251_16BIT,
					OV7251_V_CROP_END_H, &reg_val);
	if (ret)
		return ret;
	buf->crop_vertical_end = reg_val;

	ret = ov7251_read_reg(client, OV7251_16BIT,
					OV7251_H_OUTSIZE_H, &reg_val);
	if (ret)
		return ret;
	buf->output_width = reg_val;

	ret = ov7251_read_reg(client, OV7251_16BIT,
					OV7251_V_OUTSIZE_H, &reg_val);
	if (ret)
		return ret;

	buf->output_height = reg_val;
	buf->binning_factor_x = res->bin_factor_x ? res->bin_factor_x : 1;
	buf->binning_factor_y = res->bin_factor_y ? res->bin_factor_y : 1;

	return 0;
}

static long __ov7251_set_exposure(struct v4l2_subdev *sd, int exposure,
					int gain, int digital_gain)

{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	int ret;

	ret = ov7251_write_reg(client, OV7251_8BIT, OV7251_GROUP_ACCESS,
			       OV7251_START_GROUP_HOLD);
	if (ret)
		return ret;

	ret = ov7251_write_reg(client, OV7251_8BIT, OV7251_AEC_PK_EXPO_L,
						exposure & 0xFF);
	if (ret)
		return ret;

	ret = ov7251_write_reg(client, OV7251_8BIT, OV7251_AEC_PK_EXPO_M,
						(exposure >> 8) & 0xFF);
	if (ret)
		return ret;

	ret = ov7251_write_reg(client, OV7251_8BIT, OV7251_AEC_PK_EXPO_H,
						(exposure >> 16) & 0xFF);
	if (ret)
		return ret;

	ret = ov7251_write_reg(client, OV7251_8BIT, OV7251_AEC_GAIN,
						gain & 0xFF);
	if (ret)
		return ret;

	ret = ov7251_write_reg(client, OV7251_8BIT, OV7251_GROUP_ACCESS,
			       OV7251_END_GROUP_HOLD);
	if (ret)
		return ret;

	ret = ov7251_write_reg(client, OV7251_8BIT, OV7251_GROUP_ACCESS,
			       OV7251_LAUNCH_GROUP_HOLD);
	if (ret)
		return ret;

	dev->gain = gain;
	dev->exposure = exposure;
	dev->digital_gain = digital_gain;

	return 0;
}

static int ov7251_set_exposure(struct v4l2_subdev *sd, int exposure,
	int gain, int digital_gain)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	int ret;

	mutex_lock(&dev->input_lock);
	exposure = clamp_t(int, exposure, 0, OV7251_MAX_EXPOSURE_VALUE);

	/* Validate gain: must not exceed maximum 8bit value */
	gain = clamp_t(int, gain, 0, OV7251_MAX_GAIN_VALUE);

	/* Validate digital gain: must not exceed 12 bit value*/
	digital_gain = clamp_t(int, digital_gain, 0, OV7251_MWB_GAIN_MAX);

	ret = __ov7251_set_exposure(sd, exposure, gain, digital_gain);
	mutex_unlock(&dev->input_lock);

	return ret;
}

static long ov7251_s_exposure(struct v4l2_subdev *sd,
			       struct atomisp_exposure *exposure)
{
	int exp = exposure->integration_time[0];
	int gain = exposure->gain[0];
	int digitgain = exposure->gain[1];

	return ov7251_set_exposure(sd, exp, gain, digitgain);
}

static long ov7251_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{

	switch (cmd) {
	case ATOMISP_IOC_S_EXPOSURE:
		return ov7251_s_exposure(sd, arg);
	default:
		return -EINVAL;
	}
	return 0;
}


/*
 * This returns the exposure time being used. This should only be used
 * for filling in EXIF data, not for actual image processing.
 */
static int ov7251_q_exposure(struct v4l2_subdev *sd, s32 *value)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	*value = dev->exposure;

	return 0;
}
struct ov7251_control ov7251_controls[] = {
	{
		.qc = {
			.id = V4L2_CID_EXPOSURE_ABSOLUTE,
			.type = V4L2_CTRL_TYPE_INTEGER,
			.name = "exposure",
			.minimum = 0x0,
			.maximum = 0xfffff,
			.step = 0x01,
			.default_value = 0x00,
			.flags = 0,
		},
		.query = ov7251_q_exposure,
	},
	{
		.qc = {
			.id = V4L2_CID_FOCAL_ABSOLUTE,
			.type = V4L2_CTRL_TYPE_INTEGER,
			.name = "focal length",
			.minimum = OV7251_FOCAL_LENGTH_DEFAULT,
			.maximum = OV7251_FOCAL_LENGTH_DEFAULT,
			.step = 0x01,
			.default_value = OV7251_FOCAL_LENGTH_DEFAULT,
			.flags = 0,
		},
		.query = ov7251_g_focal,
	},
	{
		.qc = {
			.id = V4L2_CID_FNUMBER_ABSOLUTE,
			.type = V4L2_CTRL_TYPE_INTEGER,
			.name = "f-number",
			.minimum = OV7251_F_NUMBER_DEFAULT,
			.maximum = OV7251_F_NUMBER_DEFAULT,
			.step = 0x01,
			.default_value = OV7251_F_NUMBER_DEFAULT,
			.flags = 0,
		},
		.query = ov7251_g_fnumber,
	},
	{
		.qc = {
			.id = V4L2_CID_FNUMBER_RANGE,
			.type = V4L2_CTRL_TYPE_INTEGER,
			.name = "f-number range",
			.minimum = OV7251_F_NUMBER_RANGE,
			.maximum =  OV7251_F_NUMBER_RANGE,
			.step = 0x01,
			.default_value = OV7251_F_NUMBER_RANGE,
			.flags = 0,
		},
		.query = ov7251_g_fnumber_range,
	},
};
#define N_CONTROLS (ARRAY_SIZE(ov7251_controls))

static int ov7251_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov7251_device *dev = container_of(ctrl->handler,
					struct ov7251_device, ctrl_handler);
	unsigned int val;

	switch (ctrl->id) {
	case V4L2_CID_LINK_FREQ:
		val = ov7251_res[dev->fmt_idx].mipi_freq;
		if (val == 0)
			return -EINVAL;

		ctrl->val = val * 1000;			/* To Hz */
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static struct ov7251_control *ov7251_find_control(u32 id)
{
	int i;

	for (i = 0; i < N_CONTROLS; i++)
		if (ov7251_controls[i].qc.id == id)
			return &ov7251_controls[i];
	return NULL;
}

static int ov7251_queryctrl(struct v4l2_subdev *sd, struct v4l2_queryctrl *qc)
{
	struct ov7251_control *ctrl = ov7251_find_control(qc->id);
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	if (ctrl == NULL)
		return -EINVAL;

	mutex_lock(&dev->input_lock);
	*qc = ctrl->qc;
	mutex_unlock(&dev->input_lock);

	return 0;
}

static int ov7251_g_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct ov7251_control *s_ctrl;
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	int ret;

	if (!ctrl)
		return -EINVAL;

	s_ctrl = ov7251_find_control(ctrl->id);
	if ((s_ctrl == NULL) || (s_ctrl->query == NULL))
		return -EINVAL;

	mutex_lock(&dev->input_lock);
	ret = s_ctrl->query(sd, &ctrl->value);
	mutex_unlock(&dev->input_lock);

	return ret;
}

static int ov7251_s_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct ov7251_control *octrl = ov7251_find_control(ctrl->id);
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	int ret;

	if ((octrl == NULL) || (octrl->tweak == NULL))
		return -EINVAL;

	mutex_lock(&dev->input_lock);
	ret = octrl->tweak(sd, ctrl->value);
	mutex_unlock(&dev->input_lock);

	return ret;
}

static int ov7251_init(struct v4l2_subdev *sd)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	mutex_lock(&dev->input_lock);

	/* restore settings */
	ov7251_res = ov7251_res_preview;
	N_RES = N_RES_PREVIEW;

	dev->fps_idx = 0;
	dev->exposure = 256;
	dev->gain = 16;
	dev->digital_gain = 1024;

	/* TODO: add basic setting writing here */

	mutex_unlock(&dev->input_lock);

	return 0;
}

static int power_ctrl(struct v4l2_subdev *sd, bool flag)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	int ret = 0;

	if (!dev || !dev->platform_data)
		return -ENODEV;

	/* FIXME: skip power cycle temporarily since regulator is disabled. */
	return ret;

	/* Non-gmin platforms use the legacy callback */
	if (dev->platform_data->power_ctrl)
		return dev->platform_data->power_ctrl(sd, flag);

#ifdef CONFIG_INTEL_MID_ISP
	if (flag) {
		ret = dev->platform_data->v2p8_ctrl(sd, 1);
		if (!ret) {
			ret = dev->platform_data->v1p8_ctrl(sd, 1);
			if (ret)
				dev->platform_data->v2p8_ctrl(sd, 0);
		}
	} else {
		ret = dev->platform_data->v1p8_ctrl(sd, 0);
		ret |= dev->platform_data->v2p8_ctrl(sd, 0);
	}
#endif

	return ret;
}

static int gpio_ctrl(struct v4l2_subdev *sd, bool flag)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	int ret = 0;

	if (!dev || !dev->platform_data)
		return -ENODEV;

	/* Non-gmin platforms use the legacy callback */
	if (dev->platform_data->gpio_ctrl)
		return dev->platform_data->gpio_ctrl(sd, flag);

#ifdef CONFIG_INTEL_MID_ISP
	ret = dev->platform_data->gpio0_ctrl(sd, flag);
	if (ret)
		return ret;

	ret = dev->platform_data->gpio1_ctrl(sd, flag);
	if (ret) {
		dev->platform_data->gpio0_ctrl(sd, !flag);
		return ret;
	}
#endif
	return ret;
}

static int power_up(struct v4l2_subdev *sd)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	if (!dev->platform_data) {
		dev_err(&client->dev, "no camera_sensor_platform_data");
		return -ENODEV;
	}

	/* power control */
	ret = power_ctrl(sd, 1);
	if (ret)
		goto out;

	/* according to DS, at least 5ms is needed between DOVDD and PWDN */
	usleep_range(5000, 6000);

	/* gpio ctrl */
	ret = gpio_ctrl(sd, 1);
	if (ret)
		goto out;

	ret = dev->platform_data->flisclk_ctrl(sd, 1);
	if (ret)
		goto out_gpio;

	/* according to DS, 20ms is needed between PWDN and i2c access */
	msleep(20);

	return 0;

out_gpio:
	gpio_ctrl(sd, 0);
out:
	dev_err(&client->dev, "sensor power-up failed\n");

	return ret;
}

static int power_down(struct v4l2_subdev *sd)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	if (!dev->platform_data) {
		dev_err(&client->dev, "no camera_sensor_platform_data");
		return -ENODEV;
	}

	ret = dev->platform_data->flisclk_ctrl(sd, 0);
	if (ret)
		dev_err(&client->dev, "flisclk failed\n");

	/* gpio ctrl */
	ret |= gpio_ctrl(sd, 0);
	if (ret)
		dev_err(&client->dev, "gpio failed.\n");

	/* power control */
	ret |= power_ctrl(sd, 0);
	if (ret)
		dev_err(&client->dev, "vprog failed.\n");

	return ret;
}

static int ov7251_s_power(struct v4l2_subdev *sd, int on)
{

	if (on) {
		if (!power_up(sd))
			return ov7251_init(sd);
	} else {
		return power_down(sd);
	}

	return 0;
}

/*
 * distance - calculate the distance
 * @res: resolution
 * @w: width
 * @h: height
 *
 * Get the gap between resolution and w/h.
 * res->width/height smaller than w/h wouldn't be considered.
 * Returns the value of gap or -1 if fail.
 */
#define LARGEST_ALLOWED_RATIO_MISMATCH 800
static int distance(struct ov7251_resolution *res, u32 w, u32 h)
{
	unsigned int w_ratio = ((res->width << 13)/w);
	unsigned int h_ratio;
	int match;

	if (h == 0)
		return -1;
	h_ratio = ((res->height << 13) / h);
	if (h_ratio == 0)
		return -1;
	match   = abs(((w_ratio << 13) / h_ratio) - ((int)8192));

	if ((w_ratio < (int)8192) || (h_ratio < (int)8192)  ||
		(match > LARGEST_ALLOWED_RATIO_MISMATCH))
		return -1;

	return w_ratio + h_ratio;
}

/* Return the nearest higher resolution index */
static int nearest_resolution_index(int w, int h)
{
	int i;
	int idx = -1;
	int min_dist = INT_MAX;

	for (i = 0; i < N_RES; i++) {
		struct ov7251_resolution *tmp_res =
					&ov7251_res[i];
		int dist = distance(tmp_res, w, h);

		if (dist == -1)
			continue;
		if (dist < min_dist) {
			min_dist = dist;
			idx = i;
		}
	}

	return idx;
}

static int get_resolution_index(int w, int h)
{
	int i;

	for (i = 0; i < N_RES; i++) {
		if (w != ov7251_res[i].width)
			continue;
		if (h != ov7251_res[i].height)
			continue;

		return i;
	}

	return -EINVAL;
}

static int ov7251_try_mbus_fmt(struct v4l2_subdev *sd,
			struct v4l2_mbus_framefmt *fmt)
{
	int idx;

	if (!fmt)
		return -EINVAL;
	idx = nearest_resolution_index(fmt->width,
					fmt->height);
	if (idx == -1) {
		/* return the largest resolution */
		fmt->width = ov7251_res[N_RES - 1].width;
		fmt->height = ov7251_res[N_RES - 1].height;
	} else {
		fmt->width = ov7251_res[idx].width;
		fmt->height = ov7251_res[idx].height;
	}
	fmt->code = V4L2_MBUS_FMT_SGRBG10_1X10;

	return 0;
}

/* TODO: remove it. */
static int startup(struct v4l2_subdev *sd)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	ret = ov7251_write_reg(client, OV7251_8BIT, OV7251_SW_RESET, 0x01);
	if (ret) {
		dev_err(&client->dev, "ov7251 reset err.\n");
		return ret;
	}

	usleep_range(5000, 6000);

	ret = ov7251_write_reg_array(client, ov7251_res[dev->fmt_idx].regs);
	if (ret) {
		dev_err(&client->dev, "ov7251 write register err.\n");
		return ret;
	}

	return ret;
}

static int ov7251_s_mbus_fmt(struct v4l2_subdev *sd,
			     struct v4l2_mbus_framefmt *fmt)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_mipi_info *ov7251_info = NULL;
	int ret = 0;

	ov7251_info = v4l2_get_subdev_hostdata(sd);
	if (ov7251_info == NULL)
		return -EINVAL;

	mutex_lock(&dev->input_lock);
	ret = ov7251_try_mbus_fmt(sd, fmt);
	if (ret) {
		dev_err(&client->dev, "try fmt fail\n");
		goto err;
	}

	dev->fmt_idx = get_resolution_index(fmt->width,
					      fmt->height);
	if (dev->fmt_idx < 0) {
		dev_err(&client->dev, "get resolution fail\n");
		mutex_unlock(&dev->input_lock);
		return -EINVAL;
	}

	dev->pixels_per_line = ov7251_res[dev->fmt_idx].pixels_per_line;
	dev->lines_per_frame = ov7251_res[dev->fmt_idx].lines_per_frame;

	ret = startup(sd);
	if (ret) {
		dev_err(&client->dev, "ov7251 startup err\n");
		goto err;
	}

	ret = ov7251_get_intg_factor(client, ov7251_info,
					&ov7251_res[dev->fmt_idx]);
	if (ret)
		dev_err(&client->dev, "failed to get integration_factor\n");

err:
	mutex_unlock(&dev->input_lock);
	return ret;
}
static int ov7251_g_mbus_fmt(struct v4l2_subdev *sd,
			     struct v4l2_mbus_framefmt *fmt)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	if (!fmt)
		return -EINVAL;

	fmt->width = ov7251_res[dev->fmt_idx].width;
	fmt->height = ov7251_res[dev->fmt_idx].height;
	fmt->code = V4L2_MBUS_FMT_SBGGR10_1X10;

	return 0;
}

static int ov7251_detect(struct i2c_client *client)
{
	struct i2c_adapter *adapter = client->adapter;
	u16 high, low, id, revision;
	int ret;

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C))
		return -ENODEV;

	ret = ov7251_read_reg(client, OV7251_8BIT,
					OV7251_SC_CMMN_CHIP_ID_H, &high);
	if (ret) {
		dev_err(&client->dev, "sensor_id_high = 0x%x\n", high);
		return -ENODEV;
	}
	ret = ov7251_read_reg(client, OV7251_8BIT,
					OV7251_SC_CMMN_CHIP_ID_L, &low);
	if (ret) {
		dev_err(&client->dev, "sensor_id_low = 0x%x\n", low);
		return -ENODEV;
	}

	id = (high << 8) | low;
	if (id != OV7251_ID) {
		dev_err(&client->dev, "sensor ID error.0x%x\n", id);
		return -ENODEV;
	}

	ret = ov7251_read_reg(client, OV7251_8BIT,
					OV7251_SC_CMMN_SUB_ID, &revision);

	dev_info(&client->dev, "detect ov7251 rev.[0x%x] success\n",
		 revision);

	return 0;
}

static int ov7251_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	mutex_lock(&dev->input_lock);
	ret = ov7251_write_reg(client, OV7251_8BIT, OV7251_SW_STREAM,
				enable ? OV7251_START_STREAMING :
				OV7251_STOP_STREAMING);
	mutex_unlock(&dev->input_lock);
	return ret;
}

/* ov7251 enum frame size, frame intervals */
static int ov7251_enum_framesizes(struct v4l2_subdev *sd,
				  struct v4l2_frmsizeenum *fsize)
{
	unsigned int index = fsize->index;

	if (index >= N_RES)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = ov7251_res[index].width;
	fsize->discrete.height = ov7251_res[index].height;
	fsize->reserved[0] = ov7251_res[index].used;

	return 0;
}

static int ov7251_enum_frameintervals(struct v4l2_subdev *sd,
				      struct v4l2_frmivalenum *fival)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	unsigned int index = fival->index;

	if (index >= N_RES)
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->width = ov7251_res[index].width;
	fival->height = ov7251_res[index].height;
	fival->discrete.numerator = 1;
	fival->discrete.denominator =
			ov7251_res[index].fps_option[dev->fps_idx].fps;

	return 0;
}

static int ov7251_enum_mbus_fmt(struct v4l2_subdev *sd,
				unsigned int index,
				enum v4l2_mbus_pixelcode *code)
{
	if (index >= N_RES)
		return -EINVAL;

	*code = V4L2_MBUS_FMT_SBGGR10_1X10;

	return 0;
}

static int ov7251_s_config(struct v4l2_subdev *sd, int irq, void *platform_data)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	if (platform_data == NULL)
		return -ENODEV;

	dev->platform_data =
	    (struct camera_sensor_platform_data *)platform_data;

	mutex_lock(&dev->input_lock);
	if (dev->platform_data->platform_init) {
		ret = dev->platform_data->platform_init(client);
		if (ret) {
			dev_err(&client->dev, "platform init err\n");
			goto out;
		}
	}
	/*
	 * power off the module, then power on it in future
	 * as first power on by board may not fulfill the
	 * power on sequqence needed by the module
	 */
	ret = power_down(sd);
	if (ret)
		dev_err(&client->dev, "ov7251 power-off err.\n");

	ret = power_up(sd);
	if (ret) {
		dev_err(&client->dev, "ov7251 power-up err.\n");
		goto power_failed;
	}

	ret = dev->platform_data->csi_cfg(sd, 1);
	if (ret) {
		dev_err(&client->dev, "ov7251 csi config err.\n");
		goto power_failed;
	}

	/* config & detect sensor */
	ret = ov7251_detect(client);
	if (ret) {
		dev_err(&client->dev, "ov7251_detect err s_config.\n");
		goto csi_cfg_failed;
	}

	/* turn off sensor, after probed */
	ret = power_down(sd);
	if (ret) {
		dev_err(&client->dev, "ov7251 power-off err.\n");
		goto csi_cfg_failed;
	}

	mutex_unlock(&dev->input_lock);

	return 0;

detetc_failed:
	dev->platform_data->csi_cfg(sd, 0);
csi_cfg_failed:
	power_down(sd);
power_failed:
	if (dev->platform_data->platform_deinit)
		dev->platform_data->platform_deinit();
out:
	mutex_unlock(&dev->input_lock);
	dev_err(&client->dev, "sensor s_config failed\n");
	return ret;
}

static int ov7251_g_parm(struct v4l2_subdev *sd,
			struct v4l2_streamparm *param)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	if (!param)
		return -EINVAL;

	if (param->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		dev_err(&client->dev,  "unsupported buffer type.\n");
		return -EINVAL;
	}

	memset(param, 0, sizeof(*param));
	param->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (dev->fmt_idx >= 0 && dev->fmt_idx < N_RES) {
		param->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
		param->parm.capture.timeperframe.numerator = 1;
		param->parm.capture.capturemode = dev->run_mode;
		param->parm.capture.timeperframe.denominator =
			ov7251_res[dev->fmt_idx].fps_option[dev->fps_idx].fps;
	}
	return 0;
}

static int ov7251_s_parm(struct v4l2_subdev *sd,
			struct v4l2_streamparm *param)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	dev->run_mode = param->parm.capture.capturemode;

	mutex_lock(&dev->input_lock);
	switch (dev->run_mode) {
	case CI_MODE_VIDEO:
		ov7251_res = ov7251_res_video;
		N_RES = N_RES_VIDEO;
		break;
	case CI_MODE_STILL_CAPTURE:
		ov7251_res = ov7251_res_still;
		N_RES = N_RES_STILL;
		break;
	default:
		ov7251_res = ov7251_res_preview;
		N_RES = N_RES_PREVIEW;
	}
	mutex_unlock(&dev->input_lock);
	return 0;
}

static int ov7251_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *interval)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	mutex_lock(&dev->input_lock);
	interval->interval.numerator = 1;
	interval->interval.denominator =
	    ov7251_res[dev->fmt_idx].fps_option[dev->fps_idx].fps;
	mutex_unlock(&dev->input_lock);

	return 0;
}

static int ov7251_enum_mbus_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_fh *fh,
				struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= N_RES)
		return -EINVAL;

	code->code = V4L2_MBUS_FMT_SBGGR10_1X10;
	return 0;
}

static int ov7251_enum_frame_size(struct v4l2_subdev *sd,
				struct v4l2_subdev_fh *fh,
				struct v4l2_subdev_frame_size_enum *f)
{
	if (f->index >= N_RES)
		return -EINVAL;

	f->min_width = ov7251_res[f->index].width;
	f->min_height = ov7251_res[f->index].height;
	f->max_width = ov7251_res[f->index].width;
	f->max_height = ov7251_res[f->index].height;

	return 0;

}

static struct v4l2_mbus_framefmt *
__ov7251_get_pad_format(struct ov7251_device *sensor,
			struct v4l2_subdev_fh *fh, unsigned int pad,
			enum v4l2_subdev_format_whence which)
{
	struct i2c_client *client = v4l2_get_subdevdata(&sensor->sd);

	if (pad != 0) {
		dev_err(&client->dev,
			"__ov7251_get_pad_format err. pad %x\n", pad);
		return NULL;
	}

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(fh, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &sensor->format;
	default:
		return NULL;
	}
}

static int ov7251_get_pad_format(struct v4l2_subdev *sd,
				struct v4l2_subdev_fh *fh,
				struct v4l2_subdev_format *fmt)
{
	struct ov7251_device *snr = to_ov7251_sensor(sd);
	struct v4l2_mbus_framefmt *format =
			__ov7251_get_pad_format(snr, fh, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;
	return 0;
}

static int ov7251_set_pad_format(struct v4l2_subdev *sd,
				struct v4l2_subdev_fh *fh,
				struct v4l2_subdev_format *fmt)
{
	struct ov7251_device *snr = to_ov7251_sensor(sd);

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		snr->format = fmt->format;

	return 0;
}

static int ov7251_g_skip_frames(struct v4l2_subdev *sd, u32 *frames)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	mutex_lock(&dev->input_lock);
	*frames = ov7251_res[dev->fmt_idx].skip_frames;
	mutex_unlock(&dev->input_lock);

	return 0;
}

static const struct v4l2_subdev_sensor_ops ov7251_sensor_ops = {
	.g_skip_frames	= ov7251_g_skip_frames,
};

static struct v4l2_ctrl_ops ov7251_ctrl_ops = {
	.g_volatile_ctrl = ov7251_g_volatile_ctrl,
};

static const struct v4l2_ctrl_config v4l2_ctrl_link_freq = {
	.ops = &ov7251_ctrl_ops,
	.id = V4L2_CID_LINK_FREQ,
	.name = "Link Frequency",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 1,
	.max = 1500000 * 1000,
	.step = 1,
	.def = 1,
	.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
};

static const struct v4l2_subdev_video_ops ov7251_video_ops = {
	.s_stream = ov7251_s_stream,
	.g_parm = ov7251_g_parm,
	.s_parm = ov7251_s_parm,
	.enum_framesizes = ov7251_enum_framesizes,
	.enum_frameintervals = ov7251_enum_frameintervals,
	.enum_mbus_fmt = ov7251_enum_mbus_fmt,
	.try_mbus_fmt = ov7251_try_mbus_fmt,
	.g_mbus_fmt = ov7251_g_mbus_fmt,
	.s_mbus_fmt = ov7251_s_mbus_fmt,
	.g_frame_interval = ov7251_g_frame_interval,
};

static const struct v4l2_subdev_core_ops ov7251_core_ops = {
	.s_power = ov7251_s_power,
	.queryctrl = ov7251_queryctrl,
	.g_ctrl = ov7251_g_ctrl,
	.s_ctrl = ov7251_s_ctrl,
	.ioctl = ov7251_ioctl,
};

static const struct v4l2_subdev_pad_ops ov7251_pad_ops = {
	.enum_mbus_code = ov7251_enum_mbus_code,
	.enum_frame_size = ov7251_enum_frame_size,
	.get_fmt = ov7251_get_pad_format,
	.set_fmt = ov7251_set_pad_format,
};

static const struct v4l2_subdev_ops ov7251_ops = {
	.core = &ov7251_core_ops,
	.video = &ov7251_video_ops,
	.pad = &ov7251_pad_ops,
	.sensor = &ov7251_sensor_ops,
};

static int ov7251_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	dev_dbg(&client->dev, "ov7251_remove...\n");

	if (dev->platform_data->platform_deinit)
		dev->platform_data->platform_deinit();

	dev->platform_data->csi_cfg(sd, 0);
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	v4l2_device_unregister_subdev(sd);
	media_entity_cleanup(&dev->sd.entity);
	kfree(dev);

	return 0;
}

static int __ov7251_init_ctrl_handler(struct ov7251_device *dev)
{
	struct v4l2_ctrl_handler *hdl = &dev->ctrl_handler;

	v4l2_ctrl_handler_init(&dev->ctrl_handler, 3);

	dev->link_freq = v4l2_ctrl_new_custom(&dev->ctrl_handler,
					      &v4l2_ctrl_link_freq,
					      NULL);

	if (dev->ctrl_handler.error || dev->link_freq == NULL)
		return dev->ctrl_handler.error;

	dev->sd.ctrl_handler = hdl;

	return 0;
}

static int ov7251_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct ov7251_device *dev;
	void *pdev;
	int ret;

	dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		dev_err(&client->dev, "out of memory\n");
		return -ENOMEM;
	}

	mutex_init(&dev->input_lock);

	dev->fmt_idx = 0;
	v4l2_i2c_subdev_init(&dev->sd, client, &ov7251_ops);

	pdev = client->dev.platform_data;
#ifdef CONFIG_INTEL_MID_ISP
	if (ACPI_COMPANION(&client->dev))
		pdev = gmin_camera_platform_data(&dev->sd,
						   ATOMISP_INPUT_FORMAT_RAW_10,
						   atomisp_bayer_order_bggr);
#endif
	ret = ov7251_s_config(&dev->sd, client->irq, pdev);
	if (ret)
		goto out;

	ret = __ov7251_init_ctrl_handler(dev);
	if (ret)
		goto out;

	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	dev->format.code = V4L2_MBUS_FMT_SBGGR10_1X10;
	dev->sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV_SENSOR;

	ret = media_entity_init(&dev->sd.entity, 1, &dev->pad, 0);
	if (ret)
		goto out_ctrl_hdl_free;

#ifdef CONFIG_INTEL_MID_ISP
	if (ACPI_HANDLE(&client->dev)) {
		ret = atomisp_register_i2c_module(&dev->sd, pdev, RAW_CAMERA);
		if (ret)
			goto out_entity_free;
	}
#endif

	return ret;

out_entity_free:
	media_entity_cleanup(&dev->sd.entity);
out_ctrl_hdl_free:
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
out:
	return ret;
}

static const struct i2c_device_id ov7251_id[] = {
	{"INT35AA", 0},
	{"INT35AA:00", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, ov7251_id);

static struct acpi_device_id ov7251_acpi_match[] = {
	{ "INT35AA" },
	{ "INT35AA:00" },
	{},
};
MODULE_DEVICE_TABLE(acpi, ov7251_acpi_match);

static struct i2c_driver ov7251_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = OV7251_NAME,
		.acpi_match_table = ACPI_PTR(ov7251_acpi_match),
	},
	.probe = ov7251_probe,
	.remove = ov7251_remove,
	.id_table = ov7251_id,
};

static __init int init_ov7251_mod(void)
{
	return i2c_add_driver(&ov7251_driver);
}

static __exit void exit_ov7251_mod(void)
{
	i2c_del_driver(&ov7251_driver);
}

module_init(init_ov7251_mod);
module_exit(exit_ov7251_mod);

MODULE_AUTHOR("Jianxu Zheng <jian.xu.zheng@intel.com>");
MODULE_DESCRIPTION("A low-level driver for OmniVision 7251 sensors");
MODULE_LICENSE("GPL");
