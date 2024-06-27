// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the OV428 camera sensor.
 *
 * Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2017-2018, Linaro Ltd.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define OV428_SC_MODE_SELECT            0x0100    //reset
#define OV428_SC_MODE_SELECT_SW_STANDBY 0x0   //reset
#define OV428_SC_MODE_SELECT_STREAMING          0x1  //reset

#define OV428_CHIP_ID_HIGH              0x300a  //reset
#define OV428_CHIP_ID_HIGH_BYTE 0xfa             //reset
#define OV428_CHIP_ID_LOW               0x300b   //reset
#define OV428_CHIP_ID_LOW_BYTE          0x1f    //reset
#define OV428_SC_GP_IO_IN1              0x3029
#define OV428_AEC_EXPO_0                0x3500   //reset
#define OV428_AEC_EXPO_1                0x3501    //reset
#define OV428_AEC_EXPO_2                0x3502   //reset
#define OV428_AEC_AGC_ADJ_0             0x3a01    //reset
#define OV428_AEC_AGC_ADJ_1             0x3a02  //reset
#define OV428_TIMING_FORMAT1            0x3820
#define OV428_TIMING_FORMAT1_VFLIP      BIT(2)
#define OV428_TIMING_FORMAT2            0x3821
#define OV428_TIMING_FORMAT2_MIRROR     BIT(2)
#define OV428_PRE_ISP_00                0x5005  //reset
#define OV428_PRE_ISP_00_TEST_PATTERN   BIT(6)  //reset


struct reg_value {
	u16 reg;
	u8 val;
};

struct ov428_mode_info {
	u32 width;
	u32 height;
	const struct reg_value *data;
	u32 data_size;
	u32 pixel_clock;
	u32 link_freq;
	u16 exposure_max;
	u16 exposure_def;
	struct v4l2_fract timeperframe;
};

struct ov428 {
	struct i2c_client *i2c_client;
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_fwnode_endpoint ep;
	struct v4l2_mbus_framefmt fmt;
	struct v4l2_rect crop;
	struct clk *xclk;
	u32 xclk_freq;

	struct regulator *io_regulator;
	struct regulator *core_regulator;
	struct regulator *analog_regulator;

	const struct ov428_mode_info *current_mode;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *pixel_clock;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *gain;

	/* Cached register values */
	u8 aec_pk_manual;
	u8 pre_isp_00;
	u8 timing_format1;
	u8 timing_format2;

	struct mutex lock; /* lock to protect power state, ctrls and mode */
	bool power_on;

};

static inline struct ov428 *to_ov428(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov428, sd);
}

static const struct reg_value ov428_global_init_setting[] = {
	{ 0x0103, 0x01 },
};

static const struct reg_value ov428_setting_vga_30fps[] = {
	{ 0x0301, 0xc8 },
	{ 0x0304, 0x01 },
	{ 0x0305, 0xc4 },
	{ 0x0306, 0x04 },
	{ 0x0307, 0x00 },
	{ 0x0324, 0x01 },
	{ 0x0325, 0x90 },
	{ 0x032a, 0x09 },
	{ 0x032b, 0x00 },
	{ 0x032e, 0x00 },
	{ 0x3001, 0x20 },
	{ 0x300d, 0x00 },
	{ 0x3031, 0x02 },
	{ 0x3106, 0x20 },
	{ 0x3501, 0x00 },
	{ 0x3502, 0x04 },
	{ 0x3503, 0xaa },
	{ 0x3508, 0x01 },
	{ 0x3509, 0x00 },
	{ 0x3523, 0x03 },
	{ 0x3524, 0x0f },
	{ 0x3541, 0x00 },
	{ 0x3542, 0x04 },
	{ 0x3543, 0xaa },
	{ 0x3548, 0x01 },
	{ 0x3549, 0x00 },
	{ 0x3563, 0x03 },
	{ 0x3564, 0x0f },
	{ 0x3600, 0x00 },
	{ 0x3601, 0x00 },
	{ 0x360f, 0x80 },
	{ 0x3610, 0x2b },
	{ 0x3617, 0x08 },
	{ 0x3631, 0xb9 },
	{ 0x3660, 0x02 },
	{ 0x3663, 0x00 },
	{ 0x3665, 0x15 },
	{ 0x3668, 0x0c },
	{ 0x3701, 0x00 },
	{ 0x3737, 0xc0 },
	{ 0x3820, 0x00 },
	{ 0x3821, 0x02 },
	{ 0x3822, 0x00 },
	{ 0x3823, 0x02 },
	{ 0x3824, 0x05 },
	{ 0x3825, 0xe9 },
	{ 0x3826, 0x05 },
	{ 0x3827, 0xe9 },
	{ 0x3828, 0x05 },
	{ 0x3829, 0xdc },
	{ 0x382a, 0x05 },
	{ 0x382b, 0xdc },
	{ 0x382c, 0x06 },
	{ 0x382d, 0x68 },
	{ 0x382e, 0x06 },
	{ 0x382f, 0x5a },
	{ 0x3831, 0x06 },
	{ 0x3833, 0x06 },
	{ 0x3840, 0x00 },
	{ 0x3856, 0x16 },
	{ 0x3a02, 0x0f },
	{ 0x3a03, 0xe0 },
	{ 0x3a05, 0x30 },
	{ 0x3a0a, 0x00 },
	{ 0x3a0b, 0x7f },
	{ 0x3a0d, 0x04 },
	{ 0x3a18, 0x07 },
	{ 0x3a19, 0xff },
	{ 0x3b02, 0x00 },
	{ 0x3b03, 0x00 },
	{ 0x3b05, 0x30 },
	{ 0x3b0a, 0x00 },
	{ 0x3b0b, 0x7f },
	{ 0x3b0d, 0x04 },
	{ 0x3b18, 0x07 },
	{ 0x3b19, 0xff },
	{ 0x3f00, 0x09 },
	{ 0x3f05, 0xe0 },
	{ 0x3f0a, 0x00 },
	{ 0x3f0c, 0x00 },
	{ 0x3f0d, 0x56 },
	{ 0x3f0e, 0x64 },
	{ 0x4009, 0x01 },
	{ 0x400d, 0x01 },
	{ 0x4480, 0x02 },
	{ 0x480e, 0x00 },
	{ 0x4813, 0xe4 },
	{ 0x4827, 0x55 },
	{ 0x4837, 0x08 },
	{ 0x4b02, 0x28 },
	{ 0x4b03, 0x90 },
	{ 0x4b04, 0x00 },
	{ 0x4b05, 0x07 },
	{ 0x4b08, 0x7f },
	{ 0x4b0e, 0x8f },
	{ 0x4b0f, 0x28 },
	{ 0x4b10, 0x60 },
	{ 0x4b11, 0x60 },
	{ 0x4b12, 0x02 },
	{ 0x4b13, 0x01 },
	{ 0x4b14, 0x01 },
	{ 0x4b15, 0x01 },
	{ 0x4b16, 0x01 },
	{ 0x4b17, 0x01 },
	{ 0x4b1c, 0x02 },
	{ 0x4b1d, 0x1e },
	{ 0x4b1e, 0x01 },
	{ 0x4b1f, 0x02 },
	{ 0x4b20, 0x01 },
	{ 0x4b21, 0x02 },
	{ 0x4b22, 0x02 },
	{ 0x4b23, 0x02 },
	{ 0x4b24, 0x01 },
	{ 0x4b26, 0xa1 },
	{ 0x4b27, 0x01 },
	{ 0x4b29, 0x01 },
	{ 0x4b2b, 0x0e },
	{ 0x4b2c, 0x01 },
	{ 0x4b2d, 0x0a },
	{ 0x4b34, 0xd0 },
	{ 0x4b35, 0xaf },
	{ 0x4b36, 0x80 },
	{ 0x4b3d, 0x00 },
	{ 0x4b48, 0xbb },
	{ 0x4b49, 0x01 },
	{ 0x4f01, 0x12 },
	{ 0x5004, 0x94 },
	{ 0x5005, 0x00 },
	{ 0x500e, 0x00 },
	{ 0x5044, 0x06 },
	{ 0x5045, 0x06 },
	{ 0x5046, 0x04 },
	{ 0x5047, 0x02 },
	{ 0x5048, 0x05 },
	{ 0x5049, 0xdc },
	{ 0x504a, 0x05 },
	{ 0x504b, 0xdc },
	{ 0x504c, 0x02 },
	{ 0x504d, 0xd0 },
	{ 0x504e, 0x02 },
	{ 0x504f, 0xd0 },
	{ 0x5070, 0x04 },
	{ 0x5071, 0x02 },
	{ 0x5072, 0x04 },
	{ 0x5073, 0x02 },
	{ 0x5074, 0x02 },
	{ 0x5075, 0xd0 },
	{ 0x5076, 0x02 },
	{ 0x5077, 0xd0 },
	{ 0x5078, 0x02 },
	{ 0x5079, 0xd0 },
	{ 0x507a, 0x02 },
	{ 0x507b, 0xd0 },
	{ 0x5140, 0x00 },
	{ 0x5141, 0x00 },
	{ 0x5148, 0x05 },
	{ 0x5149, 0xdc },
	{ 0x514a, 0x05 },
	{ 0x514b, 0xdc },
	{ 0x5240, 0x33 },
	{ 0x5440, 0x00 },
	{ 0x5441, 0x00 },
	{ 0x3408, 0x0d },
	{ 0x4b03, 0xd0 },
	{ 0x4b0e, 0x8d },
	{ 0x3408, 0x1d },
	{ 0x3408, 0xad },
	{ 0x4b00, 0x00 },
	{ 0xc289, 0x20 },
	{ 0xc28d, 0x10 },
	{ 0xb208, 0x05 },
	{ 0xb800, 0x14 },
	{ 0xb87e, 0x02 },
	{ 0xb501, 0x02 },
	{ 0xb508, 0x02 },
	{ 0xb541, 0x02 },
	{ 0xb548, 0x02 },
	{ 0xb581, 0x02 },
	{ 0xb588, 0x02 },
	{ 0xb208, 0x15 },
	{ 0xb03a, 0x13 },
	{ 0x8301, 0xc8 },
	{ 0x8302, 0x31 },
	{ 0x8304, 0x01 },
	{ 0x8305, 0xf4 },
	{ 0x8307, 0x00 },
	{ 0x8309, 0x50 },
	{ 0x830a, 0x00 },
	{ 0x8320, 0x0a },
	{ 0x8324, 0x02 },
	{ 0x8325, 0x30 },
	{ 0x8326, 0xcd },
	{ 0x8327, 0x06 },
	{ 0x8329, 0x00 },
	{ 0x832a, 0x06 },
	{ 0x832b, 0x00 },
	{ 0x832f, 0xc1 },
	{ 0x8321, 0x01 },
	{ 0xb63b, 0x0e },
	{ 0x8360, 0x01 },
	{ 0xb01b, 0xf0 },
	{ 0xb020, 0x99 },
	{ 0xb022, 0x09 },
	{ 0xb026, 0xb4 },
	{ 0xb027, 0xf1 },
	{ 0xb038, 0x02 },
	{ 0xb03f, 0x03 },
	{ 0xb216, 0x31 },
	{ 0xb218, 0x24 },
	{ 0xb501, 0x00 },
	{ 0xb502, 0x80 },
	{ 0xb541, 0x00 },
	{ 0xb542, 0x40 },
	{ 0xb504, 0xc8 },
	{ 0xb507, 0x00 },
	{ 0xb508, 0x01 },
	{ 0xb509, 0x00 },
	{ 0xb50a, 0x01 },
	{ 0xb50b, 0x00 },
	{ 0xb50c, 0x00 },
	{ 0xb544, 0x48 },
	{ 0xb548, 0x01 },
	{ 0xb549, 0x00 },
	{ 0xb54a, 0x01 },
	{ 0xb54b, 0x00 },
	{ 0xb54c, 0x00 },
	{ 0xb600, 0x82 },
	{ 0xb601, 0x38 },
	{ 0xb603, 0x08 },
	{ 0xb610, 0x57 },
	{ 0xb613, 0x78 },
	{ 0xb623, 0x00 },
	{ 0xb641, 0x00 },
	{ 0xb642, 0x00 },
	{ 0xb645, 0x80 },
	{ 0xb64c, 0x70 },
	{ 0xb64d, 0x37 },
	{ 0xb65e, 0x02 },
	{ 0xb65f, 0x0f },
	{ 0xb700, 0x29 },
	{ 0xb701, 0x0d },
	{ 0xb702, 0x3c },
	{ 0xb703, 0x12 },
	{ 0xb704, 0x07 },
	{ 0xb705, 0x00 },
	{ 0xb706, 0x24 },
	{ 0xb707, 0x08 },
	{ 0xb708, 0x31 },
	{ 0xb709, 0x40 },
	{ 0xb70a, 0x00 },
	{ 0xb70b, 0x4a },
	{ 0xb70c, 0x11 },
	{ 0xb712, 0x51 },
	{ 0xb714, 0x24 },
	{ 0xb717, 0x01 },
	{ 0xb71d, 0x20 },
	{ 0xb71f, 0x09 },
	{ 0xb737, 0x08 },
	{ 0xb739, 0x28 },
	{ 0xb7e3, 0x08 },
	{ 0xb760, 0x08 },
	{ 0xb761, 0x0c },
	{ 0xb762, 0x08 },
	{ 0xb763, 0x04 },
	{ 0xb764, 0x04 },
	{ 0xb765, 0x08 },
	{ 0xb766, 0x10 },
	{ 0xb767, 0x08 },
	{ 0xb768, 0x04 },
	{ 0xb769, 0x1c },
	{ 0xb76c, 0x00 },
	{ 0xb791, 0x24 },
	{ 0xb79b, 0x4e },
	{ 0xb7ae, 0x00 },
	{ 0xb7e6, 0x08 },
	{ 0xb7cb, 0x03 },
	{ 0xb7cc, 0x01 },
	{ 0xb800, 0x00 },
	{ 0xb801, 0x00 },
	{ 0xb802, 0x00 },
	{ 0xb803, 0x00 },
	{ 0xb804, 0x05 },
	{ 0xb805, 0xeb },
	{ 0xb806, 0x05 },
	{ 0xb807, 0xeb },
	{ 0xb808, 0x05 },
	{ 0xb809, 0xec },
	{ 0xb80a, 0x05 },
	{ 0xb80b, 0xe8 },
	{ 0xb80c, 0x03 },
	{ 0xb80d, 0x34 },
	{ 0xb80e, 0x06 },
	{ 0xb80f, 0x5a },
	{ 0xb810, 0x00 },
	{ 0xb811, 0x00 },
	{ 0xb812, 0x00 },
	{ 0xb813, 0x02 },
	{ 0xb814, 0x11 },
	{ 0xb815, 0x11 },
	{ 0xb81a, 0x0c },
	{ 0xb81b, 0x9e },
	{ 0xb81f, 0x08 },
	{ 0xb820, 0x80 },
	{ 0xb821, 0x02 },
	{ 0xb822, 0x80 },
	{ 0xb823, 0x04 },
	{ 0xb82d, 0x00 },
	{ 0xb82e, 0x00 },
	{ 0xb831, 0x00 },
	{ 0xb837, 0x07 },
	{ 0xb83f, 0x40 },
	{ 0xb86b, 0x04 },
	{ 0xb871, 0x28 },
	{ 0xb894, 0x00 },
	{ 0xb94b, 0x0a },
	{ 0xb94c, 0x0a },
	{ 0xb94d, 0x0a },
	{ 0xb94e, 0x0a },
	{ 0xb94f, 0x01 },
	{ 0xb950, 0x01 },
	{ 0xb951, 0x01 },
	{ 0xb952, 0x01 },
	{ 0xb953, 0x01 },
	{ 0xb954, 0x01 },
	{ 0xb955, 0x01 },
	{ 0xb956, 0x01 },
	{ 0xb957, 0x10 },
	{ 0xb958, 0x0e },
	{ 0xb959, 0x0e },
	{ 0xb95a, 0x0e },
	{ 0xb95b, 0x12 },
	{ 0xb95c, 0x09 },
	{ 0xb95d, 0x05 },
	{ 0xb95e, 0x03 },
	{ 0xb95f, 0x00 },
	{ 0xb960, 0x00 },
	{ 0xb961, 0x00 },
	{ 0xb962, 0x00 },
	{ 0xb963, 0x00 },
	{ 0xb964, 0x00 },
	{ 0xb965, 0x00 },
	{ 0xb966, 0x00 },
	{ 0xb967, 0x00 },
	{ 0xb968, 0x01 },
	{ 0xb969, 0x01 },
	{ 0xb96a, 0x01 },
	{ 0xb96b, 0x01 },
	{ 0xb96c, 0x10 },
	{ 0xb96f, 0x00 },
	{ 0xb970, 0x2c },
	{ 0xb971, 0x2c },
	{ 0xb972, 0x2c },
	{ 0xb973, 0x10 },
	{ 0xb974, 0x00 },
	{ 0xb975, 0x31 },
	{ 0xb976, 0x31 },
	{ 0xb977, 0x31 },
	{ 0xb978, 0x12 },
	{ 0xb9b1, 0x01 },
	{ 0xb9be, 0x00 },
	{ 0xb400, 0x08 },
	{ 0xb421, 0x00 },
	{ 0xb422, 0x06 },
	{ 0xb424, 0x00 },
	{ 0xb426, 0x00 },
	{ 0xb427, 0x00 },
	{ 0xbf00, 0x10 },
	{ 0xbd85, 0x0b },
	{ 0xbd8c, 0x70 },
	{ 0xbd8d, 0x79 },
	{ 0xd112, 0x00 },
	{ 0xbdaa, 0x00 },
	{ 0xbdab, 0x10 },
	{ 0xbdae, 0x00 },
	{ 0xbdaf, 0x6f },
	{ 0xc000, 0xf8 },
	{ 0xc001, 0xeb },
	{ 0xc002, 0x00 },
	{ 0xc003, 0x10 },
	{ 0xc008, 0x00 },
	{ 0xc009, 0x0f },
	{ 0xc00a, 0x00 },
	{ 0xc00b, 0x17 },
	{ 0xc00c, 0x00 },
	{ 0xc00d, 0xa8 },
	{ 0xc00e, 0x04 },
	{ 0xc00f, 0xd1 },
	{ 0xc017, 0x02 },
	{ 0xc288, 0xc7 },
	{ 0xc29f, 0x00 },
	{ 0xc2a0, 0x31 },
	{ 0xc80e, 0x00 },
	{ 0xc837, 0x10 },
	{ 0xc850, 0x42 },
	{ 0xc883, 0x02 },
	{ 0xc885, 0x14 },
	{ 0xc88b, 0x03 },
	{ 0xcb00, 0x2a },
	{ 0xcb0d, 0x00 },
	{ 0xc500, 0x50 },
	{ 0xc501, 0x00 },
	{ 0xc502, 0x20 },
	{ 0xc503, 0x00 },
	{ 0xc504, 0x00 },
	{ 0xc505, 0x00 },
	{ 0xc508, 0x00 },
	{ 0xc50a, 0x04 },
	{ 0xc50c, 0x00 },
	{ 0xc50e, 0x00 },
	{ 0xc50f, 0x00 },
	{ 0xc800, 0x04 },
	{ 0xd000, 0x09 },
	{ 0xd110, 0x14 },
	{ 0xd111, 0x6b },
	{ 0xd410, 0x14 },
	{ 0xd411, 0x6b },
	{ 0xd160, 0x01 },
	{ 0xd161, 0x01 },
	{ 0xd164, 0x01 },
	{ 0xd165, 0x00 },
	{ 0xd152, 0x03 },
	{ 0xd154, 0x00 },
	{ 0xd155, 0x00 },
	{ 0xd156, 0x01 },
	{ 0xd157, 0x01 },
	{ 0xd158, 0x01 },
	{ 0xd159, 0x01 },
	{ 0xd15a, 0x01 },
	{ 0xd15b, 0x01 },
	{ 0xd166, 0x01 },
	{ 0xd167, 0x00 },
	{ 0xd0c0, 0x00 },
	{ 0xd038, 0x40 },
	{ 0xb016, 0x32 },
	{ 0xb65d, 0x00 },
	{ 0xc815, 0x40 },
	{ 0xc816, 0x12 },
	{ 0xc980, 0x00 },
	{ 0xcc03, 0x0c },
	{ 0xcc04, 0x18 },
	{ 0xcc05, 0x18 },
	{ 0xcc26, 0x18 },
	{ 0xc30c, 0xff },
	{ 0xcd00, 0x03 },
	{ 0xcd01, 0xcc },
	{ 0xcd02, 0xbb },
	{ 0xcd03, 0x2a },
	{ 0xcd04, 0x2c },
	{ 0xcd05, 0x74 },
	{ 0xc602, 0xf2 },
	{ 0xc608, 0x68 },
	{ 0xc680, 0x01 },
	{ 0xc683, 0x12 },
	{ 0xc68f, 0x06 },
	{ 0xb773, 0x04 },
	{ 0xb775, 0x11 },
	{ 0xb776, 0x04 },
	{ 0xb774, 0x0c },
	{ 0xb76d, 0xa1 },
	{ 0xb906, 0x00 },
	{ 0xb9d5, 0x00 },
	{ 0xb907, 0x00 },
	{ 0xb908, 0x00 },
	{ 0xb909, 0x00 },
	{ 0xb90c, 0x09 },
	{ 0xb97a, 0x03 },
	{ 0xb736, 0x30 },
	{ 0xb90a, 0x00 },
	{ 0xb911, 0x00 },
	{ 0xb917, 0x01 },
	{ 0xb918, 0x08 },
	{ 0xb919, 0x02 },
	{ 0xb920, 0x04 },
	{ 0xb7c6, 0x34 },
	{ 0xb7b0, 0x30 },
	{ 0xb7b2, 0x01 },
	{ 0xb914, 0x00 },
	{ 0xb910, 0x40 },
	{ 0xba9c, 0x0e },
	{ 0xba9d, 0x0c },
	{ 0xba9a, 0x2f },
	{ 0xb01c, 0xbc },
	{ 0xb01e, 0x1e },
	{ 0xb64b, 0x3a },
	{ 0xb640, 0x9e },
	{ 0xba49, 0x24 },
	{ 0xba4a, 0x24 },
	{ 0xba4b, 0x24 },
	{ 0xba4c, 0x24 },
	{ 0xba4d, 0x4a },
	{ 0xba4e, 0x4a },
	{ 0xba4f, 0x4a },
	{ 0xba50, 0x4a },
	{ 0xba52, 0x24 },
	{ 0xba53, 0x24 },
	{ 0xba54, 0x24 },
	{ 0xba6c, 0x80 },
	{ 0xba7b, 0x24 },
	{ 0xba7c, 0x4a },
	{ 0xba7d, 0x4a },
	{ 0xba7e, 0x4a },
	{ 0xba7f, 0x4a },
	{ 0xbaa0, 0x44 },
	{ 0xbaa6, 0x44 },
	{ 0xbaaa, 0x0d },
	{ 0xbadc, 0x08 },
	{ 0xb96d, 0xe0 },
	{ 0xb96e, 0x11 },
	{ 0xcc1f, 0x01 },
	{ 0x8100, 0x01 },
	{ 0xb218, 0x2c },
	{ 0xb821, 0x06 },
	{ 0xb809, 0xe8 },
	{ 0xb811, 0x01 },
	{ 0xb501, 0x06 },
	{ 0xb502, 0x44 },
	{ 0xb508, 0x01 },
	{ 0xb509, 0x00 },
	{ 0xb541, 0x00 },
	{ 0xb542, 0x04 },
	{ 0xb548, 0x01 },
	{ 0xb549, 0x00 },
	{ 0xb504, 0x48 },
	{ 0xb65e, 0x01 },
	{ 0xd000, 0x09 },
	{ 0x8000, 0x00 },
	{ 0x4b00, 0x10 },
	{ 0x4b00, 0x20 },
};
static const s64 link_freq[] = {
	240000000,
};

static const struct ov428_mode_info ov428_mode_info_data[] = {
	{
		.width = 1500,
		.height = 1500,
		.data = ov428_setting_vga_30fps,
		.data_size = ARRAY_SIZE(ov428_setting_vga_30fps),
		.pixel_clock = 24000000,
		.link_freq = 0, /* an index in link_freq[] */
		.exposure_max = 1704,
		.exposure_def = 504,
		.timeperframe = {
			.numerator = 100,
			.denominator = 3000
		}
	},

};

static int ov428_regulators_enable(struct ov428 *ov428)
{
	int ret;

	/* OV428 power up sequence requires core regulator
	 * to be enabled not earlier than io regulator
	 */
	ret = regulator_enable(ov428->io_regulator);
	if (ret < 0) {
		dev_err(ov428->dev, "set io voltage failed\n");
		return ret;
	}

	ret = regulator_enable(ov428->analog_regulator);
	if (ret) {
		dev_err(ov428->dev, "set analog voltage failed\n");
		goto err_disable_io;
	}

	ret = regulator_enable(ov428->core_regulator);
	if (ret) {
		dev_err(ov428->dev, "set core voltage failed\n");
		goto err_disable_analog;
	}
	return 0;

err_disable_analog:
	regulator_disable(ov428->analog_regulator);

err_disable_io:
	regulator_disable(ov428->io_regulator);

	return ret;
}

static void ov428_regulators_disable(struct ov428 *ov428)
{
	int ret;

	ret = regulator_disable(ov428->core_regulator);
	if (ret < 0)
		dev_err(ov428->dev, "core regulator disable failed\n");

	ret = regulator_disable(ov428->analog_regulator);
	if (ret < 0)
		dev_err(ov428->dev, "analog regulator disable failed\n");

	ret = regulator_disable(ov428->io_regulator);
	if (ret < 0)
		dev_err(ov428->dev, "io regulator disable failed\n");
}

static int ov428_write_reg(struct ov428 *ov428, u16 reg, u8 val)
{
	u8 regbuf[3];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;
	regbuf[2] = val;

	ret = i2c_master_send(ov428->i2c_client, regbuf, 3);
	if (ret < 0) {
		dev_err(ov428->dev, "%s: write reg error %d: reg=%x, val=%x\n",
			__func__, ret, reg, val);
		return ret;
	}

	return 0;
}

static int ov428_write_seq_regs(struct ov428 *ov428, u16 reg, u8 *val,
				 u8 num)
{
	u8 regbuf[5];
	u8 nregbuf = sizeof(reg) + num * sizeof(*val);
	int ret = 0;

	if (nregbuf > sizeof(regbuf))
		return -EINVAL;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;

	memcpy(regbuf + 2, val, num);

	ret = i2c_master_send(ov428->i2c_client, regbuf, nregbuf);
	if (ret < 0) {
		dev_err(ov428->dev,
			"%s: write seq regs error %d: first reg=%x\n",
			__func__, ret, reg);
		return ret;
	}

	return 0;
}

static int ov428_read_reg(struct ov428 *ov428, u16 reg, u8 *val)
{
	u8 regbuf[2];
	int ret;

	regbuf[0] = reg >> 8;
	regbuf[1] = reg & 0xff;

	ret = i2c_master_send(ov428->i2c_client, regbuf, 2);
	if (ret < 0) {
		dev_err(ov428->dev, "%s: write reg error %d: reg=%x\n",
			__func__, ret, reg);
		return ret;
	}

	ret = i2c_master_recv(ov428->i2c_client, val, 1);
	if (ret < 0) {
		dev_err(ov428->dev, "%s: read reg error %d: reg=%x\n",
			__func__, ret, reg);
		return ret;
	}

	return 0;
}

static int ov428_set_exposure(struct ov428 *ov428, s32 exposure)
{
	u16 reg;
	u8 val[3];

	reg = OV428_AEC_EXPO_0;
	val[0] = (exposure & 0xf000) >> 12; /* goes to OV428_AEC_EXPO_0 */
	val[1] = (exposure & 0x0ff0) >> 4;  /* goes to OV428_AEC_EXPO_1 */
	val[2] = (exposure & 0x000f) << 4;  /* goes to OV428_AEC_EXPO_2 */

	return ov428_write_seq_regs(ov428, reg, val, 3);
}

static int ov428_set_gain(struct ov428 *ov428, s32 gain)
{
	u16 reg;
	u8 val[2];

	reg = OV428_AEC_AGC_ADJ_0;
	val[0] = (gain & 0x0300) >> 8; /* goes to OV428_AEC_AGC_ADJ_0 */
	val[1] = gain & 0xff;          /* goes to OV428_AEC_AGC_ADJ_1 */

	return ov428_write_seq_regs(ov428, reg, val, 2);
}

static int ov428_set_register_array(struct ov428 *ov428,
				     const struct reg_value *settings,
				     unsigned int num_settings)
{
	unsigned int i;
	int ret;

	for (i = 0; i < num_settings; ++i, ++settings) {
		ret = ov428_write_reg(ov428, settings->reg, settings->val);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int ov428_set_power_on(struct ov428 *ov428)
{
	int ret;
	u32 wait_us;

/*
	ret = ov428_regulators_enable(ov428);
	if (ret < 0)
		return ret;
	ret = clk_prepare_enable(ov428->xclk);
	if (ret < 0) {
		dev_err(ov428->dev, "clk prepare enable failed\n");
		ov428_regulators_disable(ov428);
		return ret;
	}

*/
	/* wait at least 65536 external clock cycles */
	wait_us = DIV_ROUND_UP(65536 * 1000,
			       DIV_ROUND_UP(ov428->xclk_freq, 1000));
	usleep_range(wait_us, wait_us + 1000);

	return 0;
}

static void ov428_set_power_off(struct ov428 *ov428)
{
	clk_disable_unprepare(ov428->xclk);
	ov428_regulators_disable(ov428);
}

static int ov428_s_power(struct v4l2_subdev *sd, int on)
{
	struct ov428 *ov428 = to_ov428(sd);
	int ret = 0;

	return 0;
	mutex_lock(&ov428->lock);

	/* If the power state is not modified - no work to do. */
	if (ov428->power_on == !!on)
		goto exit;

	if (on) {
		ret = ov428_set_power_on(ov428);
		if (ret < 0)
			goto exit;

		ret = ov428_set_register_array(ov428,
					ov428_global_init_setting,
					ARRAY_SIZE(ov428_global_init_setting));
		if (ret < 0) {
			dev_err(ov428->dev, "could not set init registers\n");
			ov428_set_power_off(ov428);
			goto exit;
		}

		ov428->power_on = true;
	} else {
		ov428_set_power_off(ov428);
		ov428->power_on = false;
	}

exit:
	mutex_unlock(&ov428->lock);

	return ret;
}

static int ov428_set_hflip(struct ov428 *ov428, s32 value)
{
	u8 val = ov428->timing_format2;
	int ret;

	if (value)
		val |= OV428_TIMING_FORMAT2_MIRROR;
	else
		val &= ~OV428_TIMING_FORMAT2_MIRROR;

	ret = ov428_write_reg(ov428, OV428_TIMING_FORMAT2, val);
	if (!ret)
		ov428->timing_format2 = val;

	return ret;
}

static int ov428_set_vflip(struct ov428 *ov428, s32 value)
{
	u8 val = ov428->timing_format1;
	int ret;

	if (value)
		val |= OV428_TIMING_FORMAT1_VFLIP;
	else
		val &= ~OV428_TIMING_FORMAT1_VFLIP;

	ret = ov428_write_reg(ov428, OV428_TIMING_FORMAT1, val);
	if (!ret)
		ov428->timing_format1 = val;

	return ret;
}

static int ov428_set_test_pattern(struct ov428 *ov428, s32 value)
{
	u8 val = ov428->pre_isp_00;
	int ret;

	if (value)
		val |= OV428_PRE_ISP_00_TEST_PATTERN;
	else
		val &= ~OV428_PRE_ISP_00_TEST_PATTERN;

	ret = ov428_write_reg(ov428, OV428_PRE_ISP_00, val);
	if (!ret)
		ov428->pre_isp_00 = val;

	return ret;
}

static const char * const ov428_test_pattern_menu[] = {
	"Disabled",
	"Vertical Pattern Bars",
};

static int ov428_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov428 *ov428 = container_of(ctrl->handler,
					     struct ov428, ctrls);
	int ret;
	/* v4l2_ctrl_lock() locks our mutex */

	if (!ov428->power_on)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		printk("1");
		ret = ov428_set_exposure(ov428, ctrl->val);
		break;
	case V4L2_CID_GAIN:
		printk("2");
		ret = ov428_set_gain(ov428, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		printk("3");
		ret = ov428_set_test_pattern(ov428, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
		printk("4");
		ret = ov428_set_hflip(ov428, ctrl->val);
		break;
	case V4L2_CID_VFLIP:
		printk("5");
		ret = ov428_set_vflip(ov428, ctrl->val);
		break;
	default:
		printk("6");
		ret = ov428_set_vflip(ov428, ctrl->val);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops ov428_ctrl_ops = {
	.s_ctrl = ov428_s_ctrl,
};

static int ov428_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_Y10_1X10;

	return 0;
}

static int ov428_enum_frame_size(struct v4l2_subdev *subdev,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->code != MEDIA_BUS_FMT_Y10_1X10)
		return -EINVAL;

	if (fse->index >= ARRAY_SIZE(ov428_mode_info_data))
		return -EINVAL;

	fse->min_width = ov428_mode_info_data[fse->index].width;
	fse->max_width = ov428_mode_info_data[fse->index].width;
	fse->min_height = ov428_mode_info_data[fse->index].height;
	fse->max_height = ov428_mode_info_data[fse->index].height;

	return 0;
}

static int ov428_enum_frame_ival(struct v4l2_subdev *subdev,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_interval_enum *fie)
{
	unsigned int index = fie->index;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ov428_mode_info_data); i++) {
		if (fie->width != ov428_mode_info_data[i].width ||
		    fie->height != ov428_mode_info_data[i].height)
			continue;

		if (index-- == 0) {
			fie->interval = ov428_mode_info_data[i].timeperframe;
			return 0;
		}
	}

	return -EINVAL;
}

static struct v4l2_mbus_framefmt *
__ov428_get_pad_format(struct ov428 *ov428,
			struct v4l2_subdev_pad_config *cfg,
			unsigned int pad,
			enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(&ov428->sd, cfg, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &ov428->fmt;
	default:
		return NULL;
	}
}

static int ov428_get_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_pad_config *cfg,
			     struct v4l2_subdev_format *format)
{
	struct ov428 *ov428 = to_ov428(sd);

	mutex_lock(&ov428->lock);
	format->format = *__ov428_get_pad_format(ov428, cfg, format->pad,
						  format->which);
	mutex_unlock(&ov428->lock);

	return 0;
}

static struct v4l2_rect *
__ov428_get_pad_crop(struct ov428 *ov428, struct v4l2_subdev_pad_config *cfg,
		      unsigned int pad, enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_crop(&ov428->sd, cfg, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &ov428->crop;
	default:
		return NULL;
	}
}

static inline u32 avg_fps(const struct v4l2_fract *t)
{
	return (t->denominator + (t->numerator >> 1)) / t->numerator;
}

static const struct ov428_mode_info *
ov428_find_mode_by_ival(struct ov428 *ov428, struct v4l2_fract *timeperframe)
{
	const struct ov428_mode_info *mode = ov428->current_mode;
	unsigned int fps_req = avg_fps(timeperframe);
	unsigned int max_dist_match = (unsigned int) -1;
	unsigned int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(ov428_mode_info_data); i++) {
		unsigned int dist;
		unsigned int fps_tmp;

		if (mode->width != ov428_mode_info_data[i].width ||
		    mode->height != ov428_mode_info_data[i].height)
			continue;

		fps_tmp = avg_fps(&ov428_mode_info_data[i].timeperframe);

		dist = abs(fps_req - fps_tmp);

		if (dist < max_dist_match) {
			n = i;
			max_dist_match = dist;
		}
	}

	return &ov428_mode_info_data[n];
}

static int ov428_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_pad_config *cfg,
			     struct v4l2_subdev_format *format)
{
	struct ov428 *ov428 = to_ov428(sd);
	struct v4l2_mbus_framefmt *__format;
	struct v4l2_rect *__crop;
	const struct ov428_mode_info *new_mode;
	int ret = 0;

	mutex_lock(&ov428->lock);

	__crop = __ov428_get_pad_crop(ov428, cfg, format->pad, format->which);

	new_mode = v4l2_find_nearest_size(ov428_mode_info_data,
				ARRAY_SIZE(ov428_mode_info_data),
				width, height,
				format->format.width, format->format.height);

	__crop->width = new_mode->width;
	__crop->height = new_mode->height;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		ret = __v4l2_ctrl_s_ctrl_int64(ov428->pixel_clock,
					       new_mode->pixel_clock);
		if (ret < 0)
			goto exit;

		ret = __v4l2_ctrl_s_ctrl(ov428->link_freq,
					 new_mode->link_freq);
		if (ret < 0)
			goto exit;

		ret = __v4l2_ctrl_modify_range(ov428->exposure,
					       1, new_mode->exposure_max,
					       1, new_mode->exposure_def);
		if (ret < 0)
			goto exit;

		ret = __v4l2_ctrl_s_ctrl(ov428->exposure,
					 new_mode->exposure_def);
		if (ret < 0)
			goto exit;

		ret = __v4l2_ctrl_s_ctrl(ov428->gain, 16);
		if (ret < 0)
			goto exit;

		ov428->current_mode = new_mode;
	}

	__format = __ov428_get_pad_format(ov428, cfg, format->pad,
					   format->which);
	__format->width = __crop->width;
	__format->height = __crop->height;
	__format->code = MEDIA_BUS_FMT_Y10_1X10;
	__format->field = V4L2_FIELD_NONE;
	__format->colorspace = V4L2_COLORSPACE_SRGB;
	__format->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(__format->colorspace);
	__format->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(true,
				__format->colorspace, __format->ycbcr_enc);
	__format->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(__format->colorspace);

	format->format = *__format;

exit:
	mutex_unlock(&ov428->lock);

	return ret;
}

static int ov428_entity_init_cfg(struct v4l2_subdev *subdev,
				  struct v4l2_subdev_pad_config *cfg)
{
	struct v4l2_subdev_format fmt = {
		.which = cfg ? V4L2_SUBDEV_FORMAT_TRY
			     : V4L2_SUBDEV_FORMAT_ACTIVE,
		.format = {
			.width = 640,
			.height = 480
		}
	};

	ov428_set_format(subdev, cfg, &fmt);

	return 0;
}

static int ov428_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_selection *sel)
{
	struct ov428 *ov428 = to_ov428(sd);

	if (sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	mutex_lock(&ov428->lock);
	sel->r = *__ov428_get_pad_crop(ov428, cfg, sel->pad,
					sel->which);
	mutex_unlock(&ov428->lock);

	return 0;
}

static int ov428_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct ov428 *ov428 = to_ov428(subdev);
	int ret;

	mutex_lock(&ov428->lock);

	if (enable) {
		ret = ov428_set_register_array(ov428,
					ov428->current_mode->data,
					ov428->current_mode->data_size);
		if (ret < 0) {
			dev_err(ov428->dev, "could not set mode %dx%d\n",
				ov428->current_mode->width,
				ov428->current_mode->height);
			goto exit;
		}
		ret = __v4l2_ctrl_handler_setup(&ov428->ctrls);
		if (ret < 0) {
			dev_err(ov428->dev, "could not sync v4l2 controls\n");
			goto exit;
		}
		ret = ov428_write_reg(ov428, OV428_SC_MODE_SELECT,
				       OV428_SC_MODE_SELECT_STREAMING);
	} else {
		ret = ov428_write_reg(ov428, OV428_SC_MODE_SELECT,
				       OV428_SC_MODE_SELECT_SW_STANDBY);
	}

exit:
	mutex_unlock(&ov428->lock);

	return ret;
}

static int ov428_get_frame_interval(struct v4l2_subdev *subdev,
				     struct v4l2_subdev_frame_interval *fi)
{
	struct ov428 *ov428 = to_ov428(subdev);

	mutex_lock(&ov428->lock);
	fi->interval = ov428->current_mode->timeperframe;
	mutex_unlock(&ov428->lock);

	return 0;
}

static int ov428_set_frame_interval(struct v4l2_subdev *subdev,
				     struct v4l2_subdev_frame_interval *fi)
{
	struct ov428 *ov428 = to_ov428(subdev);
	const struct ov428_mode_info *new_mode;
	int ret = 0;

	mutex_lock(&ov428->lock);
	new_mode = ov428_find_mode_by_ival(ov428, &fi->interval);

	if (new_mode != ov428->current_mode) {
		ret = __v4l2_ctrl_s_ctrl_int64(ov428->pixel_clock,
					       new_mode->pixel_clock);
		if (ret < 0)
			goto exit;

		ret = __v4l2_ctrl_s_ctrl(ov428->link_freq,
					 new_mode->link_freq);
		if (ret < 0)
			goto exit;

		ret = __v4l2_ctrl_modify_range(ov428->exposure,
					       1, new_mode->exposure_max,
					       1, new_mode->exposure_def);
		if (ret < 0)
			goto exit;

		ret = __v4l2_ctrl_s_ctrl(ov428->exposure,
					 new_mode->exposure_def);
		if (ret < 0)
			goto exit;

		ret = __v4l2_ctrl_s_ctrl(ov428->gain, 16);
		if (ret < 0)
			goto exit;

		ov428->current_mode = new_mode;
	}

	fi->interval = ov428->current_mode->timeperframe;

exit:
	mutex_unlock(&ov428->lock);

	return ret;
}

static const struct v4l2_subdev_core_ops ov428_core_ops = {
	.s_power = ov428_s_power,
};

static const struct v4l2_subdev_video_ops ov428_video_ops = {
	.s_stream = ov428_s_stream,
	.g_frame_interval = ov428_get_frame_interval,
	.s_frame_interval = ov428_set_frame_interval,
};

static const struct v4l2_subdev_pad_ops ov428_subdev_pad_ops = {
	.init_cfg = ov428_entity_init_cfg,
	.enum_mbus_code = ov428_enum_mbus_code,
	.enum_frame_size = ov428_enum_frame_size,
	.enum_frame_interval = ov428_enum_frame_ival,
	.get_fmt = ov428_get_format,
	.set_fmt = ov428_set_format,
	.get_selection = ov428_get_selection,
};

static const struct v4l2_subdev_ops ov428_subdev_ops = {
	.core = &ov428_core_ops,
	.video = &ov428_video_ops,
	.pad = &ov428_subdev_pad_ops,
};

static int ov428_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	struct ov428 *ov428;
	u8 chip_id_high, chip_id_low, chip_rev;
	int ret;

	printk("Found device 1");
	ov428 = devm_kzalloc(dev, sizeof(struct ov428), GFP_KERNEL);
	if (!ov428)
		return -ENOMEM;

	ov428->i2c_client = client;
	ov428->dev = dev;
	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}
	ret = v4l2_fwnode_endpoint_parse(endpoint, &ov428->ep);
	fwnode_handle_put(endpoint);
	if (ret < 0) {
		dev_err(dev, "parsing endpoint node failed\n");
		return ret;
	}
	if (ov428->ep.bus_type != V4L2_MBUS_CSI2_DPHY) {
		dev_err(dev, "invalid bus type (%u), must be CSI2 (%u)\n",
			ov428->ep.bus_type, V4L2_MBUS_CSI2_DPHY);
		return -EINVAL;
	}
	/*
	ov428->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(ov428->xclk)) {
		dev_err(dev, "could not get xclk");
		return PTR_ERR(ov428->xclk);
	}
	*/
	ret = fwnode_property_read_u32(dev_fwnode(dev), "clock-frequency",
				       &ov428->xclk_freq);
	if (ret) {
		dev_err(dev, "could not get xclk frequency\n");
		return ret;
	}

	if (ov428->xclk_freq < 23760000 || ov428->xclk_freq > 24240000) {
		dev_err(dev, "external clock frequency %u is not supported\n",
			ov428->xclk_freq);
		return -EINVAL;
	}
/*
	ret = clk_set_rate(ov428->xclk, ov428->xclk_freq);
	if (ret) {
		dev_err(dev, "could not set xclk frequency\n");
		return ret;
	}
	ov428->io_regulator = devm_regulator_get(dev, "dovdd");
	if (IS_ERR(ov428->io_regulator)) {
		dev_err(dev, "cannot get io regulator\n");
		return PTR_ERR(ov428->io_regulator);
	}

	ov428->core_regulator = devm_regulator_get(dev, "dvdd");
	if (IS_ERR(ov428->core_regulator)) {
		dev_err(dev, "cannot get core regulator\n");
		return PTR_ERR(ov428->core_regulator);
	}

	ov428->analog_regulator = devm_regulator_get(dev, "avdd");
	if (IS_ERR(ov428->analog_regulator)) {
		dev_err(dev, "cannot get analog regulator\n");
		return PTR_ERR(ov428->analog_regulator);
	}
*/
	mutex_init(&ov428->lock);

	v4l2_ctrl_handler_init(&ov428->ctrls, 7);
	ov428->ctrls.lock = &ov428->lock;
/*
	v4l2_ctrl_new_std(&ov428->ctrls, &ov428_ctrl_ops,
			  V4L2_CID_HFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&ov428->ctrls, &ov428_ctrl_ops,
			  V4L2_CID_VFLIP, 0, 1, 1, 0);
			  */
	ov428->exposure = v4l2_ctrl_new_std(&ov428->ctrls, &ov428_ctrl_ops,
					     V4L2_CID_EXPOSURE, 1, 32, 1, 32);
	ov428->gain = v4l2_ctrl_new_std(&ov428->ctrls, &ov428_ctrl_ops,
					 V4L2_CID_GAIN, 16, 1023, 1, 16);
	v4l2_ctrl_new_std_menu_items(&ov428->ctrls, &ov428_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(ov428_test_pattern_menu) - 1,
				     0, 0, ov428_test_pattern_menu);
	ov428->pixel_clock = v4l2_ctrl_new_std(&ov428->ctrls,
						&ov428_ctrl_ops,
						V4L2_CID_PIXEL_RATE,
						1, INT_MAX, 1, 1);
	ov428->link_freq = v4l2_ctrl_new_int_menu(&ov428->ctrls,
						   &ov428_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(link_freq) - 1,
						   0, link_freq);
	if (ov428->link_freq)
		ov428->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ov428->sd.ctrl_handler = &ov428->ctrls;

	if (ov428->ctrls.error) {
		dev_err(dev, "%s: control initialization error %d\n",
			__func__, ov428->ctrls.error);
		ret = ov428->ctrls.error;
		goto free_ctrl;
	}
	v4l2_i2c_subdev_init(&ov428->sd, client, &ov428_subdev_ops);
	ov428->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ov428->pad.flags = MEDIA_PAD_FL_SOURCE; //SOURCE;
	ov428->sd.dev = &client->dev;
	ov428->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&ov428->sd.entity, 1, &ov428->pad);
	if (ret < 0) {
		dev_err(dev, "could not register media entity\n");
		goto free_ctrl;
	}
	/*
	ret = ov428_s_power(&ov428->sd, true);  
	if (ret < 0) {
		dev_err(dev, "could not power up OV428\n");
		goto free_entity;
	}
	*/
	ret = ov428_read_reg(ov428, OV428_CHIP_ID_HIGH, &chip_id_high);
	if (ret < 0 || chip_id_high != OV428_CHIP_ID_HIGH_BYTE) {
		dev_err(dev, "could not read ID high\n");
		ret = -ENODEV;
		goto power_down;
	}
	ret = ov428_read_reg(ov428, OV428_CHIP_ID_LOW, &chip_id_low);
	if (ret < 0 || chip_id_low != OV428_CHIP_ID_LOW_BYTE) {
		dev_err(dev, "could not read ID low\n");
		ret = -ENODEV;
		goto power_down;
	}
	ret = ov428_read_reg(ov428, OV428_SC_GP_IO_IN1, &chip_rev);
	if (ret < 0) {
		dev_err(dev, "could not read revision\n");
		ret = -ENODEV;
		goto power_down;
	}
	chip_rev >>= 4;

	dev_info(dev, "OV428 revision %x (%s) detected at address 0x%02x\n",
		 chip_rev,
		 chip_rev == 0x4 ? "1A / 1B" :
		 chip_rev == 0x5 ? "1C / 1D" :
		 chip_rev == 0x6 ? "1E" :
		 chip_rev == 0x7 ? "1F" : "unknown",
		 client->addr);
	ret = ov428_read_reg(ov428, OV428_PRE_ISP_00,
			      &ov428->pre_isp_00);
	if (ret < 0) {
		dev_err(dev, "could not read test pattern value\n");
		ret = -ENODEV;
		goto power_down;
	}

	ret = ov428_read_reg(ov428, OV428_TIMING_FORMAT1,
			      &ov428->timing_format1);
	if (ret < 0) {
		dev_err(dev, "could not read vflip value\n");
		ret = -ENODEV;
		goto power_down;
	}

	ret = ov428_read_reg(ov428, OV428_TIMING_FORMAT2,
			      &ov428->timing_format2);
	if (ret < 0) {
		dev_err(dev, "could not read hflip value\n");
		ret = -ENODEV;
		goto power_down;
	}
//	ov428_s_power(&ov428->sd, false);
	ret = v4l2_async_register_subdev(&ov428->sd);
	if (ret < 0) {
		dev_err(dev, "could not register v4l2 device\n");
		goto free_entity;
	}
	ov428_entity_init_cfg(&ov428->sd, NULL);

	return 0;
power_down:
	return 0;
	ov428_s_power(&ov428->sd, false);
free_entity:
	media_entity_cleanup(&ov428->sd.entity);
free_ctrl:
	v4l2_ctrl_handler_free(&ov428->ctrls);
	mutex_destroy(&ov428->lock);
	return ret;
}

static int ov428_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov428 *ov428 = to_ov428(sd);

	v4l2_async_unregister_subdev(&ov428->sd);
	media_entity_cleanup(&ov428->sd.entity);
	v4l2_ctrl_handler_free(&ov428->ctrls);
	mutex_destroy(&ov428->lock);

	return 0;
}

static const struct of_device_id ov428_of_match[] = {
	{ .compatible = "ovti,ov428" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov428_of_match);

static struct i2c_driver ov428_i2c_driver = {
	.driver = {
		.of_match_table = ov428_of_match,
		.name  = "ov428",
	},
	.probe_new  = ov428_probe,
	.remove = ov428_remove,
};

module_i2c_driver(ov428_i2c_driver);

MODULE_DESCRIPTION("Omnivision OV428 Camera Driver");
MODULE_AUTHOR("Todor Tomov <todor.tomov@linaro.org>");
MODULE_LICENSE("GPL v2");
