/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * A V4L2 driver for IMX214  Raw cameras.
 *
 * Copyright (c) 2017 by Allwinnertech Co., Ltd.  http://www.allwinnertech.com
 *
 * Authors:  Zhao Wei <zhaowei@allwinnertech.com>
 *    Yang Feng <yangfeng@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "../../utility/vin_log.h"
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/videodev2.h>
#include <linux/clk.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mediabus.h>
#include <linux/io.h>
#include "camera.h"
#include "sensor_helper.h"

MODULE_AUTHOR("lwj");
MODULE_DESCRIPTION("A733 IMX214 isolated 1080p60 experiment");
MODULE_VERSION("0.2.0");
MODULE_LICENSE("GPL");

#define MCLK              (24*1000*1000)
#define V4L2_IDENT_SENSOR 0x0214

#define DGAIN_R  0x100
#define DGAIN_G  0x100
#define DGAIN_B  0x100

/*
 * Our nominal (default) frame rate.
 */

#define SENSOR_FRAME_RATE 30

/*
 * The IMX214 sits on i2c with ID 0x6c
 */
#define I2C_ADDR 0x20
#define SENSOR_NAME "imx214"
static int imx214_sensor_vts;
static unsigned int receiver_deskew = 5;
module_param(receiver_deskew, uint, 0444);
MODULE_PARM_DESC(receiver_deskew,
		 "1080p60 MIPI1 receiver clock delay, 1..31; 30fps keeps7");

struct imx214_state {
	struct sensor_info info;
	bool streaming;
};

static struct imx214_state *imx214_state(struct v4l2_subdev *sd)
{
	return container_of(to_state(sd), struct imx214_state, info);
}

/* Explicitly bounded, checked writes; avoid shared array coalescing overrun. */
static int imx214_write_table(struct v4l2_subdev *sd,
			    const struct regval_list *regs, unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; ++i) {
		if (regs[i].addr == REG_DLY) {
			usleep_range(regs[i].data * 1000, regs[i].data * 1000 + 100);
			continue;
		}
		/* Stream-on is always issued explicitly after all mode settings. */
		if (regs[i].addr == 0x0100 && regs[i].data == 1)
			continue;
		ret = sensor_write(sd, regs[i].addr, regs[i].data);
		if (ret)
			return ret;
	}
	return 0;
}

static int imx214_read16(struct v4l2_subdev *sd, u16 addr, unsigned int *value)
{
	data_type hi, lo;
	int ret = sensor_read(sd, addr, &hi);

	if (ret)
		return ret;
	ret = sensor_read(sd, addr + 1, &lo);
	if (!ret)
		*value = ((unsigned int)hi << 8) | lo;
	return ret;
}

static int imx214_snapshot(struct v4l2_subdev *sd, const char *phase)
{
	static const u16 regs[] = {
		0x0100, 0x0114, 0x0112, 0x0113, 0x0136, 0x0137,
		0x0301, 0x0303, 0x0305, 0x0306, 0x0307, 0x0309, 0x030b,
		0x0310, 0x0820, 0x0821, 0x0822, 0x0823,
		0x0340, 0x0341, 0x0342, 0x0343, 0x0344, 0x0345,
		0x0346, 0x0347, 0x0348, 0x0349, 0x034a, 0x034b,
		0x034c, 0x034d, 0x034e, 0x034f, 0x0900, 0x0901, 0x0902,
		0x0202, 0x0203, 0x0204, 0x0205, 0x3a03, 0x3a04, 0x3a05,
		0x0808, 0x080b, 0x080d, 0x080f, 0x0811, 0x0813,
		0x0815, 0x0817, 0x0819,
	};
	unsigned int i;
	data_type value;
	int ret;

	for (i = 0; i < ARRAY_SIZE(regs); ++i) {
		ret = sensor_read(sd, regs[i], &value);
		if (ret)
			return ret;
		pr_info("imx214-timing %s reg=%04x value=%02x\n", phase, regs[i], value);
	}
	return 0;
}

struct cfg_array {		/* coming later */
	struct regval_list *regs;
	int size;
};



/*
 * The default register settings
 *
 */

static struct regval_list sensor_default_regs[] = {
	{0x0101, 0x03},
	{0x0105, 0x01},
	{0x0106, 0x01},
	{0x4550, 0x02},
	{0x4601, 0x04},
	{0x4642, 0x01},
	{0x6227, 0x11},
	{0x6276, 0x00},
	{0x900E, 0x06},
	{0xA802, 0x90},
	{0xA803, 0x11},
	{0xA804, 0x62},
	{0xA805, 0x77},
	{0xA806, 0xAE},
	{0xA807, 0x34},
	{0xA808, 0xAE},
	{0xA809, 0x35},
	{0xA80A, 0x62},
	{0xA80B, 0x83},
	{0xAE33, 0x00},
	{0x4174, 0x00},
	{0x4175, 0x11},
	{0x4612, 0x29},
	{0x461B, 0x2C},
	{0x461F, 0x06},
	{0x4635, 0x07},
	{0x4637, 0x30},
	{0x463F, 0x18},
	{0x4641, 0x0D},
	{0x465B, 0x2C},
	{0x465F, 0x2B},
	{0x4663, 0x2B},
	{0x4667, 0x24},
	{0x466F, 0x24},
	{0x470E, 0x09},
	{0x4909, 0xAB},
	{0x490B, 0x95},
	{0x4915, 0x5D},
	{0x4A5F, 0xFF},
	{0x4A61, 0xFF},
	{0x4A73, 0x62},
	{0x4A85, 0x00},
	{0x4A87, 0xFF},
	{0x583C, 0x04},
	{0x620E, 0x04},
	{0x6EB2, 0x01},
	{0x6EB3, 0x00},
	{0x9300, 0x02},
	{0x080b, 119},
	{0x080d, 55},
	{0x080f, 103},
	{0x0811, 55},
	{0x0813, 55},
	{0x0815, 55},
	{0x0817, 223},
	{0x0819, 47},
	{0x4601, 0x04},
	{0x4642, 0x01},
	{0x461B, 0x2C},
	{0x465B, 0x2C},
	{0x465F, 0x2B},
	{0x4663, 0x2B},
	{0x4667, 0x24},
	{0x466F, 0x24},
};


static struct regval_list sensor_13mega_regs[] = {
	{0x0114, 0x03},
	{0x0220, 0x00},
	{0x0221, 0x11},
	{0x0222, 0x01},
	{0x0340, 0x0C},
	{0x0341, 0x7A},
	{0x0342, 0x13},
	{0x0343, 0x90},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x10},
	{0x0349, 0x6F},
	{0x034A, 0x0C},
	{0x034B, 0x2F},
	{0x0381, 0x01},
	{0x0383, 0x01},
	{0x0385, 0x01},
	{0x0387, 0x01},
	{0x0900, 0x00},
	{0x0901, 0x00},
	{0x0902, 0x00},
	{0x3000, 0x35},
	{0x3054, 0x01},
	{0x305C, 0x11},
	{0x0112, 0x0A},
	{0x0113, 0x0A},
	{0x034C, 0x10},
	{0x034D, 0x70},
	{0x034E, 0x0C},
	{0x034F, 0x30},
	{0x0401, 0x00},
	{0x0404, 0x00},
	{0x0405, 0x10},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040A, 0x00},
	{0x040B, 0x00},
	{0x040C, 0x10},
	{0x040D, 0x70},
	{0x040E, 0x0C},
	{0x040F, 0x30},
	{0x0301, 0x05},
	{0x0303, 0x02},
	{0x0305, 0x03},
	{0x0306, 0x00},
	{0x0307, 0x64},
	{0x0309, 0x0A},
	{0x030B, 0x01},
	{0x0310, 0x00},
	{0x0820, 0x0C},
	{0x0821, 0x80},
	{0x0822, 0x00},
	{0x0823, 0x00},
	{0x3A03, 0x08},
	{0x3A04, 0xD0},
	{0x3A05, 0x02},
	{0x0B06, 0x01},
	{0x30A2, 0x00},
	{0x30B4, 0x00},
	{0x3A02, 0xFF},
	{0x3011, 0x00},
	{0x3013, 0x01},
	{0x0202, 0x0C},
	{0x0203, 0x70},
	{0x0224, 0x01},
	{0x0225, 0xF4},
	{0x0204, 0x00},
	{0x0205, 0x00},
	{0x020E, 0x01},
	{0x020F, 0x00},
	{0x0210, 0x01},
	{0x0211, 0x00},
	{0x0212, 0x01},
	{0x0213, 0x00},
	{0x0214, 0x01},
	{0x0215, 0x00},
	{0x0216, 0x00},
	{0x0217, 0x00},
	{0x4170, 0x00},
	{0x4171, 0x10},
	{0x4176, 0x00},
	{0x4177, 0x3C},
	{0xAE20, 0x04},
	{0xAE21, 0x5C},
	{0x0138, 0x01},
	{0x0100, 0x01},
};

static struct regval_list sensor_13mega_15fps_regs[] = {
	{0x0114, 0x03},
	{0x0220, 0x00},
	{0x0221, 0x11},
	{0x0222, 0x01},
	{0x0340, 0x18},
	{0x0341, 0xF4},
	{0x0342, 0x13},
	{0x0343, 0x90},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x10},
	{0x0349, 0x6F},
	{0x034A, 0x0C},
	{0x034B, 0x2F},
	{0x0381, 0x01},
	{0x0383, 0x01},
	{0x0385, 0x01},
	{0x0387, 0x01},
	{0x0900, 0x00},
	{0x0901, 0x00},
	{0x0902, 0x00},
	{0x3000, 0x35},
	{0x3054, 0x01},
	{0x305C, 0x11},
	{0x0112, 0x0A},
	{0x0113, 0x0A},
	{0x034C, 0x10},
	{0x034D, 0x70},
	{0x034E, 0x0C},
	{0x034F, 0x30},
	{0x0401, 0x00},
	{0x0404, 0x00},
	{0x0405, 0x10},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040A, 0x00},
	{0x040B, 0x00},
	{0x040C, 0x10},
	{0x040D, 0x70},
	{0x040E, 0x0C},
	{0x040F, 0x30},
	{0x0301, 0x05},
	{0x0303, 0x02},
	{0x0305, 0x03},
	{0x0306, 0x00},
	{0x0307, 0x64},
	{0x0309, 0x0A},
	{0x030B, 0x01},
	{0x0310, 0x00},
	{0x0820, 0x0C},
	{0x0821, 0x80},
	{0x0822, 0x00},
	{0x0823, 0x00},
	{0x3A03, 0x08},
	{0x3A04, 0xD0},
	{0x3A05, 0x02},
	{0x0B06, 0x01},
	{0x30A2, 0x00},
	{0x30B4, 0x00},
	{0x3A02, 0xFF},
	{0x3011, 0x00},
	{0x3013, 0x01},
	{0x0202, 0x0C},
	{0x0203, 0x70},
	{0x0224, 0x01},
	{0x0225, 0xF4},
	{0x0204, 0x00},
	{0x0205, 0x00},
	{0x020E, 0x01},
	{0x020F, 0x00},
	{0x0210, 0x01},
	{0x0211, 0x00},
	{0x0212, 0x01},
	{0x0213, 0x00},
	{0x0214, 0x01},
	{0x0215, 0x00},
	{0x0216, 0x00},
	{0x0217, 0x00},
	{0x4170, 0x00},
	{0x4171, 0x10},
	{0x4176, 0x00},
	{0x4177, 0x3C},
	{0xAE20, 0x04},
	{0xAE21, 0x5C},
	{0x0138, 0x01},
	{0x0100, 0x01},
};


static struct regval_list sensor_4k_videos[] = {
	{0x0114, 0x03},
	{0x0220, 0x00},
	{0x0221, 0x11},
	{0x0222, 0x01},
	{0x0340, 0x0C},
	{0x0341, 0x3A},
	{0x0342, 0x13},
	{0x0343, 0x90},
	{0x0344, 0x00},
	{0x0345, 0xB8},
	{0x0346, 0x01},
	{0x0347, 0xE0},
	{0x0348, 0x0F},
	{0x0349, 0xB7},
	{0x034A, 0x0A},
	{0x034B, 0x4F},
	{0x0381, 0x01},
	{0x0383, 0x01},
	{0x0385, 0x01},
	{0x0387, 0x01},
	{0x0900, 0x00},
	{0x0901, 0x00},
	{0x0902, 0x00},
	{0x3000, 0x35},
	{0x3054, 0x01},
	{0x305C, 0x11},
	{0x0112, 0x0A},
	{0x0113, 0x0A},
	{0x034C, 0x0F},
	{0x034D, 0x00},
	{0x034E, 0x08},
	{0x034F, 0x70},
	{0x0401, 0x00},
	{0x0404, 0x00},
	{0x0405, 0x10},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040A, 0x00},
	{0x040B, 0x00},
	{0x040C, 0x0F},
	{0x040D, 0x00},
	{0x040E, 0x08},
	{0x040F, 0x70},
	{0x0301, 0x05},
	{0x0303, 0x02},
	{0x0305, 0x03},
	{0x0306, 0x00},
	{0x0307, 0x64},
	{0x0309, 0x0A},
	{0x030B, 0x01},
	{0x0310, 0x00},
	{0x0820, 0x0C},
	{0x0821, 0x80},
	{0x0822, 0x00},
	{0x0823, 0x00},
	{0x3A03, 0x09},
	{0x3A04, 0xA0},
	{0x3A05, 0x04},
	{0x0B06, 0x01},
	{0x30A2, 0x00},
	{0x30B4, 0x00},
	{0x3A02, 0xFF},
	{0x3011, 0x00},
	{0x3013, 0x01},
	{0x0202, 0x09},
	{0x0203, 0x32},
	{0x0224, 0x01},
	{0x0225, 0xF4},
	{0x0204, 0x00},
	{0x0205, 0x00},
	{0x020E, 0x01},
	{0x020F, 0x00},
	{0x0210, 0x01},
	{0x0211, 0x00},
	{0x0212, 0x01},
	{0x0213, 0x00},
	{0x0214, 0x01},
	{0x0215, 0x00},
	{0x0216, 0x00},
	{0x0217, 0x00},
	{0x4170, 0x00},
	{0x4171, 0x10},
	{0x4176, 0x00},
	{0x4177, 0x3C},
	{0xAE20, 0x04},
	{0xAE21, 0x5C},
	{0x0138, 0x01},
	{0x0100, 0x01},
};

static struct regval_list sensor_4k_20fps_regs[] = {
	{0x0114, 0x03},
	{0x0220, 0x00},
	{0x0221, 0x11},
	{0x0222, 0x01},
	{0x0340, 0x12},
	{0x0341, 0x57},
	{0x0342, 0x13},
	{0x0343, 0x90},
	{0x0344, 0x00},
	{0x0345, 0xB8},
	{0x0346, 0x01},
	{0x0347, 0xE0},
	{0x0348, 0x0F},
	{0x0349, 0xB7},
	{0x034A, 0x0A},
	{0x034B, 0x4F},
	{0x0381, 0x01},
	{0x0383, 0x01},
	{0x0385, 0x01},
	{0x0387, 0x01},
	{0x0900, 0x00},
	{0x0901, 0x00},
	{0x0902, 0x00},
	{0x3000, 0x35},
	{0x3054, 0x01},
	{0x305C, 0x11},
	{0x0112, 0x0A},
	{0x0113, 0x0A},
	{0x034C, 0x0F},
	{0x034D, 0x00},
	{0x034E, 0x08},
	{0x034F, 0x70},
	{0x0401, 0x00},
	{0x0404, 0x00},
	{0x0405, 0x10},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040A, 0x00},
	{0x040B, 0x00},
	{0x040C, 0x0F},
	{0x040D, 0x00},
	{0x040E, 0x08},
	{0x040F, 0x70},
	{0x0301, 0x05},
	{0x0303, 0x02},
	{0x0305, 0x03},
	{0x0306, 0x00},
	{0x0307, 0x64},
	{0x0309, 0x0A},
	{0x030B, 0x01},
	{0x0310, 0x00},
	{0x0820, 0x0C},
	{0x0821, 0x80},
	{0x0822, 0x00},
	{0x0823, 0x00},
	{0x3A03, 0x09},
	{0x3A04, 0xA0},
	{0x3A05, 0x04},
	{0x0B06, 0x01},
	{0x30A2, 0x00},
	{0x30B4, 0x00},
	{0x3A02, 0xFF},
	{0x3011, 0x00},
	{0x3013, 0x01},
	{0x0202, 0x09},
	{0x0203, 0x32},
	{0x0224, 0x01},
	{0x0225, 0xF4},
	{0x0204, 0x00},
	{0x0205, 0x00},
	{0x020E, 0x01},
	{0x020F, 0x00},
	{0x0210, 0x01},
	{0x0211, 0x00},
	{0x0212, 0x01},
	{0x0213, 0x00},
	{0x0214, 0x01},
	{0x0215, 0x00},
	{0x0216, 0x00},
	{0x0217, 0x00},
	{0x4170, 0x00},
	{0x4171, 0x10},
	{0x4176, 0x00},
	{0x4177, 0x3C},
	{0xAE20, 0x04},
	{0xAE21, 0x5C},
	{0x0138, 0x01},
	{0x0100, 0x01},
};

static struct regval_list sensor_1080p_regs[] = {
	{0x0114, 0x03},
	{0x0220, 0x00},
	{0x0221, 0x11},
	{0x0222, 0x01},
	{0x0340, 0x08},
	{0x0341, 0x50},
	{0x0342, 0x13},
	{0x0343, 0x90},
	{0x0344, 0x00},
	{0x0345, 0xB8},
	{0x0346, 0x01},
	{0x0347, 0xE0},
	{0x0348, 0x0F},
	{0x0349, 0xB7},
	{0x034A, 0x0A},
	{0x034B, 0x4F},
	{0x0381, 0x01},
	{0x0383, 0x01},
	{0x0385, 0x01},
	{0x0387, 0x01},
	{0x0900, 0x01},
	{0x0901, 0x22},
	{0x0902, 0x02},
	{0x3000, 0x35},
	{0x3054, 0x01},
	{0x305C, 0x11},
	{0x0112, 0x0A},
	{0x0113, 0x0A},
	{0x034C, 0x07},
	{0x034D, 0x80},
	{0x034E, 0x04},
	{0x034F, 0x38},
	{0x0401, 0x00},
	{0x0404, 0x00},
	{0x0405, 0x10},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040A, 0x00},
	{0x040B, 0x00},
	{0x040C, 0x07},
	{0x040D, 0x80},
	{0x040E, 0x04},
	{0x040F, 0x38},
	{0x0301, 0x05},
	{0x0303, 0x02},
	{0x0305, 0x03},
	{0x0306, 0x00},
	{0x0307, 0x64},
	{0x0309, 0x0A},
	{0x030B, 0x01},
	{0x0310, 0x00},
	{0x0820, 0x0C},
	{0x0821, 0x80},
	{0x0822, 0x00},
	{0x0823, 0x00},
	{0x3A03, 0x06},
	{0x3A04, 0x28},
	{0x3A05, 0x04},
	{0x0B06, 0x01},
	{0x30A2, 0x00},
	{0x30B4, 0x00},
	{0x3A02, 0xFF},
	{0x3011, 0x00},
	{0x3013, 0x01},
	{0x0202, 0x08},
	{0x0203, 0x46},
	{0x0224, 0x01},
	{0x0225, 0xF4},
	{0x0204, 0x00},
	{0x0205, 0x00},
	{0x020E, 0x01},
	{0x020F, 0x00},
	{0x0210, 0x01},
	{0x0211, 0x00},
	{0x0212, 0x01},
	{0x0213, 0x00},
	{0x0214, 0x01},
	{0x0215, 0x00},
	{0x0216, 0x00},
	{0x0217, 0x00},
	{0x4170, 0x00},
	{0x4171, 0x10},
	{0x4176, 0x00},
	{0x4177, 0x3C},
	{0xAE20, 0x04},
	{0xAE21, 0x5C},
	{0x0138, 0x01},
	{0x0100, 0x01},
};

/*
 * Here we'll try to encapsulate the changes for just the output
 * video format.
 *
 */

#include "imx214-1080p60.h"

static struct regval_list sensor_fmt_raw[] = {
};

static int sensor_g_exp(struct v4l2_subdev *sd, __s32 *value)
{
	struct sensor_info *info = to_state(sd);
	unsigned int lines, hts, vts;
	int ret;

	if (!imx214_state(sd)->streaming) {
		*value = info->exp;
		return 0;
	}
	ret = imx214_read16(sd, 0x0202, &lines);
	if (ret)
		return ret;
	info->exp = lines << 4;
	*value = info->exp;
	ret = imx214_read16(sd, 0x0342, &hts);
	if (!ret)
		ret = imx214_read16(sd, 0x0340, &vts);
	if (!ret)
		pr_info("imx214-live exposure_lines=%u hts=%u vts=%u\n", lines, hts, vts);
	return ret;
}

static int sensor_s_exp(struct v4l2_subdev *sd, unsigned int exp_val)
{
	struct sensor_info *info = to_state(sd);
	unsigned int lines = clamp_t(unsigned int, (exp_val + 8) >> 4, 1, 65525);
	int ret;

	if (info->current_wins && info->current_wins->fps_fixed == 60)
		lines = min_t(unsigned int, lines, info->current_wins->vts - 10);
	ret = sensor_write(sd, 0x0202, lines >> 8);
	if (ret)
		return ret;
	ret = sensor_write(sd, 0x0203, lines & 0xff);
	if (!ret)
		info->exp = lines << 4;
	return ret;
}

static int sensor_g_gain(struct v4l2_subdev *sd, __s32 *value)
{
	struct sensor_info *info = to_state(sd);

	*value = info->gain;
	sensor_dbg("sensor_get_gain = %d\n", info->gain);
	return 0;
}

static int sensor_s_gain(struct v4l2_subdev *sd, int gain_val)
{
	struct sensor_info *info = to_state(sd);
	unsigned char gainlow = 0;
	unsigned char gainhigh = 0;
	int gainana = 512 - 8192 / gain_val;

	gainlow = (unsigned char)(gainana & 0xff);
	gainhigh = (unsigned char)((gainana >> 8) & 0xff);

	sensor_write(sd, 0x0205, gainlow);
	sensor_write(sd, 0x0204, gainhigh);

	info->gain = gain_val;

	return 0;
}

static int sensor_s_exp_gain(struct v4l2_subdev *sd,
			     struct sensor_exp_gain *exp_gain)
{
	int exp_val, gain_val, ret;
	struct sensor_info *info = to_state(sd);

	exp_val = exp_gain->exp_val;
	gain_val = exp_gain->gain_val;

	if (gain_val < 1 * 16)
		gain_val = 16;
	if (gain_val > 64 * 16 - 1)
		gain_val = 64 * 16 - 1;

	if (exp_val > 0xfffff)
		exp_val = 0xfffff;

	ret = sensor_write(sd, 0x0104, 0x01);
	if (ret)
		return ret;
	ret = sensor_s_exp(sd, exp_val);
	if (!ret)
		ret = sensor_s_gain(sd, gain_val);
	/* Always release group hold, including when a write failed. */
	if (sensor_write(sd, 0x0104, 0x00) && !ret)
		ret = -EIO;
	if (ret)
		return ret;

	if (gain_val > 64) {
		sensor_write(sd, 0x30a2, 0x03);
		sensor_write(sd, 0x9706, (gain_val - 64) / 12);
		sensor_write(sd, 0x9e25, (gain_val - 64));
	} else {
		sensor_write(sd, 0x30a2, 0x00);
	}

	info->gain = gain_val;
	return 0;
}

/*
 * Stuff that knows about the sensor.
 */

static int sensor_power(struct v4l2_subdev *sd, int on)
{
	switch (on) {
	case STBY_ON:
		sensor_dbg("STBY_ON!\n");
		cci_lock(sd);
		vin_gpio_write(sd, RESET, CSI_GPIO_LOW);
		cci_unlock(sd);
		vin_set_mclk(sd, OFF);
		break;
	case STBY_OFF:
		sensor_dbg("STBY_OFF!\n");
		cci_lock(sd);
		vin_set_mclk_freq(sd, MCLK);
		vin_set_mclk(sd, ON);
		msleep(20);
		cci_unlock(sd);
		vin_gpio_write(sd, RESET, CSI_GPIO_HIGH);
		break;
	case PWR_ON:
		sensor_dbg("PWR_ON!\n");
		cci_lock(sd);
		vin_gpio_set_status(sd, PWDN, 1);
		vin_gpio_set_status(sd, RESET, 1);
		vin_gpio_write(sd, PWDN, CSI_GPIO_LOW);
		vin_gpio_write(sd, RESET, CSI_GPIO_LOW);
		usleep_range(1000, 1200);
		vin_set_mclk_freq(sd, MCLK);
		vin_set_mclk(sd, ON);
		usleep_range(10000, 12000);
		vin_gpio_write(sd, POWER_EN, CSI_GPIO_HIGH);
		vin_set_pmu_channel(sd, IOVDD, ON);
		vin_set_pmu_channel(sd, AVDD, ON);
		vin_set_pmu_channel(sd, DVDD, ON);
		vin_set_pmu_channel(sd, AFVDD, ON);
		usleep_range(10000, 12000);
		vin_gpio_write(sd, PWDN, CSI_GPIO_HIGH);
		vin_gpio_write(sd, RESET, CSI_GPIO_HIGH);

		usleep_range(30000, 31000);
		cci_unlock(sd);
		break;
	case PWR_OFF:
		imx214_state(sd)->streaming = false;
		sensor_dbg("PWR_OFF!\n");
		cci_lock(sd);
		vin_set_mclk(sd, OFF);
		vin_gpio_write(sd, POWER_EN, CSI_GPIO_LOW);
		vin_set_pmu_channel(sd, AFVDD, OFF);
		vin_set_pmu_channel(sd, DVDD, OFF);
		vin_set_pmu_channel(sd, AVDD, OFF);
		vin_set_pmu_channel(sd, IOVDD, OFF);
		usleep_range(10000, 12000);
		vin_gpio_write(sd, PWDN, CSI_GPIO_LOW);
		vin_gpio_write(sd, RESET, CSI_GPIO_LOW);

		vin_gpio_set_status(sd, RESET, 0);
		vin_gpio_set_status(sd, PWDN, 0);
		cci_unlock(sd);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int sensor_reset(struct v4l2_subdev *sd, u32 val)
{
	switch (val) {
	case 0:
		vin_gpio_write(sd, RESET, CSI_GPIO_HIGH);
		usleep_range(10000, 12000);
		break;
	case 1:
		vin_gpio_write(sd, RESET, CSI_GPIO_LOW);
		usleep_range(10000, 12000);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int sensor_detect(struct v4l2_subdev *sd)
{
	data_type rdval;

	sensor_read(sd, 0x0000, &rdval);
	sensor_read(sd, 0x0001, &rdval);
	sensor_print("find the sony IMX214 ***********\n");
	return 0;
}

static int sensor_init(struct v4l2_subdev *sd, u32 val)
{
	int ret;
	struct sensor_info *info = to_state(sd);

	sensor_dbg("sensor_init\n");

	/* Make sure it is a target sensor */
	ret = sensor_detect(sd);
	if (ret) {
		sensor_err("chip found is not an target chip.\n");
		return ret;
	}

	info->focus_status = 0;
	info->low_speed = 0;
	info->width = 4208;
	info->height = 3120;
	info->hflip = 0;
	info->vflip = 0;
	info->gain = 0;

	info->tpf.numerator = 1;
	info->tpf.denominator = 30;	/* 30fps */

	info->preview_first_flag = 1;

	return 0;
}

static long sensor_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	int ret = 0;
	struct sensor_info *info = to_state(sd);

	switch (cmd) {
	case GET_CURRENT_WIN_CFG:
		if (info->current_wins != NULL) {
			memcpy(arg,
			       info->current_wins,
			       sizeof(*info->current_wins));
			ret = 0;
		} else {
			sensor_err("empty wins!\n");
			ret = -1;
		}
		break;
	case SET_FPS:
		/* Use VIN S_PARM followed by S_FMT; live PLL changes are unsafe. */
		return -EOPNOTSUPP;
	case VIDIOC_VIN_SENSOR_EXP_GAIN:
		ret = sensor_s_exp_gain(sd, (struct sensor_exp_gain *)arg);
		break;
	case VIDIOC_VIN_SENSOR_CFG_REQ:
		sensor_cfg_req(sd, (struct sensor_config *)arg);
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

/*
 * Store information about the video data format.
 */
static struct sensor_format_struct sensor_formats[] = {
	{
		.desc = "Raw RGB Bayer",
		.mbus_code = MEDIA_BUS_FMT_SBGGR10_1X10,
		.regs = sensor_fmt_raw,
		.regs_size = ARRAY_SIZE(sensor_fmt_raw),
		.bpp = 1
	},
};
#define N_FMTS ARRAY_SIZE(sensor_formats)

/*
 * Then there is the issue of window sizes.  Try to capture the info here.
 */

static struct sensor_win_size sensor_win_sizes[] = {
#if 1
	/* Fullsize: 4208*3120 */
	{
	 .width = 4208,
	 .height = 3120,
	 .hoffset = 0,
	 .voffset = 0,
	 .hts = 5008,
#ifdef CONFIG_ARCH_SUN55IW3
	 .vts = 6388,
#else
	 .vts = 3194,
#endif
	 .pclk = 320 * 1000 * 1000,
	 .mipi_bps = 800 * 1000 * 1000,
#ifdef CONFIG_ARCH_SUN55IW3
	 .fps_fixed = 15,
#else
	 .fps_fixed = 30,
#endif
	 .bin_factor = 1,
	 .intg_min = 16,
#ifdef CONFIG_ARCH_SUN55IW3
	 .intg_max = (6388 - 10) << 4,
#else
	 .intg_max = (3194 - 10) << 4,
#endif
	 .gain_min = 16,
	 .gain_max = (16 << 4),
#ifdef CONFIG_ARCH_SUN55IW3
	 .regs = sensor_13mega_15fps_regs,
	 .regs_size = ARRAY_SIZE(sensor_13mega_15fps_regs),
#else
	 .regs = sensor_13mega_regs,
	 .regs_size = ARRAY_SIZE(sensor_13mega_regs),
#endif
	 .set_size = NULL,
	 },
#endif
#if 1
	/* 4k video */
	{
	 .width = 3840,
	 .height = 2160,
	 .hoffset = 0,
	 .voffset = 0,
	 .hts = 5008,
#ifdef CONFIG_ARCH_SUN55IW3
	 .vts = 4695,
#else
	 .vts = 3130,
#endif
	 .pclk = 320 * 1000 * 1000,
	 .mipi_bps = 800 * 1000 * 1000,
#ifdef CONFIG_ARCH_SUN55IW3
	 .fps_fixed = 20,
#else
	 .fps_fixed = 30,
#endif
	 .bin_factor = 1,
	 .intg_min = 16,
#ifdef CONFIG_ARCH_SUN55IW3
	 .intg_max = (4695 - 10) << 4,
#else
	 .intg_max = (3130 - 10) << 4,
#endif
	 .gain_min = 16,
	 .gain_max = (16 << 4),
#ifdef CONFIG_ARCH_SUN55IW3
	 .regs = sensor_4k_20fps_regs,
	 .regs_size = ARRAY_SIZE(sensor_4k_20fps_regs),
#else
	 .regs = sensor_4k_videos,
	 .regs_size = ARRAY_SIZE(sensor_4k_videos),
#endif
	 .set_size = NULL,
	 },

	/* 1080p */
	{
	 .width = 1920,
	 .height = 1080,
	 .hoffset = 0,
	 .voffset = 0,
	 .hts = 5008,
	 .vts = 2128,
	 .pclk = 320 * 1000 * 1000,
	 .mipi_bps = 800 * 1000 * 1000,
	 .fps_fixed = 30,
	 .bin_factor = 1,
	 .intg_min = 16,
	 .intg_max = (2128 - 10) << 4,
	 .gain_min = 16,
	 .gain_max = (16 << 4),
	 .regs = sensor_1080p_regs,
	 .regs_size = ARRAY_SIZE(sensor_1080p_regs),
	 .set_size = NULL,
	 },
#endif

	/* Nominal 60; exact timing is defined by the generated reference profile. */
	{
	 .width = 1920,
	 .height = 1080,
	 .hts = 5008,
	 .vts = IMX214_60_VTS,
	 .pclk = IMX214_60_PCLK,
	 .mipi_bps = IMX214_60_MIPI_BPS,
	 .fps_fixed = 60,
	 .bin_factor = 2,
	 .intg_min = 16,
	 .intg_max = (IMX214_60_VTS - 10) << 4,
	 .gain_min = 16,
	 .gain_max = (16 << 4),
	 .regs = (struct regval_list *)sensor_1080p60_regs,
	 .regs_size = ARRAY_SIZE(sensor_1080p60_regs),
	 },

};

#define N_WIN_SIZES (ARRAY_SIZE(sensor_win_sizes))

static int sensor_reg_init(struct sensor_info *info)
{
	struct v4l2_subdev *sd = &info->sd;
	struct sensor_win_size *w = info->current_wins;
	unsigned int hts, vts, width, height;
	int ret;

	if (!w)
		return -EINVAL;
	ret = sensor_write(sd, 0x0100, 0);
	if (ret)
		return ret;
	usleep_range(2000, 2500);
	if (w->fps_fixed != 60) {
		ret = imx214_write_table(sd, sensor_default_regs,
					 ARRAY_SIZE(sensor_default_regs));
		if (ret)
			return ret;
	}
	ret = imx214_write_table(sd, info->fmt->regs, info->fmt->regs_size);
	if (ret)
		return ret;
	ret = imx214_write_table(sd, w->regs, w->regs_size);
	if (ret)
		return ret;
	if (w->set_size) {
		ret = w->set_size(sd);
		if (ret)
			return ret;
	}
	ret = imx214_read16(sd, 0x0342, &hts);
	if (!ret)
		ret = imx214_read16(sd, 0x0340, &vts);
	if (!ret)
		ret = imx214_read16(sd, 0x034c, &width);
	if (!ret)
		ret = imx214_read16(sd, 0x034e, &height);
	if (ret)
		return ret;
	if (hts != w->hts || vts != w->vts ||
	    width != w->width || height != w->height)
		return -EIO;
	info->width = w->width;
	info->height = w->height;
	imx214_sensor_vts = w->vts;
	ret = imx214_snapshot(sd, "configured");
	if (ret)
		return ret;
	ret = sensor_write(sd, 0x0100, 1);
	if (!ret) {
		imx214_state(sd)->streaming = true;
		pr_info("imx214-mode width=%u height=%u nominal_fps=%u "
			"hts=%u vts=%u pclk=%u mipi_bps=%u\n",
			w->width, w->height, w->fps_fixed, w->hts, w->vts,
			w->pclk, w->mipi_bps);
	}
	return ret;
}

static int sensor_s_stream(struct v4l2_subdev *sd, int enable)
{
	int ret;

	if (!enable) {
		ret = sensor_write(sd, 0x0100, 0);
		if (!ret)
			imx214_state(sd)->streaming = false;
		return ret;
	}
	return sensor_reg_init(to_state(sd));
}

/* Enumerate each matching interval once; do not expose global table indexes. */
static int imx214_enum_interval(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_frame_interval_enum *fie)
{
	unsigned int i, match = 0;

	for (i = 0; i < N_WIN_SIZES; ++i) {
		struct sensor_win_size *w = &sensor_win_sizes[i];

		if (w->width != fie->width || w->height != fie->height)
			continue;
		if (match++ == fie->index) {
			fie->interval.numerator = 1;
			fie->interval.denominator = w->fps_fixed;
			return 0;
		}
	}
	return -EINVAL;
}

static int imx214_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct sensor_info *info = to_state(sd);

	/* Carry deskew into mbus reserved metadata before CSI setup. */
	info->deskew = info->tpf.denominator == 60 ? receiver_deskew : 7;
	return sensor_set_fmt(sd, state, fmt);
}

static int imx214_frame_interval(struct v4l2_subdev *sd,
				 struct v4l2_subdev_frame_interval *fi)
{
	struct sensor_win_size *w = to_state(sd)->current_wins;

	if (!w)
		return -EINVAL;
	fi->interval.numerator = w->hts * w->vts;
	fi->interval.denominator = w->pclk;
	return 0;
}

static int sensor_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				struct v4l2_mbus_config *cfg)
{
	cfg->type = V4L2_MBUS_CSI2_DPHY;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	cfg->flags = 0 | V4L2_MBUS_CSI2_4_LANE | V4L2_MBUS_CSI2_CHANNEL_0;
#else
	cfg->bus.mipi_csi2.num_data_lanes = 0 | V4L2_MBUS_CSI2_4_LANE | V4L2_MBUS_CSI2_CHANNEL_0;
#endif
	return 0;
}

static int sensor_g_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sensor_info *info =
			container_of(ctrl->handler, struct sensor_info, handler);
	struct v4l2_subdev *sd = &info->sd;

	switch (ctrl->id) {
	case V4L2_CID_GAIN:
		return sensor_g_gain(sd, &ctrl->val);
	case V4L2_CID_EXPOSURE:
		return sensor_g_exp(sd, &ctrl->val);
	}
	return -EINVAL;
}

static int sensor_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sensor_info *info =
			container_of(ctrl->handler, struct sensor_info, handler);
	struct v4l2_subdev *sd = &info->sd;

	switch (ctrl->id) {
	case V4L2_CID_FRAME_RATE:
		if (imx214_state(sd)->streaming)
			return -EBUSY;
		info->tpf.numerator = 1;
		info->tpf.denominator = ctrl->val;
		return 0;
	case V4L2_CID_GAIN:
		return sensor_s_gain(sd, ctrl->val);
	case V4L2_CID_EXPOSURE:
		return sensor_s_exp(sd, ctrl->val);
	}
	return -EINVAL;
}

/* ----------------------------------------------------------------------- */

static const struct v4l2_ctrl_ops sensor_ctrl_ops = {
	.g_volatile_ctrl = sensor_g_ctrl,
	.s_ctrl = sensor_s_ctrl,
};

static const struct v4l2_subdev_core_ops sensor_core_ops = {
	.reset = sensor_reset,
	.init = sensor_init,
	.s_power = sensor_power,
	.ioctl = sensor_ioctl,
#if IS_ENABLED(CONFIG_COMPAT)
	.compat_ioctl32 = sensor_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops sensor_video_ops = {
	.s_stream = sensor_s_stream,
	.g_frame_interval = imx214_frame_interval,
};

static const struct v4l2_subdev_pad_ops sensor_pad_ops = {
	.enum_mbus_code = sensor_enum_mbus_code,
	.enum_frame_size = sensor_enum_frame_size,
	.enum_frame_interval = imx214_enum_interval,
	.get_fmt = sensor_get_fmt,
	.set_fmt = imx214_set_fmt,
	.get_mbus_config = sensor_g_mbus_config,
};

static const struct v4l2_subdev_ops sensor_ops = {
	.core = &sensor_core_ops,
	.video = &sensor_video_ops,
	.pad = &sensor_pad_ops,
};

/* ----------------------------------------------------------------------- */
static struct cci_driver cci_drv = {
	.name = SENSOR_NAME,
	.addr_width = CCI_BITS_16,
	.data_width = CCI_BITS_8,
};

static const struct v4l2_ctrl_config sensor_custom_ctrls[] = {
	{
		.ops = &sensor_ctrl_ops,
		.id = V4L2_CID_FRAME_RATE,
		.name = "frame rate",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 30,
		.max = 60,
		.step = 30,
		.def = 30,
	},
};

static int sensor_init_controls(struct v4l2_subdev *sd, const struct v4l2_ctrl_ops *ops)
{
	struct sensor_info *info = to_state(sd);
	struct v4l2_ctrl_handler *handler = &info->handler;
	struct v4l2_ctrl *ctrl;
	int i;
	int ret = 0;

	v4l2_ctrl_handler_init(handler, 2 + ARRAY_SIZE(sensor_custom_ctrls));

	ctrl = v4l2_ctrl_new_std(handler, ops, V4L2_CID_EXPOSURE, 0,
			      65536 * 16, 1, 0);
	if (ctrl != NULL)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	v4l2_ctrl_new_std(handler, ops, V4L2_CID_GAIN, 1 * 1600,
			      256 * 1600, 1, 1 * 1600);
	for (i = 0; i < ARRAY_SIZE(sensor_custom_ctrls); i++)
		v4l2_ctrl_new_custom(handler, &sensor_custom_ctrls[i], NULL);

	if (handler->error) {
		ret = handler->error;
		v4l2_ctrl_handler_free(handler);
	}

	sd->ctrl_handler = handler;

	return ret;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0))
static int sensor_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
#else
static int sensor_probe(struct i2c_client *client)
#endif
{
	struct v4l2_subdev *sd;
	struct sensor_info *info;

	info = (struct sensor_info *)kzalloc(sizeof(struct imx214_state), GFP_KERNEL);
	if (info == NULL)
		return -ENOMEM;
	sd = &info->sd;
	cci_dev_probe_helper(sd, client, &sensor_ops, &cci_drv);
	sensor_init_controls(sd, &sensor_ctrl_ops);
	mutex_init(&info->lock);

#if IS_ENABLED(CONFIG_SAME_I2C)
	info->sensor_i2c_addr = I2C_ADDR >> 1;
#endif

	info->fmt = &sensor_formats[0];
	info->fmt_pt = &sensor_formats[0];
	info->win_pt = &sensor_win_sizes[0];
	info->fmt_num = N_FMTS;
	info->win_size_num = N_WIN_SIZES;
	info->current_wins = &sensor_win_sizes[2];
	info->sensor_field = V4L2_FIELD_NONE;
	info->af_first_flag = 1;

	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
static int sensor_remove(struct i2c_client *client)
#else
static void sensor_remove(struct i2c_client *client)
#endif
{
	struct v4l2_subdev *sd;

	sd = cci_dev_remove_helper(client, &cci_drv);
	kfree(to_state(sd));
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	return 0;
#endif
}

static const struct i2c_device_id sensor_id[] = {
	{SENSOR_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, sensor_id);

static struct i2c_driver sensor_driver = {
	.driver = {
		   .owner = THIS_MODULE,
		   .name = SENSOR_NAME,
		   },
	.probe = sensor_probe,
	.remove = sensor_remove,
	.id_table = sensor_id,
};
static __init int init_sensor(void)
{
	if (receiver_deskew < 1 || receiver_deskew > 31)
		return -EINVAL;
	return cci_dev_init_helper(&sensor_driver);
}

static __exit void exit_sensor(void)
{
	cci_dev_exit_helper(&sensor_driver);
}

VIN_INIT_DRIVERS(init_sensor);
module_exit(exit_sensor);
