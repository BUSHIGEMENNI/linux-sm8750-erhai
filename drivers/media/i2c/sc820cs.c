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
 * IMPORTANT - register tables are NOT the vendor values:
 * For the SM8750 ("sun") generation Qualcomm removed per-sensor drivers
 * from the kernel; the SC820CS resolution/PLL register arrays live in the
 * userspace camx sensor module (com.qti.sensor.sc820cs) which is not GPL
 * source.  The mode register lists below therefore contain the *window /
 * timing / MIPI lane* registers with computed values, but the vendor PLL
 * and analog tuning blocks are missing.  They are marked
 * "TODO(vendor-regs)" and MUST be transplanted from the userspace module
 * before valid frames can be expected on hardware.  待硬件验证.
 *
 * Copyright (C) 2026 erhai mainline project
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

/* Exposure: 16-bit line count packed over three registers
 * (0x3e00 = H nibble, 0x3e01 = M byte, 0x3e02 = L nibble << 4).
 * Same layout as SC8238. 待硬件验证 for SC820CS. */
#define SC820CS_REG_EXPOSURE_H		CCI_REG8(0x3e00)
#define SC820CS_REG_EXPOSURE_M		CCI_REG8(0x3e01)
#define SC820CS_REG_EXPOSURE_L		CCI_REG8(0x3e02)
#define SC820CS_EXPOSURE_MIN		4
#define SC820CS_EXPOSURE_STEP		1
#define SC820CS_EXPOSURE_DEFAULT	0x250
#define SC820CS_EXPOSURE_OFFSET		8

/* Analog gain: 0x3e08 = coarse (range select), 0x3e09 = fine (7-bit).
 * Gain unit is 1/128: 128 = 1x, 1016 = 7.9375x analog ceiling.
 * Above that the digital gain registers 0x3e06/0x3e07 continue
 * (not exposed as a separate control here). 待硬件验证. */
#define SC820CS_REG_AGAIN_COARSE	CCI_REG8(0x3e08)
#define SC820CS_REG_AGAIN_FINE		CCI_REG8(0x3e09)
#define SC820CS_AGAIN_COARSE_1X		0x03
#define SC820CS_AGAIN_COARSE_2X		0x07
#define SC820CS_AGAIN_COARSE_4X		0x0f
#define SC820CS_AGAIN_COARSE_8X		0x1f
#define SC820CS_AGAIN_MIN		128
#define SC820CS_AGAIN_MAX		1016
#define SC820CS_AGAIN_STEP		1
#define SC820CS_AGAIN_DEFAULT		128

/* Frame timing */
#define SC820CS_REG_HTS_H		CCI_REG8(0x320c)
#define SC820CS_REG_HTS_L		CCI_REG8(0x320d)
#define SC820CS_REG_VTS_H		CCI_REG8(0x320e)
#define SC820CS_REG_VTS_L		CCI_REG8(0x320f)
#define SC820CS_VTS_MAX			0x7fff
#define SC820CS_VBLANK_MIN		8
#define SC820CS_HTS_MIN			0x0e10

/* Output window size */
#define SC820CS_REG_OUT_WIDTH_H		CCI_REG8(0x3208)
#define SC820CS_REG_OUT_WIDTH_L		CCI_REG8(0x3209)
#define SC820CS_REG_OUT_HEIGHT_H	CCI_REG8(0x320a)
#define SC820CS_REG_OUT_HEIGHT_L	CCI_REG8(0x320b)

/* Mirror / flip: mirror bits [2:1], flip bits [6:5]. Same as SC8238. */
#define SC820CS_REG_FLIP_MIRROR		CCI_REG8(0x3221)
#define SC820CS_FETCH_MIRROR(v, en)	((en) ? ((v) | 0x06) : ((v) & 0xf9))
#define SC820CS_FETCH_FLIP(v, en)	((en) ? ((v) | 0x60) : ((v) & 0x9f))

/* Test pattern: 0x4501 BIT(3) enables color bars. */
#define SC820CS_REG_TEST_PATTERN	CCI_REG8(0x4501)
#define SC820CS_TEST_PATTERN_BIT	BIT(3)
#define SC820CS_TEST_PATTERN_DEF_VAL	0xa4

/* PLL clock gates used around PLL reconfiguration in vendor tables */
#define SC820CS_REG_PLL_GATE_ISP	CCI_REG8(0x36e9)
#define SC820CS_REG_PLL_GATE_SENSOR	CCI_REG8(0x36f9)

/* 19.2 MHz MCLK4, per the erhai vendor DT clock-rates property */
#define SC820CS_XCLK_FREQ		19200000

/* 2 data lanes, RAW10: link freq 720 MHz => 1.44 Gbps/lane,
 * pixel rate = 2 lanes * 2 (DDR) * 720 MHz / 10 bit = 288 MHz. */
#define SC820CS_LINK_FREQ		720000000ULL
#define SC820CS_PIXEL_RATE		288000000ULL

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
 * TODO(vendor-regs): the vendor PLL / analog tuning blocks are not
 * available in GPL kernel source for this generation (see file header).
 * The lists below carry the structural registers (window size, HTS/VTS,
 * MIPI lane count, PLL clock gates) with computed values; the PLL divider
 * registers themselves are placeholders following the SC8238 programming
 * model.  Streaming before transplanting the vendor register block must
 * be expected to produce no or invalid frames.  待硬件验证.
 */
static const struct cci_reg_sequence sc820cs_mode_3264x2448_30fps[] = {
	/* PLL clock gates off during (re)configuration */
	{ SC820CS_REG_PLL_GATE_ISP, 0x80 },
	{ SC820CS_REG_PLL_GATE_SENSOR, 0x80 },

	/* TODO(vendor-regs): PLL divider block - placeholder values,
	 * target 1.44 Gbps/lane from a 19.2 MHz MCLK. 待硬件验证. */
	{ CCI_REG8(0x3018), 0x32 },	/* MIPI 2-lane enable */
	{ CCI_REG8(0x3019), 0x00 },
	{ CCI_REG8(0x301f), 0x02 },
	{ CCI_REG8(0x3031), 0x0a },
	{ CCI_REG8(0x3037), 0x20 },
	{ CCI_REG8(0x3038), 0x44 },

	/* Window / timing */
	{ SC820CS_REG_OUT_WIDTH_H, 0x0c },	/* width 3264 */
	{ SC820CS_REG_OUT_WIDTH_L, 0xc0 },
	{ SC820CS_REG_OUT_HEIGHT_H, 0x09 },	/* height 2448 */
	{ SC820CS_REG_OUT_HEIGHT_L, 0x90 },
	{ SC820CS_REG_HTS_H, 0x0e },		/* HTS 3600 */
	{ SC820CS_REG_HTS_L, 0x10 },
	{ SC820CS_REG_VTS_H, 0x0a },		/* VTS 2670 */
	{ SC820CS_REG_VTS_L, 0x6e },

	/* PLL clock gates on */
	{ SC820CS_REG_PLL_GATE_ISP, 0x53 },	/* TODO(vendor-regs) 待硬件验证 */
	{ SC820CS_REG_PLL_GATE_SENSOR, 0x57 },
};

static const struct cci_reg_sequence sc820cs_mode_1920x1080_30fps[] = {
	{ SC820CS_REG_PLL_GATE_ISP, 0x80 },
	{ SC820CS_REG_PLL_GATE_SENSOR, 0x80 },

	/* TODO(vendor-regs): same PLL placeholder as above. 待硬件验证. */
	{ CCI_REG8(0x3018), 0x32 },
	{ CCI_REG8(0x3019), 0x00 },
	{ CCI_REG8(0x301f), 0x02 },
	{ CCI_REG8(0x3031), 0x0a },
	{ CCI_REG8(0x3037), 0x20 },
	{ CCI_REG8(0x3038), 0x44 },

	/* Window / timing (2x2 binned 1080p; binning registers TBD from
	 * vendor table). 待硬件验证. */
	{ SC820CS_REG_OUT_WIDTH_H, 0x07 },	/* width 1920 */
	{ SC820CS_REG_OUT_WIDTH_L, 0x80 },
	{ SC820CS_REG_OUT_HEIGHT_H, 0x04 },	/* height 1080 */
	{ SC820CS_REG_OUT_HEIGHT_L, 0x38 },
	{ SC820CS_REG_HTS_H, 0x0e },		/* HTS 3600 */
	{ SC820CS_REG_HTS_L, 0x10 },
	{ SC820CS_REG_VTS_H, 0x0a },		/* VTS 2670 */
	{ SC820CS_REG_VTS_L, 0x6e },

	{ SC820CS_REG_PLL_GATE_ISP, 0x53 },	/* TODO(vendor-regs) 待硬件验证 */
	{ SC820CS_REG_PLL_GATE_SENSOR, 0x57 },
};

static const struct sc820cs_mode supported_modes[] = {
	{
		/* Full resolution 8MP @ 30fps, RAW10 */
		.width		= 3264,
		.height		= 2448,
		.hts_def	= 3600,
		.vts_def	= 2670,
		.reg_list	= sc820cs_mode_3264x2448_30fps,
		.reg_list_len	= ARRAY_SIZE(sc820cs_mode_3264x2448_30fps),
	},
	{
		/* 1080p (binned) @ 30fps, RAW10 */
		.width		= 1920,
		.height		= 1080,
		.hts_def	= 3600,
		.vts_def	= 2670,
		.reg_list	= sc820cs_mode_1920x1080_30fps,
		.reg_list_len	= ARRAY_SIZE(sc820cs_mode_1920x1080_30fps),
	},
};

static const s64 sc820cs_link_freq_menu[] = {
	SC820CS_LINK_FREQ,
};

/* Bayer order: base BGGR (待硬件验证), rotated by mirror/flip.
 * Order: no flip, hflip, vflip, h+v flip. */
static const u32 sc820cs_mbus_formats[] = {
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
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
	int ret = 0;

	cci_write(sc820cs->regmap, SC820CS_REG_EXPOSURE_H,
		  (exposure >> 12) & 0xf, &ret);
	cci_write(sc820cs->regmap, SC820CS_REG_EXPOSURE_M,
		  (exposure >> 4) & 0xff, &ret);
	cci_write(sc820cs->regmap, SC820CS_REG_EXPOSURE_L,
		  (exposure & 0xf) << 4, &ret);

	return ret;
}

static int sc820cs_set_analogue_gain(struct sc820cs *sc820cs, u32 gain)
{
	u32 coarse, fine;
	int ret = 0;

	/* Coarse/fine split identical to SC8238: the 7-bit fine register
	 * carries gain/128 within the range, the coarse register selects
	 * the binary range. 待硬件验证. */
	if (gain < 0x100) {		/* 1x .. 2x */
		fine = gain & 0x7f;
		coarse = SC820CS_AGAIN_COARSE_1X;
	} else if (gain < 0x200) {	/* 2x .. 4x */
		fine = (gain >> 1) & 0x7f;
		coarse = SC820CS_AGAIN_COARSE_2X;
	} else if (gain < 0x400) {	/* 4x .. 8x */
		fine = (gain >> 2) & 0x7f;
		coarse = SC820CS_AGAIN_COARSE_4X;
	} else {			/* 8x .. 7.94x ceiling */
		fine = (gain >> 3) & 0x7f;
		coarse = SC820CS_AGAIN_COARSE_8X;
	}

	cci_write(sc820cs->regmap, SC820CS_REG_AGAIN_COARSE, coarse, &ret);
	cci_write(sc820cs->regmap, SC820CS_REG_AGAIN_FINE, fine, &ret);

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
			       SC820CS_EXPOSURE_OFFSET;
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
		cci_write(sc820cs->regmap, SC820CS_REG_VTS_H,
			  (val >> 8) & 0x7f, &ret);
		cci_write(sc820cs->regmap, SC820CS_REG_VTS_L,
			  val & 0xff, &ret);
		break;
	case V4L2_CID_HBLANK:
		val = format->width + ctrl->val;
		cci_write(sc820cs->regmap, SC820CS_REG_HTS_H,
			  (val >> 8) & 0xff, &ret);
		cci_write(sc820cs->regmap, SC820CS_REG_HTS_L,
			  val & 0xff, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		val = ctrl->val ? SC820CS_TEST_PATTERN_BIT : 0;
		cci_update_bits(sc820cs->regmap, SC820CS_REG_TEST_PATTERN,
				SC820CS_TEST_PATTERN_BIT, val, &ret);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
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

	sc820cs->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &sc820cs_ctrl_ops,
					    V4L2_CID_VBLANK,
					    SC820CS_VBLANK_MIN,
					    SC820CS_VTS_MAX - mode->height, 1,
					    mode->vts_def - mode->height);

	sc820cs->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &sc820cs_ctrl_ops,
					    V4L2_CID_HBLANK,
					    SC820CS_HTS_MIN - mode->width,
					    SC820CS_VTS_MAX - mode->width, 1,
					    mode->hts_def - mode->width);

	exposure_max = mode->vts_def - SC820CS_EXPOSURE_OFFSET;
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

	/* Mode register list (window/PLL/MIPI). See TODO(vendor-regs). */
	ret = cci_multi_reg_write(sc820cs->regmap, sc820cs->mode->reg_list,
				  sc820cs->mode->reg_list_len, NULL);
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
		exposure_max = mode->vts_def - SC820CS_EXPOSURE_OFFSET;
		exposure_def = min_t(int, exposure_max,
				     SC820CS_EXPOSURE_DEFAULT);

		__v4l2_ctrl_modify_range(sc820cs->vblank, SC820CS_VBLANK_MIN,
					 SC820CS_VTS_MAX - mode->height, 1,
					 mode->vts_def - mode->height);
		__v4l2_ctrl_s_ctrl(sc820cs->vblank,
				   mode->vts_def - mode->height);

		__v4l2_ctrl_modify_range(sc820cs->hblank,
					 SC820CS_HTS_MIN - mode->width,
					 SC820CS_VTS_MAX - mode->width, 1,
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
		/* Full active array; the exact array offset is unknown
		 * without the vendor register table. 待硬件验证. */
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

	return sc820cs_set_pad_format(sd, state, &fmt);
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
 * Power-up sequence. The vendor camx power sequence lives in the
 * userspace sensor module; the delays below follow the SmartSens
 * convention (SC8238 driver: 1-2 ms after reset release, 10-20 ms
 * before the first I2C access). Order: reset asserted -> supplies
 * (dovdd, avdd, dvdd) -> MCLK -> reset deasserted. 待硬件验证.
 */
#define SC820CS_POWER_SETTLE_US		5000	/* supplies settle */
#define SC820CS_MCLK_SETTLE_US		1000	/* MCLK before reset release */
#define SC820CS_RESET_DELAY_US		20000	/* reset to first I2C */

static int sc820cs_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sc820cs *sc820cs = to_sc820cs(sd);
	int ret;

	/* Hold the sensor in reset while powering up */
	gpiod_set_value_cansleep(sc820cs->reset_gpio, 0);

	ret = regulator_bulk_enable(SC820CS_NUM_SUPPLIES, sc820cs->supplies);
	if (ret) {
		dev_err(dev, "%s: failed to enable regulators\n", __func__);
		return ret;
	}

	usleep_range(SC820CS_POWER_SETTLE_US,
		     SC820CS_POWER_SETTLE_US + 1000);

	ret = clk_prepare_enable(sc820cs->xclk);
	if (ret) {
		dev_err(dev, "%s: failed to enable clock\n", __func__);
		goto reg_off;
	}

	usleep_range(SC820CS_MCLK_SETTLE_US,
		     SC820CS_MCLK_SETTLE_US + 1000);

	/* Release reset */
	gpiod_set_value_cansleep(sc820cs->reset_gpio, 1);
	usleep_range(SC820CS_RESET_DELAY_US,
		     SC820CS_RESET_DELAY_US + 1000);

	return 0;

reg_off:
	regulator_bulk_disable(SC820CS_NUM_SUPPLIES, sc820cs->supplies);

	return ret;
}

static int sc820cs_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sc820cs *sc820cs = to_sc820cs(sd);

	gpiod_set_value_cansleep(sc820cs->reset_gpio, 0);
	regulator_bulk_disable(SC820CS_NUM_SUPPLIES, sc820cs->supplies);
	clk_disable_unprepare(sc820cs->xclk);

	return 0;
}

/* -----------------------------------------------------------------------------
 * Probe & remove
 */

static int sc820cs_identify_module(struct sc820cs *sc820cs)
{
	struct i2c_client *client = v4l2_get_subdevdata(&sc820cs->sd);
	int ret;
	u64 val;

	ret = cci_read(sc820cs->regmap, SC820CS_REG_CHIP_ID, &val, NULL);
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

	/* Only the 2-lane configuration is validated against real mode
	 * timings; the erhai front module is 2-lane per vendor DT usage. */
	if (ep_cfg.bus.mipi_csi2.num_data_lanes != 2) {
		dev_err_probe(dev, -EINVAL,
			      "only 2 data lanes are supported\n");
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
	int ret;

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

	ret = devm_regulator_bulk_get(dev, SC820CS_NUM_SUPPLIES,
				      sc820cs->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	sc820cs->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						      GPIOD_OUT_LOW);
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
MODULE_DESCRIPTION("SmartSens SC820CS sensor driver");
MODULE_LICENSE("GPL v2");
