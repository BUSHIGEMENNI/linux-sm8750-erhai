// SPDX-License-Identifier: GPL-2.0-only
/*
 * OnePlus Pad 2 Pro magnetic pogo-pin keyboard (tinno) mainline driver.
 *
 * The keyboard MCU sits on a UART (serdev) and speaks a small framed
 * protocol reverse-engineered from the vendor driver
 * (oplus/kernel/device_info/pogo_keyboard):
 *
 *   frame = 0x55*sync... 0xF1 <addr> <addr2> <cmd> <payload...> 0xFE 0xAA 0xAA
 *
 *   addr  0xA1 = keyboard, 0xA2 = pad (tablet)
 *   cmd   0x01 = general key report (8-byte USB-HID boot-protocol report)
 *         0x02 = multimedia key report
 *         0x03 = touchpad report
 *         0x2C/0x2D low-power, 0x3A/0x3B user-general, ...
 *
 * The keyboard sends standard USB HID boot-protocol key reports, so the key
 * table below mirrors usbkbd.c's HID-usage -> Linux-keycode mapping.
 */

#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/serdev.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define PK_HEAD_SYNC		0x55
#define PK_START		0xf1
#define PK_END			0xfe
#define PK_TAIL_SYNC		0xaa
#define PK_ADDR_KEYBOARD	0xa1
#define PK_ADDR_PAD		0xa2

#define PK_CMD_KEY		0x01
#define PK_CMD_MEDIA_KEY	0x02
#define PK_CMD_TOUCHPAD		0x03
#define PK_CMD_USER_GENERAL	0x3a
#define PK_CMD_USER_GENERAL_ACK	0x3b

#define PK_TOUCH_FINGERS	5
#define PK_BAUD			1500000
#define PK_RX_MAX		256

/* USB-HID usage -> Linux keycode (mirrors usbkbd.c) */
static const unsigned char pk_keycode[256] = {
	  0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
	 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
	  4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
	 27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
	 65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
	105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
	 72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
	191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
	115,114,  0,  0,  0,121,  0, 89, 93,124, 92, 94, 95,  0,  0,  0,
	122,123, 90, 91, 85,  0,  0,  0,  0,  0,  0,  0,111,  0,  0,  0,
	/* 160..255 mostly unknown on this keyboard; vendor table tail: */
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,179,180,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,111,  0,  0,  0,  0,  0,  0,  0,
	 29, 42, 56,125, 97, 54,100,126,164,166,165,163,161,115,114,113,
	150,158,159,128,136,177,178,176,142,152,173,140,  0,  0,  0,  0,
};

struct pk_dev {
	struct serdev_device *serdev;
	struct input_dev *kbd;
	struct input_dev *pad;

	/* frame reassembly */
	u8 rx[PK_RX_MAX];
	int rx_len;
	bool rx_active;
	int sync_cnt;

	/* key state (6-key rollover HID report) */
	u8 old_keys[8];

	unsigned int tp_x_max;
	unsigned int tp_y_max;
};

static void pk_report_keys(struct pk_dev *pk, const u8 *rpt)
{
	struct input_dev *dev = pk->kbd;
	int i;

	/* byte 0: modifier bits -> usages 224..231 */
	for (i = 0; i < 8; i++)
		input_report_key(dev, pk_keycode[i + 224], (rpt[0] >> i) & 1);

	/* bytes 2..7: up to 6 simultaneous keycodes */
	for (i = 2; i < 8; i++) {
		/* released: in old but not in new */
		if (pk->old_keys[i] > 3 && !memchr(rpt + 2, pk->old_keys[i], 6) &&
		    pk_keycode[pk->old_keys[i]])
			input_report_key(dev, pk_keycode[pk->old_keys[i]], 0);
		/* pressed: in new but not in old */
		if (rpt[i] > 3 && !memchr(pk->old_keys + 2, rpt[i], 6) &&
		    pk_keycode[rpt[i]])
			input_report_key(dev, pk_keycode[rpt[i]], 1);
	}
	input_sync(dev);
	memcpy(pk->old_keys, rpt, 8);
}

static void pk_report_touch(struct pk_dev *pk, const u8 *buf, int len)
{
	struct input_dev *dev = pk->pad;
	int fingers, j, count = 0;
	u8 btn;

	if (len < 2)
		return;
	fingers = buf[len - 1];
	if (fingers > PK_TOUCH_FINGERS || 1 + fingers * 5 + 1 > len)
		return;

	btn = buf[len] & 0x03; /* bit0 left, bit1 right (vendor reads data[len-1]) */
	for (j = 0; j < fingers; j++) {
		const u8 *f = &buf[1 + j * 5];
		int id = (f[0] >> 4) & 0xf;
		bool down = (f[0] >> 1) & 0x01;
		int x = f[1] | (f[2] << 8);
		int y = f[3] | (f[4] << 8);

		input_mt_slot(dev, id);
		if (input_mt_report_slot_state(dev, MT_TOOL_FINGER, down)) {
			input_report_abs(dev, ABS_MT_POSITION_X, x);
			input_report_abs(dev, ABS_MT_POSITION_Y, y);
			count++;
		}
	}
	input_report_key(dev, BTN_TOUCH, count > 0);
	input_mt_sync_frame(dev);
	input_sync(dev);
}

static void pk_handle_frame(struct pk_dev *pk, const u8 *data, int len)
{
	u8 cmd;

	if (len < 1)
		return;
	cmd = data[0];
	switch (cmd) {
	case PK_CMD_KEY:
		if (len >= 9)
			pk_report_keys(pk, &data[1]);
		break;
	case PK_CMD_TOUCHPAD:
		if (len >= 2)
			pk_report_touch(pk, &data[1], len - 1);
		break;
	default:
		dev_dbg(&pk->serdev->dev, "unhandled cmd 0x%02x len %d\n", cmd, len);
		break;
	}
}

static size_t pk_receive(struct serdev_device *serdev, const u8 *buf, size_t len)
{
	struct pk_dev *pk = serdev_device_get_drvdata(serdev);
	size_t i;

	for (i = 0; i < len; i++) {
		u8 b = buf[i];

		if (b == PK_HEAD_SYNC) {
			pk->sync_cnt++;
			continue;
		}
		if (b == PK_START && pk->sync_cnt >= 4) {
			pk->rx_active = true;
			pk->rx_len = 0;
		}
		pk->sync_cnt = 0;

		if (!pk->rx_active)
			continue;

		if (pk->rx_len < PK_RX_MAX)
			pk->rx[pk->rx_len++] = b;

		/* complete frame: ... 0xFE 0xAA 0xAA */
		if (pk->rx_len >= 3 &&
		    pk->rx[pk->rx_len - 3] == PK_END &&
		    pk->rx[pk->rx_len - 2] == PK_TAIL_SYNC &&
		    pk->rx[pk->rx_len - 1] == PK_TAIL_SYNC) {
			/* strip start+addr1+addr2 and end+sync+sync */
			int dlen = pk->rx_len - 6;

			pk->rx_active = false;
			if (dlen > 0)
				pk_handle_frame(pk, &pk->rx[3], dlen);
			pk->rx_len = 0;
		}
	}
	return len;
}

static const struct serdev_device_ops pk_serdev_ops = {
	.receive_buf = pk_receive,
	.write_wakeup = serdev_device_write_wakeup,
};

static int pk_probe(struct serdev_device *serdev)
{
	struct pk_dev *pk;
	int ret;

	pk = devm_kzalloc(&serdev->dev, sizeof(*pk), GFP_KERNEL);
	if (!pk)
		return -ENOMEM;
	pk->serdev = serdev;
	pk->tp_x_max = 1400;
	pk->tp_y_max = 900;
	serdev_device_set_drvdata(serdev, pk);

	ret = serdev_device_open(serdev);
	if (ret)
		return dev_err_probe(&serdev->dev, ret, "serdev open failed\n");
	serdev_device_set_baudrate(serdev, PK_BAUD);
	serdev_device_set_flow_control(serdev, false);
	serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);

	pk->kbd = devm_input_allocate_device(&serdev->dev);
	if (!pk->kbd) {
		ret = -ENOMEM;
		goto err_close;
	}
	pk->kbd->name = "OnePlus pogo keyboard";
	pk->kbd->phys = "pogo/serdev0";
	pk->kbd->id.bustype = BUS_RS232;
	__set_bit(EV_KEY, pk->kbd->evbit);
	__set_bit(EV_REP, pk->kbd->evbit);
	for (ret = 0; ret < 256; ret++)
		if (pk_keycode[ret])
			__set_bit(pk_keycode[ret], pk->kbd->keybit);
	ret = input_register_device(pk->kbd);
	if (ret) {
		dev_err(&serdev->dev, "kbd register failed: %d\n", ret);
		goto err_close;
	}

	pk->pad = devm_input_allocate_device(&serdev->dev);
	if (!pk->pad) {
		ret = -ENOMEM;
		goto err_close;
	}
	pk->pad->name = "OnePlus pogo touchpad";
	pk->pad->phys = "pogo/serdev1";
	pk->pad->id.bustype = BUS_RS232;
	input_set_abs_params(pk->pad, ABS_MT_POSITION_X, 0, pk->tp_x_max, 0, 0);
	input_set_abs_params(pk->pad, ABS_MT_POSITION_Y, 0, pk->tp_y_max, 0, 0);
	input_set_capability(pk->pad, EV_KEY, BTN_TOUCH);
	input_set_capability(pk->pad, EV_KEY, BTN_MOUSE);
	input_set_capability(pk->pad, EV_KEY, BTN_RIGHT);
	ret = input_mt_init_slots(pk->pad, PK_TOUCH_FINGERS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		goto err_close;
	ret = input_register_device(pk->pad);
	if (ret) {
		dev_err(&serdev->dev, "touchpad register failed: %d\n", ret);
		goto err_close;
	}

	dev_info(&serdev->dev, "pogo keyboard registered\n");
	return 0;

err_close:
	serdev_device_close(serdev);
	return ret;
}

static const struct of_device_id pk_of_match[] = {
	{ .compatible = "oneplus,pogo-keyboard" },
	{ }
};
MODULE_DEVICE_TABLE(of, pk_of_match);

static struct serdev_device_driver pk_driver = {
	.probe = pk_probe,
	.driver = {
		.name = "oneplus-pogo-keyboard",
		.of_match_table = pk_of_match,
	},
	.ops = &pk_serdev_ops,
};
module_serdev_device_driver(pk_driver);

MODULE_DESCRIPTION("OnePlus Pad 2 Pro pogo-pin keyboard/touchpad driver");
MODULE_LICENSE("GPL");
