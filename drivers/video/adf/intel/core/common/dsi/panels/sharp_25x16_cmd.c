/**************************************************************************
 * Copyright (c) 2007, Intel Corporation.
 * All Rights Reserved.
 * Copyright (c) 2008, Tungsten Graphics, Inc. Cedar Park, TX., USA.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 **************************************************************************/
#include <asm/intel_scu_pmic.h>
#include <asm/intel_mid_rpmsg.h>
#include <asm/intel_mid_remoteproc.h>
#include <linux/gpio.h>

#include "intel_adf_device.h"
#include "core/common/dsi/dsi_panel.h"
#include "core/common/dsi/dsi_pkg_sender.h"
#include "core/common/dsi/dsi_config.h"
#include "core/common/dsi/dsi_pipe.h"

#define SHARP_25x16_CMD_PHY_WIDTH	217
#define SHARP_25x16_CMD_PHY_HEIGHT	136
#define SHARP_25x16_CMD_BPP		24
#define SHARP_25x16_CMD_DSI_TYPE	DSI_DBI
#define SHARP_25x16_CMD_LANE_NUM	4

static int mipi_reset_gpio;

static u8 sharp_mode_set_data[7][3] = {
			{0x10, 0x00, 0x2f},
			{0x10, 0x01, 0x01},
			{0x10, 0x07, 0x00},
			{0x70, 0x00, 0x70},
			{0x00, 0x1f, 0x00},
			{0x20, 0x2e, 0x12},
			{0x20, 0x2a, 0x0}
};
static u8 sharp_clumn_addr_left[] = {0x2a, 0x00, 0x00, 0x09, 0xff};
static u8 sharp_page_addr_left[] = {0x2b, 0x00, 0x00, 0x06, 0x3f};
static u8 sharp_set_brightness[3] = {0x20, 0x2a, 0x0};

static int sharp25x16_cmd_drv_ic_init(struct dsi_pipe *pipe)
{
	int err = 0;
	int i;
	struct dsi_pkg_sender *sender = &pipe->sender;

	pr_debug("\n");

	if (!sender) {
		pr_err("Cannot get sender\n");
		return -EINVAL;
	}

	for (i = 0; i < 7; i++) {
		err = dsi_send_gen_long_hs(sender, sharp_mode_set_data[i],
				3,
				DSI_SEND_PACKAGE);
		if (err) {
			pr_err("%s: %d: Set Mode data\n", __func__, __LINE__);
			goto ic_init_err;
		}
		REG_WRITE(MIPIA_HS_GEN_CTRL_REG, 5);
	}

	err = dsi_send_mcs_long_hs(sender,
			sharp_clumn_addr_left,
			5, DSI_SEND_PACKAGE);
	if (err) {
		pr_err("%s: %d: Set Clumn Address\n", __func__, __LINE__);
		goto ic_init_err;
	}

	REG_WRITE(MIPIA_HS_GEN_CTRL_REG, 5);
	err = dsi_send_mcs_long_hs(sender,
			sharp_page_addr_left,
			5, DSI_SEND_PACKAGE);
	if (err) {
		pr_err("%s: %d: Set Page Address\n", __func__, __LINE__);
		goto ic_init_err;
	}
	REG_WRITE(MIPIA_HS_GEN_CTRL_REG, 5);

	err = dsi_send_mcs_short_hs(sender,
			set_tear_on, 0x00, 1,
			DSI_SEND_PACKAGE);
	if (err) {
		pr_err("%s: %d: Set Tear On\n",
		__func__, __LINE__);
		goto ic_init_err;
	}
	return 0;

ic_init_err:
	err = -EIO;
	return err;
}

static void sharp25x16_cmd_controller_init(struct dsi_pipe *pipe)
{
	struct dsi_context *hw_ctx = &pipe->config.ctx;

	pr_debug("\n");

	/*reconfig lane configuration*/
	pipe->config.lane_count = 4;
	pipe->config.lane_config = DSI_DATA_LANE_4_0;
	hw_ctx->cck_div = 1;
	hw_ctx->pll_bypass_mode = 0;

	hw_ctx->mipi_control = 0x0;
	hw_ctx->intr_en = 0xFFFFFFFF;
	hw_ctx->hs_tx_timeout = 0xFFFFFF;
	hw_ctx->lp_rx_timeout = 0xFFFFFF;
	hw_ctx->device_reset_timer = 0xffff;
	hw_ctx->turn_around_timeout = 0x3f;
	hw_ctx->high_low_switch_count = 0x40;
	hw_ctx->clk_lane_switch_time_cnt = 0x16002d;
	hw_ctx->lp_byteclk = 0x5;
	hw_ctx->dphy_param = 0x3c1fc51f;
	hw_ctx->eot_disable = 0x0;
	hw_ctx->init_count = 0xfa0;
	hw_ctx->dbi_bw_ctrl = 1024;
	hw_ctx->hs_ls_dbi_enable = 0x0;
	hw_ctx->dsi_func_prg = DBI_16BIT_IF_OPTION_2 | pipe->config.lane_count;
	hw_ctx->mipi = SEL_FLOPPED_HSTX	| LP_OUTPUT_HOLD_ENABLE |
		DUAL_LINK_ENABLE | DUAL_LINK_CAPABLE | TE_TRIGGER_BY_GPIO |
		DUAL_LINK_MODE_PIXEL_ALTER;
	hw_ctx->video_mode_format = DISABLE_VIDEO_BTA | IP_TG_CONFIG |
				    BURST_MODE;
}

static int sharp25x16_cmd_panel_connection_detect(struct dsi_pipe *pipe)
{
	int status;
	int idx = pipe->base.base.idx;

	pr_debug("\n");

	if (idx == 0)
		status = DSI_PANEL_CONNECTED;
	else {
		pr_info("%s: do NOT support dual panel\n", __func__);
		status = DSI_PANEL_DISCONNECTED;
	}

	return status;
}

static int sharp25x16_cmd_power_on(struct dsi_pipe *pipe)
{
	int err = 0;
	struct dsi_pkg_sender *sender = &pipe->sender;

	pr_debug("\n");

	if (!sender) {
		pr_err("Failed to get DSI packet sender\n");
		return -EINVAL;
	}

	msleep(150);
	err = dsi_send_mcs_short_hs(sender,
		set_address_mode, 0x0, 1,
		DSI_SEND_PACKAGE);
	if (err) {
		pr_err("%s: %d: Set Address Mode\n", __func__, __LINE__);
		goto power_err;
	}

	err = dsi_send_mcs_short_hs(sender,
			set_pixel_format, 0x77, 1,
			DSI_SEND_PACKAGE);
	if (err) {
		pr_err("%s: %d: Set Pixel format\n", __func__, __LINE__);
		goto power_err;
	}

	/* Sleep Out */
	err = dsi_send_mcs_short_hs(sender, exit_sleep_mode, 0, 0,
			DSI_SEND_PACKAGE);
	if (err) {
		pr_err("%s: %d: Exit Sleep Mode\n", __func__, __LINE__);
		goto power_err;
	}

	msleep(20);
	/* Set Display on 0x29 */
	err = dsi_send_mcs_short_hs(sender, set_display_on, 0, 0,
			DSI_SEND_PACKAGE);
	if (err) {
		pr_err("%s: %d: Set Display On\n", __func__, __LINE__);
		goto power_err;
	}
	msleep(150);

power_err:
	return err;
}

static int sharp25x16_cmd_power_off(struct dsi_pipe *pipe)
{
	int err;
	struct dsi_pkg_sender *sender = &pipe->sender;

	pr_debug("\n");

	msleep(100);
	if (!sender) {
		pr_err("Failed to get DSI packet sender\n");
		return -EINVAL;
	}

	err = dsi_send_mcs_short_hs(sender,
			set_display_off, 0, 0,
			DSI_SEND_PACKAGE);
	if (err) {
		pr_err("%s: %d: Set Display Off\n", __func__, __LINE__);
		goto power_off_err;
	}

	err = dsi_send_mcs_short_hs(sender,
			enter_sleep_mode, 0, 0,
			DSI_SEND_PACKAGE);
	if (err) {
		pr_err("%s: %d: Enter Sleep Mode\n", __func__, __LINE__);
		goto power_off_err;
	}
	/* enable it after AOB re-work
	* gpio_set_value_cansleep(mipi_reset_gpio, 0);
	*/
	msleep(100);
	return 0;
power_off_err:
	err = -EIO;
	return err;
}

static int sharp25x16_cmd_set_brightness(struct dsi_pipe *pipe, int level)
{
	u8 duty_val = 0;
	struct dsi_pkg_sender *sender = &pipe->sender;

	pr_debug("level = %d\n", level);

	if (!sender) {
		pr_err("Failed to get DSI packet sender\n");
		return -EINVAL;
	}

	duty_val = (0xFF * level) / 100;
	if (duty_val < 12)
		duty_val = 0;
	sharp_set_brightness[2] = duty_val;
	dsi_send_gen_long_hs(sender, sharp_set_brightness,
				3,
				DSI_SEND_PACKAGE);

	REG_WRITE(MIPIA_HS_GEN_CTRL_REG, 5);
	return 0;
}

static int sharp25x16_cmd_panel_reset(struct dsi_pipe *pipe)
{
	int ret = 0;

	pr_debug("\n");

	usleep_range(10000, 10500);
	if (mipi_reset_gpio == 0) {
		ret = get_gpio_by_name("disp0_rst");
		if (ret < 0) {
			pr_err("Faild to get panel reset gpio, use default reset pin\n");
			return 0;
		}
		mipi_reset_gpio = ret;
		ret = gpio_request(mipi_reset_gpio, "mipi_display");
		if (ret) {
			pr_err("Faild to request panel reset gpio\n");
			return 0;
		}
	}
	gpio_direction_output(mipi_reset_gpio, 0);
	usleep_range(1000, 1500);
	gpio_set_value_cansleep(mipi_reset_gpio, 1);

	return ret;
}

static int sharp25x16_cmd_exit_deep_standby(struct dsi_pipe *pipe)
{
	pr_debug("\n");

	gpio_set_value_cansleep(mipi_reset_gpio, 0);
	usleep_range(1000, 1500);
	gpio_set_value_cansleep(mipi_reset_gpio, 1);

	return 0;
}

static int sharp25x16_cmd_get_config_mode(struct drm_mode_modeinfo *mode)
{
	pr_debug("\n");

	if (!mode)
		return -EINVAL;

	mode->hdisplay = 2560;

	mode->hsync_start = mode->hdisplay + 48;
	mode->hsync_end = mode->hsync_start + 32;
	mode->htotal = mode->hsync_end + 80;

	mode->vdisplay = 1600;
	mode->vsync_start = mode->vdisplay + 3;
	mode->vsync_end = mode->vsync_start + 33;
	mode->vtotal = mode->vsync_end + 10;

	mode->vrefresh = 60;
	mode->clock = mode->vrefresh * mode->vtotal * mode->htotal / 1000;

	adf_modeinfo_set_name(mode);

	return 0;
}

static void sharp25x16_cmd_get_panel_info(struct panel_info *pi)
{
	pr_debug("\n");

	pi->width_mm = SHARP_25x16_CMD_PHY_WIDTH;
	pi->height_mm = SHARP_25x16_CMD_PHY_HEIGHT;
	pi->bpp = SHARP_25x16_CMD_BPP;
	pi->dsi_type = SHARP_25x16_CMD_DSI_TYPE;
	pi->lane_num = SHARP_25x16_CMD_LANE_NUM;
	pi->dual_link = DSI_PANEL_DUAL_LINK;
}

static struct panel_ops sharp25x16_cmd_panel_ops = {
	.drv_ic_init = sharp25x16_cmd_drv_ic_init,
	.dsi_controller_init = sharp25x16_cmd_controller_init,
	.detect = sharp25x16_cmd_panel_connection_detect,
	.power_on = sharp25x16_cmd_power_on,
	.power_off = sharp25x16_cmd_power_off,
	.set_brightness = sharp25x16_cmd_set_brightness,
	.reset = sharp25x16_cmd_panel_reset,
	.get_config_mode = sharp25x16_cmd_get_config_mode,
	.exit_deep_standby = sharp25x16_cmd_exit_deep_standby,
	.get_panel_info = sharp25x16_cmd_get_panel_info,
};

struct dsi_panel sharp25x16_cmd_panel = {
	.panel_id = SHARP_25x16_CMD,
	.info.width_mm = SHARP_25x16_CMD_PHY_WIDTH,
	.info.height_mm = SHARP_25x16_CMD_PHY_HEIGHT,
	.info.bpp = SHARP_25x16_CMD_BPP,
	.info.dsi_type = SHARP_25x16_CMD_DSI_TYPE,
	.info.lane_num = SHARP_25x16_CMD_LANE_NUM,
	.info.dual_link = DSI_PANEL_DUAL_LINK,
	.ops = &sharp25x16_cmd_panel_ops,
};

const struct dsi_panel *sharp_25x16_cmd_get_panel(void)
{
	return &sharp25x16_cmd_panel;
}
