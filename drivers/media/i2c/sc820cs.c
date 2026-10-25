// SPDX-License-Identifier: GPL-2.0
/*
 * A V4L2 driver for the SmartSens SC820CS 8M camera sensor.
 *
 * Target hardware: OnePlus Pad 2 Pro (OPD2413, "erhai", Qualcomm SM8750)
 * front camera.  Hardware facts (I2C address 0x20, MCLK4 19.2 MHz,
 * RESET4, avdd 2.8 V / dvdd 1.2 V / dovdd 1.8 V, chip id 0xd154 @ 0x3107)
 * are taken from the vendor device tree
 * (OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8750,
 * branch oneplus/sm8750_b_16.0.0_pad_2_pro,
 * vendor/qcom/opensource/camera-devicetree/oplus/erhai_camera_overlay_common_evb.dtsi).
 *
 * Driver skeleton follows drivers/media/i2c/imx219.c.
 * SmartSens register map conventions (0x0100 streaming, 0x3e00.. exposure,
 * 0x3e06..0x3e09 gain, 0x320c..0x320f HTS/VTS, 0x3221 flip/mirror,
 * 0x4501 BIT(3) test pattern) are taken from the Rockchip SC8238 driver
 * (rockchip-linux kernel develop-6.1 drivers/media/i2c/sc8238.c, GPL-2.0),
 * a sibling SmartClarity sensor using the same register layout.
 *
 * Register table status (TODO(vendor-regs) resolved):
 * The full 116-entry power-up / PLL / analog tuning sequence for the
 * 3264x2448 mode is transplanted from the OnePlus Pad Pro ("caihong",
 * SM8650) downstream camera tree, via the out-of-tree driver at
 * https://github.com/Osilvfe/out_tree_module (GPL-2.0-only, copyright
 * (c) 2026 Osilvfe <osilvfe@gmail.com>), where it is hardware-verified
 * to stream 3264x2448 RAW10 at 24 fps.  See sc820cs-caihong-regs.h.
 * The exposure and analogue-gain register encodings below are taken from
 * that driver as well.
 *
 * Cross-validation against the erhai odm blob
 * (com.qti.sensormodule.erhaifront.bin): the caihong register sequence is
 * a strict subsequence of the erhai register-write script, in identical
 * order; erhai only adds 9 analog-tuning registers on top (see the regs
 * header).  The register *values* of the erhai script are not decoded
 * yet, and the erhai-specific tuning delta remains unverified.
 *
 * erhai board specifics that differ from caihong (board facts live in DT,
 * not in this driver): DVDD rail is L11B (caihong: L2G), the sensor sits
 * on QUP SE9 I2C rather than a CCI master, feeding csiphy5 (caihong:
 * CSIPHY4).
 *
 * I2C address: the erhai vendor DT uses reg = <0x20>, but the caihong
 * README reports the sensor answering at the Linux 7-bit address 0x10
 * (0x20 would be its 8-bit write form).  The erhai DT keeps 0x20 for
 * now; i2cdetect on QUP SE9 must settle 0x10 vs 0x20.  待硬件验证.
 *
 * Copyright (C) 2026 erhai mainline project
 * Mode table and control encodings derived from the caihong SC820CS
 * driver, Copyright (c) 2026 Osilvfe <osilvfe@gmail.com> (GPL-2.0-only)
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

#include "sc820cs-caihong-regs.h"

/* Chip ID: 16-bit value spread over 0x3107 (high) / 0x3108 (low).
 * 0xd154 verified against the erhai vendor DT "ftm_setting_compatible"
 * property which reads 2 bytes at 0x3107 and expects 0xd154. */
#define SC820CS_REG_CHIP_ID		CCI_REG16(0x3107)
#define SC820CS_CHIP_ID			0xd154

/* Software reset and streaming control */
#define SC820CS_REG_SW_RESET		CCI_REG8(0x0103)
#define SC820CS_SW_RESET		0x01
#define SC820CS_REG_MODE_SELECT		CCI_REG8(0x0100)
#define SC820CS_MODE_STANDBY		0x00
#define SC820CS_MODE_STREAMING		0x01

/* Exposure: line count, applied to the sensor in half-line units.
 * encoded = lines * 2 is spread over four registers: 0x3e20 [23:20],
 * 0x3e00 [19:12], 0x3e01 [11:4] and 0x3e02 [7:5] (written as
 * (lines & 7) << 5).  Encoding as hardware-verified on caihong. */
#define SC820CS_REG_EXPOSURE_20		CCI_REG8(0x3e20)
#define SC820CS_REG_EXPOSURE_H		CCI_REG8(0x3e00)
#define SC820CS_REG_EXPOSURE_M		CCI_REG8(0x3e01)
#define SC820CS_REG_EXPOSURE_L		CCI_REG8(0x3e02)
#define SC820CS_EXPOSURE_MIN		1
#define SC820CS_EXPOSURE_STEP		1
#define SC820CS_EXPOSURE_DEFAULT	750
#define SC820CS_EXPOSURE_MARGIN		6

/* Analogue gain, 1/1024 units: 1024 = 1x .. 16384 = 16x.  The range is
 * selected by a coarse code in 0x3e08 and a fine value in 0x3e07 counted
 * in 1/128 of the selected range.  Encoding as hardware-verified on
 * caihong. */
#define SC820CS_REG_AGAIN_COARSE	CCI_REG8(0x3e08)
#define SC820CS_REG_AGAIN_FINE		CCI_REG8(0x3e07)
#define SC820CS_AGAIN_MIN		1024
#define SC820CS_AGAIN_MAX		16384
#define SC820CS_AGAIN_STEP		1
#define SC820CS_AGAIN_DEFAULT		1024

/* Frame timing: HTS @ 0x320c and VTS @ 0x320e, both 16-bit.  The vendor
 * 3264x2448 mode runs HTS 3888 / VTS 2500 at 24 fps.  The mode register
 * list does not program them (reset defaults + runtime control writes
 * carry the verified values). */
#define SC820CS_REG_HTS			CCI_REG16(0x320c)
#define SC820CS_REG_VTS			CCI_REG16(0x320e)
#define SC820CS_HTS_DEF			3888
#define SC820CS_VTS_DEF			2500
#define SC820CS_HTS_MAX			0xffff
#define SC820CS_VTS_MAX			0xffff

/* Mirror / flip: mirror bits [2:1], flip bits [6:5]. Same as SC8238. */
#define SC820CS_REG_FLIP_MIRROR		CCI_REG8(0x3221)
#define SC820CS_FETCH_MIRROR(v, en)	((en) ? ((v) | 0x06) : ((v) & 0xf9))
#define SC820CS_FETCH_FLIP(v, en)	((en) ? ((v) | 0x60) : ((v) & 0x9f))

/* See the HFLIP/VFLIP case in sc820cs_set_ctrl(): nonzero makes the
 * control path write this exact value to 0x3221 so candidate flip
 * encodings can be swept at runtime. */
static unsigned int reg_3221_override;
module_param(reg_3221_override, uint, 0644);
MODULE_PARM_DESC(reg_3221_override, "raw value forced into 0x3221 at stream setup (0 = normal RMW)");

/* Test pattern: 0x4501 BIT(3) enables color bars.  Derived from SC8238;
 * the caihong driver does not exercise this register, so the encoding is
 * unverified on SC820CS.  待硬件验证. */
#define SC820CS_REG_TEST_PATTERN	CCI_REG8(0x4501)
#define SC820CS_TEST_PATTERN_BIT	BIT(3)
#define SC820CS_TEST_PATTERN_DEF_VAL	0xa4

/* 19.2 MHz MCLK4, per the erhai vendor DT clock-rates property */
#define SC820CS_XCLK_FREQ		19200000

/* 4 data lanes, RAW10, link frequency 368 MHz => 736 Mbps/lane (DDR).
 * Ground truth for the link rate: Android CAM_START_PHYDEV for PHY-5 on
 * this tablet reports Datarate 736000000/lane, and TheMojoMan's verified
 * Pad 3 tree (same module) ships the same 368 MHz figure — this replaces
 * the earlier 366 MHz borrowed from caihong, which was never verified on
 * erhai.  camss derives the CSIPHY settle count from
 * V4L2_CID_LINK_FREQ, so the DT link-frequencies property must carry
 * this same value (check_hwcfg rejects anything else).  The pixel rate
 * stays at the caihong-measured 3888 * 2500 * 24 = 233.28 MHz.
 * 0x301f = 0x0e in the vendor table is the 4-lane lane-mode value. */
#define SC820CS_NUM_DATA_LANES		4
#define SC820CS_LINK_FREQ		368000000ULL
#define SC820CS_PIXEL_RATE		233280000ULL

#define SC820CS_NATIVE_WIDTH		3264U
#define SC820CS_NATIVE_HEIGHT		2448U

/* Regulator supplies; voltages from the erhai vendor DT:
 * dovdd 1.8 V (L4B), avdd 2.8 V (L16B), dvdd 1.2 V (L11B). */
static const char * const sc820cs_supply_name[] = {
	"dovdd", /* IO  (1.8V) - erhai L4B  */
	"avdd",  /* analog (2.8V) - erhai L16B */
	"dvdd",  /* core (1.2V) - erhai L11B */
};

#define SC820CS_NUM_SUPPLIES		ARRAY_SIZE(sc820cs_supply_name)

struct sc820cs_mode {
	unsigned int width;
	unsigned int height;
	unsigned int hts_def;
	unsigned int vts_def;
	const struct cci_reg_sequence *reg_list;
	unsigned int reg_list_len;
};

/*
 * Mode register lists.
 *
 * The 3264x2448 list is the caihong-verified vendor sequence
 * (sc820cs-caihong-regs.h): soft reset, PLL gates, 4-lane MIPI mode and
 * the analog tuning blocks.  It starts with a software reset and is
 * written with a settle delay after the first entry - see
 * sc820cs_enable_streams().
 *
 * Binned modes (1080p etc.) exist in the erhai odm blob (5 mode blocks
 * at 0x1000 stride) but are not decoded yet; only the hardware-verified
 * full-resolution mode is offered until then.
 */
static const struct sc820cs_mode supported_modes[] = {
	{
		/* Full resolution 8MP @ 24 fps, RAW10 */
		.width		= 3264,
		.height		= 2448,
		.hts_def	= SC820CS_HTS_DEF,
		.vts_def	= SC820CS_VTS_DEF,
		.reg_list	= sc820cs_caihong_3264x2448_regs,
		.reg_list_len	= ARRAY_SIZE(sc820cs_caihong_3264x2448_regs),
	},
};

/* Analogue gain range steps: coarse code for 0x3e08 with the gain value
 * (1/1024 units) at the start of each range.  From the caihong driver. */
struct sc820cs_gain_step {
	u32 gain;
	u8 coarse;
};

static const struct sc820cs_gain_step sc820cs_gain_steps[] = {
	{ 1024, 0x00 },
	{ 2048, 0x08 },
	{ 4096, 0x09 },
	{ 8192, 0x0b },
	{ 16384, 0x0f },
};

static const s64 sc820cs_link_freq_menu[] = {
	SC820CS_LINK_FREQ,
};

/* Bayer order at the un-flipped readout is BGGR (hardware-verified: the
 * yellow-object test reads yellow with this code).  A readout rotation
 * moves the CFA start, so the four flip combinations map as
 * BGGR -> GBRG (h) -> GRBG (v) -> RGGB (both); same mapping as imx219.
 * Order: no flip, hflip, vflip, h+v flip. */
static const u32 sc820cs_mbus_formats[] = {
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SRGGB10_1X10,
};

static const char * const sc820cs_test_pattern_menu[] = {
	"Disabled",
	"Color Bars",
};

struct sc820cs {
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct regmap *regmap;
	struct clk *xclk;

	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[SC820CS_NUM_SUPPLIES];

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;

	const struct sc820cs_mode *mode;
};

static inline struct sc820cs *to_sc820cs(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct sc820cs, sd);
}

static u32 sc820cs_get_format_code(struct sc820cs *sc820cs, u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sc820cs_mbus_formats); i++)
		if (sc820cs_mbus_formats[i] == code)
			break;

	if (i >= ARRAY_SIZE(sc820cs_mbus_formats))
		i = 0;

	i = (i & ~3) | (sc820cs->vflip->val ? 2 : 0) |
	    (sc820cs->hflip->val ? 1 : 0);

	return sc820cs_mbus_formats[i];
}

/* -----------------------------------------------------------------------------
 * Controls
 */

static int sc820cs_set_exposure(struct sc820cs *sc820cs, u32 exposure)
{
	u32 encoded = exposure * 2;
	int ret = 0;

	cci_write(sc820cs->regmap, SC820CS_REG_EXPOSURE_20,
		  (encoded >> 20) & 0x0f, &ret);
	cci_write(sc820cs->regmap, SC820CS_REG_EXPOSURE_H,
		  (encoded >> 12) & 0xff, &ret);
	cci_write(sc820cs->regmap, SC820CS_REG_EXPOSURE_M,
		  (encoded >> 4) & 0xff, &ret);
	cci_write(sc820cs->regmap, SC820CS_REG_EXPOSURE_L,
		  (exposure & 0x07) << 5, &ret);

	return ret;
}

static int sc820cs_set_analogue_gain(struct sc820cs *sc820cs, u32 gain)
{
	const struct sc820cs_gain_step *step = &sc820cs_gain_steps[0];
	unsigned int i, fine;
	int ret = 0;

	for (i = 1; i < ARRAY_SIZE(sc820cs_gain_steps); i++) {
		if (gain < sc820cs_gain_steps[i].gain)
			break;
		step = &sc820cs_gain_steps[i];
	}

	fine = (gain * 128 + step->gain / 2) / step->gain;

	cci_write(sc820cs->regmap, SC820CS_REG_AGAIN_COARSE,
		  step->coarse, &ret);
	cci_write(sc820cs->regmap, SC820CS_REG_AGAIN_FINE,
		  clamp_val(fine, 0, 0xff), &ret);

	return ret;
}

static int sc820cs_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sc820cs *sc820cs =
		container_of(ctrl->handler, struct sc820cs, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&sc820cs->sd);
	const struct v4l2_mbus_framefmt *format;
	struct v4l2_subdev_state *state;
	u64 read_val;
	s64 val;
	int ret = 0;

	state = v4l2_subdev_get_locked_active_state(&sc820cs->sd);
	format = v4l2_subdev_state_get_format(state, 0);

	if (ctrl->id == V4L2_CID_VBLANK) {
		int exposure_max;

		exposure_max = format->height + ctrl->val -
				       SC820CS_EXPOSURE_MARGIN;
		ret = __v4l2_ctrl_modify_range(sc820cs->exposure,
					       sc820cs->exposure->minimum,
					       exposure_max,
					       sc820cs->exposure->step,
					       sc820cs->exposure->default_value);
		if (ret)
			return ret;
	}

	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = sc820cs_set_exposure(sc820cs, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = sc820cs_set_analogue_gain(sc820cs, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		val = format->height + ctrl->val;
		cci_write(sc820cs->regmap, SC820CS_REG_VTS, val, &ret);
		break;
	case V4L2_CID_HBLANK:
		val = format->width + ctrl->val;
		cci_write(sc820cs->regmap, SC820CS_REG_HTS, val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		val = ctrl->val ? SC820CS_TEST_PATTERN_BIT : 0;
		cci_update_bits(sc820cs->regmap, SC820CS_REG_TEST_PATTERN,
				SC820CS_TEST_PATTERN_BIT, val, &ret);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		/*
		 * Laboratory override: writing a nonzero reg_3221 to the
		 * module parameter makes the flip path write that exact
		 * value instead of the read-modify-write, so candidate
		 * bits (single bits of 0x3221, or other registers after
		 * extending SC820CS_REG_FLIP_MIRROR) can be swept at
		 * runtime without a rebuild.  Zero keeps normal behaviour.
		 */
		if (reg_3221_override) {
			cci_write(sc820cs->regmap, SC820CS_REG_FLIP_MIRROR,
				  reg_3221_override, &ret);
			break;
		}
		ret = cci_read(sc820cs->regmap, SC820CS_REG_FLIP_MIRROR,
			       &read_val, NULL);
		if (ret)
			break;
		val = SC820CS_FETCH_MIRROR(read_val, sc820cs->hflip->val);
		val = SC820CS_FETCH_FLIP(val, sc820cs->vflip->val);
		cci_write(sc820cs->regmap, SC820CS_REG_FLIP_MIRROR,
			  val, &ret);
		break;
	default:
		dev_dbg(&client->dev, "ctrl 0x%x not handled\n", ctrl->id);
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops sc820cs_ctrl_ops = {
	.s_ctrl = sc820cs_set_ctrl,
};

static int sc820cs_init_controls(struct sc820cs *sc820cs)
{
	struct i2c_client *client = v4l2_get_subdevdata(&sc820cs->sd);
	const struct sc820cs_mode *mode = &supported_modes[0];
	struct v4l2_ctrl_handler *ctrl_hdlr;
	struct v4l2_fwnode_device_properties props;
	int exposure_max, exposure_def;
	int ret;

	ctrl_hdlr = &sc820cs->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 9);
	if (ret)
		return ret;

	sc820cs->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &sc820cs_ctrl_ops,
						V4L2_CID_PIXEL_RATE,
						SC820CS_PIXEL_RATE,
						SC820CS_PIXEL_RATE, 1,
						SC820CS_PIXEL_RATE);

	sc820cs->link_freq =
		v4l2_ctrl_new_int_menu(ctrl_hdlr, &sc820cs_ctrl_ops,
				       V4L2_CID_LINK_FREQ,
				       ARRAY_SIZE(sc820cs_link_freq_menu) - 1,
				       0, sc820cs_link_freq_menu);
	if (sc820cs->link_freq)
		sc820cs->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* VBLANK/HBLANK minimums are pinned to the hardware-verified vendor
	 * timing (HTS 3888 / VTS 2500); frame timing can only be stretched,
	 * never shrunk below the known-good mode. */
	sc820cs->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &sc820cs_ctrl_ops,
					    V4L2_CID_VBLANK,
					    mode->vts_def - mode->height,
					    SC820CS_VTS_MAX - mode->height, 1,
					    mode->vts_def - mode->height);

	sc820cs->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &sc820cs_ctrl_ops,
					    V4L2_CID_HBLANK,
					    mode->hts_def - mode->width,
					    SC820CS_HTS_MAX - mode->width, 1,
					    mode->hts_def - mode->width);

	exposure_max = mode->vts_def - SC820CS_EXPOSURE_MARGIN;
	exposure_def = min_t(int, exposure_max, SC820CS_EXPOSURE_DEFAULT);
	sc820cs->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &sc820cs_ctrl_ops,
					      V4L2_CID_EXPOSURE,
					      SC820CS_EXPOSURE_MIN,
					      exposure_max,
					      SC820CS_EXPOSURE_STEP,
					      exposure_def);

	v4l2_ctrl_new_std(ctrl_hdlr, &sc820cs_ctrl_ops,
			  V4L2_CID_ANALOGUE_GAIN,
			  SC820CS_AGAIN_MIN, SC820CS_AGAIN_MAX,
			  SC820CS_AGAIN_STEP, SC820CS_AGAIN_DEFAULT);

	sc820cs->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &sc820cs_ctrl_ops,
					   V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (sc820cs->hflip)
		sc820cs->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	/*
	 * Measured 2026-10-25 (frame correlation at both register states):
	 * 0x00 -> 0x66 is a true 180-degree rotation of the readout, and the
	 * 0x66 state is the one that comes out upside-down on this module —
	 * so the un-flipped POR state is upright.  Default both flips OFF;
	 * the rotated readout also moves the Bayer start.  Every colour
	 * observation the user made (yellow->blue, blue-black) was under the
	 * SBGGR report in the 0x66 state, so that state is really RGGB and
	 * the phase at 0x00 is SBGGR.
	 */
	sc820cs->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &sc820cs_ctrl_ops,
					   V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (sc820cs->vflip)
		sc820cs->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &sc820cs_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(sc820cs_test_pattern_menu) - 1,
				     0, 0, sc820cs_test_pattern_menu);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err_probe(&client->dev, ret, "control init failed\n");
		goto error;
	}

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &sc820cs_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	sc820cs->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

/* -----------------------------------------------------------------------------
 * Subdev operations
 */

static int sc820cs_enable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  u32 pad, u64 streams_mask)
{
	struct sc820cs *sc820cs = to_sc820cs(sd);
	struct i2c_client *client = v4l2_get_subdevdata(&sc820cs->sd);
	int ret;

	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret < 0)
		return ret;

	/* Mode register list (soft reset, PLL, 4-lane MIPI, analog tuning).
	 * The vendor sequence starts with a software reset (0x0103 = 1); let
	 * the sensor settle before writing the rest of the list (5 ms, per
	 * the caihong driver). */
	ret = cci_write(sc820cs->regmap, sc820cs->mode->reg_list[0].reg,
			sc820cs->mode->reg_list[0].val, NULL);
	if (ret) {
		dev_err(&client->dev, "failed to soft reset sensor\n");
		goto err_rpm_put;
	}

	usleep_range(5000, 6000);

	ret = cci_multi_reg_write(sc820cs->regmap,
				  &sc820cs->mode->reg_list[1],
				  sc820cs->mode->reg_list_len - 1, NULL);
	if (ret) {
		dev_err(&client->dev, "failed to write mode registers\n");
		goto err_rpm_put;
	}

	/* Apply controls (exposure/gain/timing) on top of the mode list */
	ret = __v4l2_ctrl_handler_setup(sc820cs->sd.ctrl_handler);
	if (ret)
		goto err_rpm_put;

	ret = cci_write(sc820cs->regmap, SC820CS_REG_MODE_SELECT,
			SC820CS_MODE_STREAMING, NULL);
	if (ret)
		goto err_rpm_put;

	__v4l2_ctrl_grab(sc820cs->hflip, true);
	__v4l2_ctrl_grab(sc820cs->vflip, true);

	return 0;

err_rpm_put:
	pm_runtime_put_autosuspend(&client->dev);
	return ret;
}

static int sc820cs_disable_streams(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   u32 pad, u64 streams_mask)
{
	struct sc820cs *sc820cs = to_sc820cs(sd);
	struct i2c_client *client = v4l2_get_subdevdata(&sc820cs->sd);
	int ret;

	ret = cci_write(sc820cs->regmap, SC820CS_REG_MODE_SELECT,
			SC820CS_MODE_STANDBY, NULL);
	if (ret)
		dev_err(&client->dev, "failed to set standby\n");

	__v4l2_ctrl_grab(sc820cs->hflip, false);
	__v4l2_ctrl_grab(sc820cs->vflip, false);

	pm_runtime_put_autosuspend(&client->dev);

	return ret;
}

static int sc820cs_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct sc820cs *sc820cs = to_sc820cs(sd);

	if (code->index >= ARRAY_SIZE(sc820cs_mbus_formats) / 4)
		return -EINVAL;

	code->code = sc820cs_get_format_code(sc820cs,
			sc820cs_mbus_formats[code->index * 4]);

	return 0;
}

static int sc820cs_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct sc820cs *sc820cs = to_sc820cs(sd);
	u32 code;

	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	code = sc820cs_get_format_code(sc820cs, fse->code);
	if (fse->code != code)
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int sc820cs_set_pad_format(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_format *fmt)
{
	struct sc820cs *sc820cs = to_sc820cs(sd);
	const struct sc820cs_mode *mode;
	struct v4l2_mbus_framefmt *format;
	int exposure_max, exposure_def;

	format = v4l2_subdev_state_get_format(state, 0);

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	fmt->format.code = sc820cs_get_format_code(sc820cs, fmt->format.code);
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->format.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;

	*format = fmt->format;
	sc820cs->mode = mode;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		exposure_max = mode->vts_def - SC820CS_EXPOSURE_MARGIN;
		exposure_def = min_t(int, exposure_max,
				     SC820CS_EXPOSURE_DEFAULT);

		__v4l2_ctrl_modify_range(sc820cs->vblank,
					 mode->vts_def - mode->height,
					 SC820CS_VTS_MAX - mode->height, 1,
					 mode->vts_def - mode->height);
		__v4l2_ctrl_s_ctrl(sc820cs->vblank,
				   mode->vts_def - mode->height);

		__v4l2_ctrl_modify_range(sc820cs->hblank,
					 mode->hts_def - mode->width,
					 SC820CS_HTS_MAX - mode->width, 1,
					 mode->hts_def - mode->width);
		__v4l2_ctrl_s_ctrl(sc820cs->hblank,
				   mode->hts_def - mode->width);

		__v4l2_ctrl_modify_range(sc820cs->exposure,
					 sc820cs->exposure->minimum,
					 exposure_max,
					 sc820cs->exposure->step,
					 exposure_def);
	}

	return 0;
}

static int sc820cs_get_selection(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		return 0;

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = SC820CS_NATIVE_WIDTH;
		sel->r.height = SC820CS_NATIVE_HEIGHT;

		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		/* Full active array; the vendor register table does not
		 * carry crop offsets, so the exact array offset is unknown.
		 * 待硬件验证. */
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = SC820CS_NATIVE_WIDTH;
		sel->r.height = SC820CS_NATIVE_HEIGHT;

		return 0;
	}

	return -EINVAL;
}

static int sc820cs_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.code = MEDIA_BUS_FMT_SBGGR10_1X10,
			.width = supported_modes[0].width,
			.height = supported_modes[0].height,
		},
	};
	struct v4l2_rect *crop;
	int ret;

	ret = sc820cs_set_pad_format(sd, state, &fmt);
	if (ret)
		return ret;

	/* get_selection serves V4L2_SEL_TGT_CROP straight from the state;
	 * leave it zeroed and the crop reads back as 0x0, which makes
	 * libcamera warn and default the analogue rectangle itself.  The
	 * vendor register table carries no array offset, so anchor the
	 * crop at (0,0) covering the whole active mode. */
	crop = v4l2_subdev_state_get_crop(state, 0);
	crop->left = 0;
	crop->top = 0;
	crop->width = supported_modes[0].width;
	crop->height = supported_modes[0].height;

	return 0;
}

static const struct v4l2_subdev_video_ops sc820cs_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops sc820cs_pad_ops = {
	.enum_mbus_code = sc820cs_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = sc820cs_set_pad_format,
	.get_selection = sc820cs_get_selection,
	.enum_frame_size = sc820cs_enum_frame_size,
	.enable_streams = sc820cs_enable_streams,
	.disable_streams = sc820cs_disable_streams,
};

static const struct v4l2_subdev_ops sc820cs_subdev_ops = {
	.video = &sc820cs_video_ops,
	.pad = &sc820cs_pad_ops,
};

static const struct v4l2_subdev_internal_ops sc820cs_internal_ops = {
	.init_state = sc820cs_init_state,
};

/* -----------------------------------------------------------------------------
 * Power management
 */

/*
 * Power-up sequence, mirroring the vendor qcom cam_sensor_power_up()
 * (AOSP camera driver, cam_sensor_core.c), which walks the DT
 * power-sequence table one entry at a time:
 *
 *   1. regulators enabled individually in DT order (vio -> vana -> vdig),
 *      each followed by its configured settle delay (~1 ms here),
 *   2. MCLK enabled (19.2 MHz),
 *   3. RESET asserted, held >= 1 ms, released, then ~10 ms before the
 *      first I2C transaction.
 *
 * The reset line is ACTIVE_LOW in DT, so gpiod logical 1 == physical low
 * == asserted, logical 0 == released (consumer.rst, active_low_semantics:
 * gpiod_set_value_xxx() operates on the logical value).  The old code
 * reasoned in raw levels twice over: the sensor was never asserted before
 * the rails came up, and - after the DT flip to ACTIVE_LOW - it was held
 * IN reset during the chip-id read, which is why reg 0x3107 read -5.
 */
#define SC820CS_SUPPLY_SETTLE_US	1000	/* rail-to-rail settle */
#define SC820CS_RESET_ASSERT_US		1000	/* reset assert width */
#define SC820CS_RESET_RELEASE_US		10000	/* release to first I2C */

static int sc820cs_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sc820cs *sc820cs = to_sc820cs(sd);
	int i, ret;

	/* Assert reset (logical 1 -> physical low) while the rails come up */
	gpiod_set_value_cansleep(sc820cs->reset_gpio, 1);

	/* Rails one by one, DT order: dovdd (vio) -> avdd (vana) -> dvdd
	 * (vdig), ~1 ms apart - not a simultaneous bulk_enable. */
	for (i = 0; i < SC820CS_NUM_SUPPLIES; i++) {
		ret = regulator_enable(sc820cs->supplies[i].consumer);
		if (ret) {
			dev_err(dev, "%s: failed to enable %s regulator\n",
				__func__, sc820cs->supplies[i].supply);
			goto err_reg_off;
		}
		usleep_range(SC820CS_SUPPLY_SETTLE_US,
			     SC820CS_SUPPLY_SETTLE_US + 500);
	}

	ret = clk_prepare_enable(sc820cs->xclk);
	if (ret) {
		dev_err(dev, "%s: failed to enable clock\n", __func__);
		goto err_reg_off;
	}

	/* Reset pulse: assert >= 1 ms, release, wait ~10 ms for the sensor
	 * boot ROM before the first I2C access. */
	gpiod_set_value_cansleep(sc820cs->reset_gpio, 1);
	usleep_range(SC820CS_RESET_ASSERT_US, SC820CS_RESET_ASSERT_US + 500);
	gpiod_set_value_cansleep(sc820cs->reset_gpio, 0);
	usleep_range(SC820CS_RESET_RELEASE_US, SC820CS_RESET_RELEASE_US + 2000);

	return 0;

err_reg_off:
	while (i--)
		regulator_disable(sc820cs->supplies[i].consumer);

	return ret;
}

static int sc820cs_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sc820cs *sc820cs = to_sc820cs(sd);

	/* Mirror of power-up: hold reset asserted, stop MCLK, rails down */
	gpiod_set_value_cansleep(sc820cs->reset_gpio, 1);
	clk_disable_unprepare(sc820cs->xclk);
	regulator_bulk_disable(SC820CS_NUM_SUPPLIES, sc820cs->supplies);

	return 0;
}

/* -----------------------------------------------------------------------------
 * Probe & remove
 */

static int sc820cs_identify_module(struct sc820cs *sc820cs)
{
	struct i2c_client *client = v4l2_get_subdevdata(&sc820cs->sd);
	int ret, i;
	u64 val;

	/* the first transaction right after reset release can race the
	 * sensor's boot ROM; 8 x 20 ms is the verified recipe from the
	 * Pad 3 tree (probe + GNOME Snapshot stream, commit ed20467e) */
	for (i = 0; i < 8; i++) {
		ret = cci_read(sc820cs->regmap, SC820CS_REG_CHIP_ID, &val, NULL);
		if (!ret)
			break;
		usleep_range(20000, 25000);
	}
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to read chip id (%#x)\n",
				     SC820CS_CHIP_ID);

	if (val != SC820CS_CHIP_ID)
		return dev_err_probe(&client->dev, -EIO,
				     "chip id mismatch: %#x != %#llx\n",
				     SC820CS_CHIP_ID, val);

	return 0;
}

static int sc820cs_check_hwcfg(struct device *dev, struct sc820cs *sc820cs)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	unsigned long link_freq_bitmap;
	int ret = -EINVAL;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint)
		return dev_err_probe(dev, -EINVAL,
				     "endpoint node not found\n");

	if (v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep_cfg)) {
		dev_err_probe(dev, -EINVAL, "could not parse endpoint\n");
		goto error_out;
	}

	/* 4-lane per the hardware-verified caihong vendor table
	 * (0x301f = 0x0e); the earlier 2-lane assumption is dropped.
	 * 待硬件验证 on erhai. */
	if (ep_cfg.bus.mipi_csi2.num_data_lanes != SC820CS_NUM_DATA_LANES) {
		dev_err_probe(dev, -EINVAL,
			      "only %u data lanes are supported\n",
			      SC820CS_NUM_DATA_LANES);
		goto error_out;
	}

	ret = v4l2_link_freq_to_bitmap(dev,
				       ep_cfg.link_frequencies,
				       ep_cfg.nr_of_link_frequencies,
				       sc820cs_link_freq_menu,
				       ARRAY_SIZE(sc820cs_link_freq_menu),
				       &link_freq_bitmap);
	if (ret || !(link_freq_bitmap & BIT(0))) {
		ret = -EINVAL;
		dev_err_probe(dev, -EINVAL,
			      "link frequency %lld not supported\n",
			      ep_cfg.nr_of_link_frequencies ?
			      ep_cfg.link_frequencies[0] : 0);
	}

error_out:
	v4l2_fwnode_endpoint_free(&ep_cfg);
	fwnode_handle_put(endpoint);

	return ret;
}

static int sc820cs_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sc820cs *sc820cs;
	int ret, i;

	sc820cs = devm_kzalloc(dev, sizeof(*sc820cs), GFP_KERNEL);
	if (!sc820cs)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&sc820cs->sd, client, &sc820cs_subdev_ops);
	sc820cs->sd.internal_ops = &sc820cs_internal_ops;
	sc820cs->mode = &supported_modes[0];

	if (sc820cs_check_hwcfg(dev, sc820cs))
		return -EINVAL;

	sc820cs->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sc820cs->regmap))
		return dev_err_probe(dev, PTR_ERR(sc820cs->regmap),
				     "failed to initialize CCI regmap\n");

	sc820cs->xclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(sc820cs->xclk))
		return dev_err_probe(dev, PTR_ERR(sc820cs->xclk),
				     "failed to get xclk\n");

	if (clk_get_rate(sc820cs->xclk) != SC820CS_XCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "xclk frequency not supported: %lu Hz\n",
				     clk_get_rate(sc820cs->xclk));

	for (i = 0; i < SC820CS_NUM_SUPPLIES; i++)
		sc820cs->supplies[i].supply = sc820cs_supply_name[i];

	ret = devm_regulator_bulk_get(dev, SC820CS_NUM_SUPPLIES,
				      sc820cs->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	/* GPIOD_OUT_HIGH is logical assert: with the ACTIVE_LOW DT flag this
	 * drives the physical pin LOW, so the sensor starts (and stays) in
	 * reset until sc820cs_power_on() pulses it. */
	sc820cs->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(sc820cs->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sc820cs->reset_gpio),
				     "failed to get reset gpio\n");

	/* The sensor must be powered to read the chip id */
	ret = sc820cs_power_on(dev);
	if (ret)
		return ret;

	ret = sc820cs_identify_module(sc820cs);
	if (ret)
		goto error_power_off;

	/* Make sure the sensor is in standby after the ID check */
	ret = cci_write(sc820cs->regmap, SC820CS_REG_MODE_SELECT,
			SC820CS_MODE_STANDBY, NULL);
	if (ret < 0)
		goto error_power_off;

	ret = sc820cs_init_controls(sc820cs);
	if (ret)
		goto error_power_off;

	sc820cs->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sc820cs->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	sc820cs->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sc820cs->sd.entity, 1, &sc820cs->pad);
	if (ret) {
		dev_err_probe(dev, ret, "failed to init entity pads\n");
		goto error_handler_free;
	}

	sc820cs->sd.state_lock = sc820cs->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&sc820cs->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret, "subdev init error\n");
		goto error_media_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&sc820cs->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret,
			      "failed to register sensor sub-device\n");
		goto error_subdev_cleanup;
	}

	pm_runtime_idle(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	return 0;

error_subdev_cleanup:
	v4l2_subdev_cleanup(&sc820cs->sd);
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);

error_media_entity:
	media_entity_cleanup(&sc820cs->sd.entity);

error_handler_free:
	v4l2_ctrl_handler_free(sc820cs->sd.ctrl_handler);

error_power_off:
	sc820cs_power_off(dev);

	return ret;
}

static void sc820cs_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sc820cs *sc820cs = to_sc820cs(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sc820cs->sd.ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev)) {
		sc820cs_power_off(&client->dev);
		pm_runtime_set_suspended(&client->dev);
	}
}

static const struct of_device_id sc820cs_dt_ids[] = {
	{ .compatible = "smartsens,sc820cs" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sc820cs_dt_ids);

static const struct dev_pm_ops sc820cs_pm_ops = {
	SET_RUNTIME_PM_OPS(sc820cs_power_off, sc820cs_power_on, NULL)
};

static struct i2c_driver sc820cs_i2c_driver = {
	.driver = {
		.name = "sc820cs",
		.of_match_table = sc820cs_dt_ids,
		.pm = &sc820cs_pm_ops,
	},
	.probe = sc820cs_probe,
	.remove = sc820cs_remove,
};

module_i2c_driver(sc820cs_i2c_driver);

MODULE_AUTHOR("erhai mainline project");
MODULE_AUTHOR("Osilvfe <osilvfe@gmail.com> (caihong register table)");
MODULE_DESCRIPTION("SmartSens SC820CS sensor driver");
MODULE_LICENSE("GPL v2");
