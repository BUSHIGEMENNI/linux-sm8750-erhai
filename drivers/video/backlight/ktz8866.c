// SPDX-License-Identifier: GPL-2.0-only
/*
 * Backlight driver for the Kinetic KTZ8866/KTZ8868
 *
 * Copyright (C) 2022, 2023 Jianhua Lu <lujianhua000@gmail.com>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define DEFAULT_BRIGHTNESS 1500
#define MAX_BRIGHTNESS 2047
#define NUM_SINKS 6
#define REG_MAX 0x15

/* reg */
#define DEVICE_ID 0x01
#define KTZ8868_BL_CTRL 0x01	/* KTZ8868: global backlight on/off */
#define BL_CFG1 0x02
#define BL_CFG2 0x03
#define BL_BRT_LSB 0x04
#define BL_BRT_MSB 0x05
#define BL_EN 0x08
#define LCD_BIAS_CFG1 0x09
#define LCD_BIAS_CFG2 0x0A
#define LCD_BIAS_CFG3 0x0B
#define LCD_BOOST_CFG 0x0C
#define OUTP_CFG 0x0D
#define OUTN_CFG 0x0E
#define FLAG 0x0F
#define BL_OPTION1 0x10
#define BL_OPTION2 0x11
#define PWM2DIG_LSBs 0x12
#define PWM2DIG_MSBs 0x13
#define BL_DIMMING 0x14
#define PWM_RAMP_TIME 0x15

/* definition */
#define BL_EN_BIT BIT(6)
#define LCD_BIAS_EN 0x9F
#define PWM_HYST 0x5

/*
 * KTZ8868: the LED driver is not live until KTZ8868_BL_CTRL is written after
 * the sink enable + brightness (the sink bits alone leave the string dark),
 * and the LCD bias outputs are enabled through the ENP/ENN pins rather than
 * LCD_BIAS_CFG1 bits. Values below follow the KTZ8868 reference sequence
 * (OVP 34V, linear dimming, 4.7uH/2.5A boost, 30mA/channel, sinks 1-3/5-6).
 */
#define KTZ8868_BL_CFG1_VAL 0xB3
#define KTZ8868_BL_CFG2_VAL 0xEB
#define KTZ8868_BL_OPTION2_VAL 0x76
#define KTZ8868_PWM_RAMP_TIME_VAL 0xF8
#define KTZ8868_BL_EN_VAL 0x77
#define KTZ8868_LCD_BOOST_CFG_VAL 0x28
#define KTZ8868_OUTP_CFG_VAL 0x1E	/* +6.0V */
#define KTZ8868_OUTN_CFG_VAL 0x1E	/* -6.0V */
#define KTZ8868_LCD_BIAS_CFG1_VAL 0x99	/* bias enable from ENP/ENN pins */

enum ktz8866_variant {
	KTZ8866,
	KTZ8868,
};

struct ktz8866 {
	struct i2c_client *client;
	struct regmap *regmap;
	bool led_on;
	struct gpio_descs *enable_gpios;
	enum ktz8866_variant variant;
	struct backlight_device *backlight;
	struct led_classdev led;
	unsigned int bl_en;
};

static const struct regmap_config ktz8866_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = REG_MAX,
};

static int ktz8866_write(struct ktz8866 *ktz, unsigned int reg,
			 unsigned int val)
{
	return regmap_write(ktz->regmap, reg, val);
}

static int ktz8866_update_bits(struct ktz8866 *ktz, unsigned int reg,
			       unsigned int mask, unsigned int val)
{
	return regmap_update_bits(ktz->regmap, reg, mask, val);
}

static void ktz8866_set_brightness(struct ktz8866 *ktz, unsigned int brightness)
{
	ktz8866_write(ktz, BL_BRT_LSB, brightness & 0x7);
	ktz8866_write(ktz, BL_BRT_MSB, (brightness >> 3) & 0xFF);
}

static int ktz8868_update_brightness(struct ktz8866 *ktz,
				     unsigned int brightness)
{
	if (brightness == 0) {
		if (ktz->led_on) {
			ktz8866_write(ktz, KTZ8868_BL_CTRL, 0);
			msleep(9);
		}
		ktz8866_set_brightness(ktz, 0);
		if (ktz->led_on) {
			ktz8866_write(ktz, BL_EN, 0);
			ktz->led_on = false;
		}
		return 0;
	}

	if (!ktz->led_on) {
		ktz8866_write(ktz, BL_CFG1, KTZ8868_BL_CFG1_VAL);
		ktz8866_write(ktz, BL_CFG2, KTZ8868_BL_CFG2_VAL);
		ktz8866_write(ktz, BL_OPTION2, KTZ8868_BL_OPTION2_VAL);
		ktz8866_write(ktz, PWM_RAMP_TIME, KTZ8868_PWM_RAMP_TIME_VAL);
		ktz8866_write(ktz, BL_EN, ktz->bl_en);
	}

	ktz8866_set_brightness(ktz, brightness);

	if (!ktz->led_on) {
		msleep(15);
		ktz8866_write(ktz, KTZ8868_BL_CTRL, 1);
		ktz->led_on = true;
	}

	return 0;
}

static int ktz8866_backlight_update_status(struct backlight_device *backlight_dev)
{
	struct ktz8866 *ktz = bl_get_data(backlight_dev);
	unsigned int brightness = backlight_get_brightness(backlight_dev);

	if (ktz->variant == KTZ8868)
		return ktz8868_update_brightness(ktz, brightness);

	if (!ktz->led_on && brightness > 0) {
		ktz8866_update_bits(ktz, BL_EN, BL_EN_BIT, BL_EN_BIT);
		ktz->led_on = true;
	} else if (brightness == 0) {
		ktz8866_update_bits(ktz, BL_EN, BL_EN_BIT, 0);
		ktz->led_on = false;
	}

	ktz8866_set_brightness(ktz, brightness);

	return 0;
}

static const struct backlight_ops ktz8866_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = ktz8866_backlight_update_status,
};

static int ktz8868_led_brightness_set(struct led_classdev *led,
				      enum led_brightness brightness)
{
	struct ktz8866 *ktz = container_of(led, struct ktz8866, led);

	return ktz8868_update_brightness(ktz, brightness);
}

/*
 * A KTZ8868 with a led child node exposes its LED string as an LED class
 * device, so that several chips lighting one panel can be combined into a
 * single backlight by led-backlight. led-sources selects the current sinks.
 */
static int ktz8868_register_led(struct ktz8866 *ktz,
				struct fwnode_handle *child)
{
	struct device *dev = &ktz->client->dev;
	struct led_init_data init_data = { .fwnode = child };
	u32 sources[NUM_SINKS];
	int count, i, ret;

	count = fwnode_property_count_u32(child, "led-sources");
	if (count < 1 || count > NUM_SINKS)
		return dev_err_probe(dev, -EINVAL, "invalid led-sources\n");

	ret = fwnode_property_read_u32_array(child, "led-sources", sources,
					     count);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read led-sources\n");

	ktz->bl_en = BL_EN_BIT;
	for (i = 0; i < count; i++) {
		if (sources[i] >= NUM_SINKS)
			return dev_err_probe(dev, -EINVAL,
					     "invalid LED sink %u\n", sources[i]);
		ktz->bl_en |= BIT(sources[i]);
	}

	ktz->led.max_brightness = MAX_BRIGHTNESS;
	ktz->led.brightness_set_blocking = ktz8868_led_brightness_set;
	ktz->led.flags = LED_CORE_SUSPENDRESUME;

	ret = devm_led_classdev_register_ext(dev, &ktz->led, &init_data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register LED\n");

	return 0;
}

static void ktz8866_init(struct ktz8866 *ktz)
{
	unsigned int val = 0;

	if (!of_property_read_u32(ktz->client->dev.of_node, "current-num-sinks", &val))
		ktz8866_write(ktz, BL_EN, BIT(val) - 1);
	else
		/* Enable all 6 current sinks if the number of current sinks isn't specified. */
		ktz8866_write(ktz, BL_EN, BIT(6) - 1);

	if (!of_property_read_u32(ktz->client->dev.of_node, "kinetic,current-ramp-delay-ms", &val)) {
		if (val <= 128)
			ktz8866_write(ktz, BL_CFG2, BIT(7) | (ilog2(val) << 3) | PWM_HYST);
		else
			ktz8866_write(ktz, BL_CFG2, BIT(7) | ((5 + val / 64) << 3) | PWM_HYST);
	}

	if (!of_property_read_u32(ktz->client->dev.of_node, "kinetic,led-enable-ramp-delay-ms", &val)) {
		if (val == 0)
			ktz8866_write(ktz, BL_DIMMING, 0);
		else {
			unsigned int ramp_off_time = ilog2(val) + 1;
			unsigned int ramp_on_time = ramp_off_time << 4;
			ktz8866_write(ktz, BL_DIMMING, ramp_on_time | ramp_off_time);
		}
	}

	if (of_property_read_bool(ktz->client->dev.of_node, "kinetic,enable-lcd-bias"))
		ktz8866_write(ktz, LCD_BIAS_CFG1, LCD_BIAS_EN);
}

/*
 * KTZ8868 power-up: HWEN, then the bias configuration over I2C, then the
 * ENP/ENN pins in that order with the settling delays the part needs. The
 * enable-gpios array is HWEN, ENP, ENN; a design that wires only HWEN gets
 * the same sequence minus the pin-driven bias enable.
 */
static int ktz8868_power_up(struct ktz8866 *ktz)
{
	struct gpio_descs *gpios = ktz->enable_gpios;
	bool lcd_bias = device_property_read_bool(&ktz->client->dev,
						  "kinetic,enable-lcd-bias");
	int ret;

	if (!gpios)
		return 0;

	ret = gpiod_direction_output(gpios->desc[0], 1);
	if (ret)
		return ret;
	usleep_range(1000, 1500);

	if (lcd_bias) {
		ktz8866_write(ktz, LCD_BOOST_CFG, KTZ8868_LCD_BOOST_CFG_VAL);
		ktz8866_write(ktz, OUTP_CFG, KTZ8868_OUTP_CFG_VAL);
		ktz8866_write(ktz, OUTN_CFG, KTZ8868_OUTN_CFG_VAL);
		ktz8866_write(ktz, LCD_BIAS_CFG1, KTZ8868_LCD_BIAS_CFG1_VAL);
	}

	if (gpios->ndescs < 3)
		return 0;

	usleep_range(1000, 1500);
	ret = gpiod_direction_output(gpios->desc[1], 1);	/* ENP */
	if (ret)
		return ret;
	msleep(8);
	ret = gpiod_direction_output(gpios->desc[2], 1);	/* ENN */
	if (ret)
		return ret;
	msleep(10);

	return 0;
}

static int ktz8866_probe(struct i2c_client *client)
{
	struct fwnode_handle *led_node __free(fwnode_handle) = NULL;
	struct backlight_device *backlight_dev;
	struct backlight_properties props;
	struct ktz8866 *ktz;
	int ret = 0;

	ktz = devm_kzalloc(&client->dev, sizeof(*ktz), GFP_KERNEL);
	if (!ktz)
		return -ENOMEM;

	ktz->client = client;
	ktz->variant = (uintptr_t)i2c_get_match_data(client);
	ktz->regmap = devm_regmap_init_i2c(client, &ktz8866_regmap_config);
	if (IS_ERR(ktz->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(ktz->regmap), "failed to init regmap\n");

	ret = devm_regulator_get_enable(&client->dev, "vddpos");
	if (ret)
		return dev_err_probe(&client->dev, ret, "get regulator vddpos failed\n");
	ret = devm_regulator_get_enable(&client->dev, "vddneg");
	if (ret)
		return dev_err_probe(&client->dev, ret, "get regulator vddneg failed\n");

	ktz->enable_gpios = devm_gpiod_get_array_optional(&client->dev, "enable",
			ktz->variant == KTZ8868 ? GPIOD_ASIS : GPIOD_OUT_HIGH);
	if (IS_ERR(ktz->enable_gpios))
		return PTR_ERR(ktz->enable_gpios);

	if (ktz->variant == KTZ8868) {
		ktz->bl_en = KTZ8868_BL_EN_VAL;
		led_node = device_get_named_child_node(&client->dev, "led");

		ret = ktz8868_power_up(ktz);
		if (ret)
			return dev_err_probe(&client->dev, ret, "failed to power up\n");
	}

	i2c_set_clientdata(client, ktz);

	if (led_node)
		return ktz8868_register_led(ktz, led_node);

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = MAX_BRIGHTNESS;
	props.brightness = DEFAULT_BRIGHTNESS;
	of_property_read_u32(client->dev.of_node, "default-brightness",
			     &props.brightness);
	props.scale = BACKLIGHT_SCALE_LINEAR;

	backlight_dev = devm_backlight_device_register(&client->dev, "ktz8866-backlight",
					&client->dev, ktz, &ktz8866_backlight_ops, &props);
	if (IS_ERR(backlight_dev))
		return dev_err_probe(&client->dev, PTR_ERR(backlight_dev),
				"failed to register backlight device\n");

	if (ktz->variant == KTZ8866)
		ktz8866_init(ktz);

	ktz->backlight = backlight_dev;
	backlight_update_status(backlight_dev);

	return 0;
}

static void ktz8866_remove(struct i2c_client *client)
{
	struct ktz8866 *ktz = i2c_get_clientdata(client);

	/* The LED core switches a registered LED off when it is unregistered. */
	if (!ktz->backlight)
		return;

	ktz->backlight->props.brightness = 0;
	backlight_update_status(ktz->backlight);
}

static const struct i2c_device_id ktz8866_ids[] = {
	{ .name = "ktz8866", .driver_data = KTZ8866 },
	{ .name = "ktz8868", .driver_data = KTZ8868 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ktz8866_ids);

static const struct of_device_id ktz8866_match_table[] = {
	{ .compatible = "kinetic,ktz8866", .data = (void *)KTZ8866 },
	{ .compatible = "kinetic,ktz8868", .data = (void *)KTZ8868 },
	{},
};
MODULE_DEVICE_TABLE(of, ktz8866_match_table);

static struct i2c_driver ktz8866_driver = {
	.driver = {
		.name = "ktz8866",
		.of_match_table = ktz8866_match_table,
	},
	.probe = ktz8866_probe,
	.remove = ktz8866_remove,
	.id_table = ktz8866_ids,
};

module_i2c_driver(ktz8866_driver);

MODULE_DESCRIPTION("Kinetic KTZ8866/KTZ8868 Backlight Driver");
MODULE_AUTHOR("Jianhua Lu <lujianhua000@gmail.com>");
MODULE_LICENSE("GPL");
