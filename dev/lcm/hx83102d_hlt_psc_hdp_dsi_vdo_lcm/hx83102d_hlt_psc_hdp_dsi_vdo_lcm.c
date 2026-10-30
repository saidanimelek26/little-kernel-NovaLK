/***********************************************************
** Copyright (C), 2008-2016, OPPO Mobile Comm Corp., Ltd.
** ODM_WT_EDIT
** File: hx83102d_hlt_psc_hdp_dsi_vdo_lcm.c
** Description: Full LK-compatible driver for hx83102d + HLT
****************************************************************/

#ifdef BUILD_LK
#include <platform/mt_gpio.h>
#include <platform/mt_pmic.h>
#include <platform/upmu_common.h>
#include <string.h>
#else
#include <linux/string.h>
#include <linux/kernel.h>
#endif

#include "lcm_drv.h"

/* --------------------------------------------------------------- */
#ifdef BUILD_LK
#define LCM_LOGI(fmt, args...)  dprintf(0, "[LK/LCM] " fmt, ##args)
#define LCM_LOGD(fmt, args...)  dprintf(1, "[LK/LCM] " fmt, ##args)
#else
#define LCM_LOGI(fmt, args...)  pr_info("[LCM] " fmt, ##args)
#define LCM_LOGD(fmt, args...)  pr_debug("[LCM] " fmt, ##args)
#endif

/* --------------------------------------------------------------- */
static LCM_UTIL_FUNCS lcm_util = {0};

#define SET_RESET_PIN(v)     (lcm_util.set_reset_pin((v)))
#define MDELAY(n)            (lcm_util.mdelay(n))
#define UDELAY(n)            (lcm_util.udelay(n))

/* Power pins – only call if the function pointer exists */
#define SET_LCM_VSP_PIN(v)   do { if (lcm_util.set_gpio_lcd_enp_bias) lcm_util.set_gpio_lcd_enp_bias(v); } while(0)
#define SET_LCM_VSN_PIN(v)   do { if (lcm_util.set_gpio_lcd_enn_bias) lcm_util.set_gpio_lcd_enn_bias(v); } while(0)

#define dsi_set_cmdq_V2(cmd, count, ppara, force_update) \
		lcm_util.dsi_set_cmdq_V2(cmd, count, ppara, force_update)
#define dsi_set_cmdq(pdata, queue_size, force_update) \
		lcm_util.dsi_set_cmdq(pdata, queue_size, force_update)
#define wrtie_cmd(cmd)       lcm_util.dsi_write_cmd(cmd)
#define write_regs(addr, pdata, byte_nums) \
		lcm_util.dsi_write_regs(addr, pdata, byte_nums)
#define read_reg(cmd)        lcm_util.dsi_dcs_read_lcm_reg(cmd)
#define read_reg_v2(cmd, buffer, buffer_size) \
		lcm_util.dsi_dcs_read_lcm_reg_v2(cmd, buffer, buffer_size)

/* --------------------------------------------------------------- */
#define FRAME_WIDTH          (720)
#define FRAME_HEIGHT         (1600)
#define LCM_PHYSICAL_WIDTH   (67930)
#define LCM_PHYSICAL_HEIGHT  (150960)

#define REGFLAG_DELAY        0xFFFC
#define REGFLAG_UDELAY       0xFFFB
#define REGFLAG_END_OF_TABLE 0xFFFD

/* --------------------------------------------------------------- */
struct LCM_setting_table {
	unsigned int cmd;
	unsigned char count;
	unsigned char para_list[64];
};

/* ===================== Full initialization sequence ===================== */
static struct LCM_setting_table lcm_initialization_setting[] = {
	{0x51, 2, {0x00, 0x00}},
	{0x53, 1, {0x2C}},
	{0xBD, 1, {0x00}},
	{0x11, 0, {0x00}},
	{REGFLAG_DELAY, 120, {}},

	{0xB9, 3, {0x83, 0x10, 0x2D}},
	{0xB1, 11, {0x22, 0x44, 0x31, 0x31, 0x22, 0x34, 0x2F, 0x57, 0x0D, 0x0D, 0x0D}},
	{0xB2, 14, {0x00, 0x00, 0x06, 0x40, 0x00, 0x08, 0xBE, 0x37, 0x13, 0x00, 0x00, 0x01, 0x85, 0xE0}},
	{0xB4, 14, {0x58, 0x4F, 0x58, 0x4F, 0x58, 0x52, 0x58, 0x4F, 0x05, 0xFF, 0x03, 0x03, 0x00, 0xFF}},
	{0xCC, 1, {0x02}},

	{0xD3, 25, {0x00, 0x00, 0x01, 0x01, 0x00, 0x08, 0x00, 0x47, 0x00, 0x43, 0x33, 0x0B, 0x0B, 0x00, 0x00, 0x32, 0x10, 0x07, 0x00, 0x07, 0x54, 0x15, 0xA8, 0x05, 0xA8}},
	{0xD5, 44, {0x18, 0x18, 0x18, 0x18, 0x3A, 0x3A, 0x18, 0x18, 0x20, 0x21, 0x22, 0x23, 0x19, 0x19, 0x19, 0x19, 0x04, 0x05, 0x06, 0x07, 0x00, 0x01, 0x02, 0x03, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18}},
	{0xD6, 44, {0x18, 0x18, 0x19, 0x19, 0x3A, 0x3A, 0x18, 0x18, 0x23, 0x22, 0x21, 0x20, 0x18, 0x18, 0x19, 0x19, 0x03, 0x02, 0x01, 0x00, 0x07, 0x06, 0x05, 0x04, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18}},

	{0xE7, 2, {0xFF, 0x0F}},
	{0xBD, 1, {0x01}},
	{0xE7, 1, {0x01}},
	{0xBD, 1, {0x00}},

	{0xD8, 36, {0x2A, 0xAA, 0xAA, 0x00, 0x00, 0x00, 0x2A, 0xAA, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0xBD, 1, {0x01}},
	{0xD8, 36, {0x3F, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x3F, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0xBD, 1, {0x02}},
	{0xD8, 12, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0}},
	{0xBD, 1, {0x03}},
	{0xD8, 24, {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xA0, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xA0, 0xBF, 0xFF, 0xFF, 0xAA, 0xAA, 0xA0, 0xBF, 0xFF, 0xFF, 0xAA, 0xAA, 0xA0}},
	{0xBD, 1, {0x00}},

	{0xE0, 46, {0x00, 0x02, 0x07, 0x0C, 0x10, 0x15, 0x2A, 0x31, 0x3B, 0x3A, 0x5A, 0x66, 0x72, 0x89, 0x8D, 0x9D, 0xA9, 0xC1, 0xC4, 0x62, 0x6B, 0x77, 0x7F, 0x00, 0x02, 0x07, 0x0C, 0x10, 0x15, 0x2A, 0x31, 0x3B, 0x3A, 0x5A, 0x66, 0x72, 0x89, 0x8D, 0x9D, 0xA9, 0xC1, 0xC4, 0x62, 0x6B, 0x77, 0x7F}},

	{0xBA, 19, {0x71, 0x23, 0xA8, 0x93, 0xB2, 0xC0, 0xC0, 0x01, 0x10, 0x00, 0x00, 0x00, 0x0D, 0x3D, 0x82, 0x77, 0x04, 0x01, 0x00}},
	{0xBF, 7, {0xFC, 0x00, 0x04, 0x9E, 0xF6, 0x00, 0x51}},
	{0xCB, 5, {0x00, 0x13, 0x00, 0x02, 0x58}},
	{0xC0, 6, {0x32, 0x32, 0x00, 0x00, 0x19, 0x21}},

	{0xBD, 1, {0x01}},
	{0xCB, 1, {0x01}},
	{0xBD, 1, {0x02}},
	{0xB4, 8, {0x42, 0x00, 0x33, 0x00, 0x33, 0x88, 0xB3, 0x00}},
	{0xB1, 3, {0x7F, 0x03, 0xF5}},
	{0xBD, 1, {0x00}},
	{0xC1, 1, {0x00}},

	{0x29, 0, {0x00}},
	{REGFLAG_DELAY, 20, {}},
	{0x35, 1, {0x00}},
	{0xC9, 4, {0x44, 0x08, 0x54, 0x01}},
	{0xE9, 1, {0xD0}},
	{0xE4, 9, {0x0F, 0x1E, 0x50, 0x50, 0x00, 0x00, 0x05, 0x01, 0x14}},
	{0xE9, 1, {0x3F}},
	{0x5E, 2, {0x01, 0x00}},

	{REGFLAG_END_OF_TABLE, 0x00, {}}
};

static struct LCM_setting_table lcm_suspend_setting[] = {
	{0x28, 0, {}},
	{REGFLAG_DELAY, 20, {}},
	{0x10, 0, {}},
	{REGFLAG_DELAY, 80, {}},
	{REGFLAG_END_OF_TABLE, 0x00, {}}
};

static struct LCM_setting_table lcm_backlight_level_setting[] = {
	{0x51, 2, {0x0F, 0xFF}},
	{REGFLAG_END_OF_TABLE, 0x00, {}}
};

/* --------------------------------------------------------------- */
static void push_table(struct LCM_setting_table *table, unsigned int count, unsigned char force_update)
{
	unsigned int i;
	for (i = 0; i < count; i++) {
		unsigned int cmd = table[i].cmd;
		switch (cmd) {
		case REGFLAG_DELAY:
			MDELAY(table[i].count);
			break;
		case REGFLAG_UDELAY:
			UDELAY(table[i].count);
			break;
		case REGFLAG_END_OF_TABLE:
			break;
		default:
			dsi_set_cmdq_V2(cmd, table[i].count, table[i].para_list, force_update);
			break;
		}
	}
}

/* --------------------------------------------------------------- */
static void lcm_set_util_funcs(const LCM_UTIL_FUNCS *util)
{
	memcpy(&lcm_util, util, sizeof(LCM_UTIL_FUNCS));
}

static void lcm_get_params(LCM_PARAMS *params)
{
	memset(params, 0, sizeof(LCM_PARAMS));

	params->type   = LCM_TYPE_DSI;
	params->width  = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;
	params->physical_width  = LCM_PHYSICAL_WIDTH / 1000;
	params->physical_height = LCM_PHYSICAL_HEIGHT / 1000;
	params->physical_width_um  = LCM_PHYSICAL_WIDTH;
	params->physical_height_um = LCM_PHYSICAL_HEIGHT;

	params->dsi.mode = SYNC_PULSE_VDO_MODE;
	params->dsi.switch_mode = CMD_MODE;
	params->dsi.switch_mode_enable = 0;

	params->dsi.LANE_NUM = LCM_THREE_LANE;
	params->dsi.data_format.color_order = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq   = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding     = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format      = LCM_DSI_FORMAT_RGB888;
	params->dsi.packet_size = 256;
	params->dsi.PS = LCM_PACKED_PS_24BIT_RGB888;

	params->dsi.vertical_sync_active   = 2;
	params->dsi.vertical_backporch     = 16;
	params->dsi.vertical_frontporch    = 218;
	params->dsi.vertical_active_line   = FRAME_HEIGHT;

	params->dsi.horizontal_sync_active = 16;
	params->dsi.horizontal_backporch   = 30;
	params->dsi.horizontal_frontporch  = 18;
	params->dsi.horizontal_active_pixel = FRAME_WIDTH;

	params->dsi.ssc_disable = 1;
	params->dsi.CLK_HS_PRPR = 7;

	/* Use the value that matches your bootargs fps */
	params->dsi.PLL_CLOCK = 360;

	params->dsi.esd_check_enable = 1;
	params->dsi.customization_esd_check_enable = 1;
	params->dsi.lcm_esd_check_table[0].cmd = 0x0A;
	params->dsi.lcm_esd_check_table[0].count = 1;
	params->dsi.lcm_esd_check_table[0].para_list[0] = 0x9D;
}

static void lcm_init_power(void)
{
	LCM_LOGI("lcm_init_power\n");
	MDELAY(1);
	SET_LCM_VSP_PIN(1);
	MDELAY(3);
	SET_LCM_VSN_PIN(1);
	MDELAY(5);
}

static void lcm_suspend_power(void)
{
	LCM_LOGI("lcm_suspend_power\n");
	SET_LCM_VSN_PIN(0);
	MDELAY(2);
	SET_LCM_VSP_PIN(0);
}

static void lcm_resume_power(void)
{
	LCM_LOGI("lcm_resume_power\n");
	SET_LCM_VSP_PIN(1);
	MDELAY(3);
	SET_LCM_VSN_PIN(1);
	MDELAY(5);
}

static void lcm_init(void)
{
	LCM_LOGI("lcm_init +++\n");

	SET_RESET_PIN(0);
	MDELAY(5);
	SET_RESET_PIN(1);
	MDELAY(20);

	push_table(lcm_initialization_setting,
		   sizeof(lcm_initialization_setting) / sizeof(struct LCM_setting_table), 1);

	LCM_LOGI("lcm_init ---\n");
}

static void lcm_suspend(void)
{
	LCM_LOGI("lcm_suspend\n");
	push_table(lcm_suspend_setting,
		   sizeof(lcm_suspend_setting) / sizeof(struct LCM_setting_table), 1);
	SET_RESET_PIN(0);
	MDELAY(10);
}

static void lcm_resume(void)
{
	LCM_LOGI("lcm_resume\n");
	lcm_init();
}

static void lcm_setbacklight(unsigned int level)
{
	unsigned int val;

	if (level > 255)
		level = 255;

	/* Map 0~255 → 0~4095 (12-bit) */
	val = (level * 4095) / 255;

	lcm_backlight_level_setting[0].para_list[0] = (val >> 8) & 0x0F;
	lcm_backlight_level_setting[0].para_list[1] = val & 0xFF;

	LCM_LOGI("setbacklight level=%d → 0x%02X 0x%02X\n",
		 level,
		 lcm_backlight_level_setting[0].para_list[0],
		 lcm_backlight_level_setting[0].para_list[1]);

	push_table(lcm_backlight_level_setting,
		   sizeof(lcm_backlight_level_setting) / sizeof(struct LCM_setting_table), 1);
}

static unsigned int lcm_esd_check(void)
{
	return FALSE;
}

static unsigned int lcm_esd_recover(void)
{
	LCM_LOGI("lcm_esd_recover\n");
	lcm_init_power();
	lcm_init();
	return FALSE;
}

/* --------------------------------------------------------------- */
LCM_DRIVER hx83102d_hlt_psc_hdp_dsi_vdo_lcm_drv = {
	.name            = "hx83102d_hlt_psc_hdp_dsi_vdo_lcm",
	.set_util_funcs  = lcm_set_util_funcs,
	.get_params      = lcm_get_params,
	.init            = lcm_init,
	.suspend         = lcm_suspend,
	.resume          = lcm_resume,
	.init_power      = lcm_init_power,
	.resume_power    = lcm_resume_power,
	.suspend_power   = lcm_suspend_power,
	.set_backlight   = lcm_setbacklight,
	.esd_check       = lcm_esd_check,
	.esd_recover     = lcm_esd_recover,
};
