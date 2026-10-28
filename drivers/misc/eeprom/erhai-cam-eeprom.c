// SPDX-License-Identifier: GPL-2.0
/*
 * erhai-cam-eeprom - rear camera module calibration EEPROM (OPLUS erhai)
 *
 * The rear camera module carries a small I2C EEPROM on the CCI bus holding
 * the module id string ("001QC5324T12GE1" at 0x08 in the unit this was
 * written for) and calibration tables; ~0xb28 bytes are populated, the rest
 * of the 4 KiB window reads back 0xff.
 *
 * The Qualcomm CCI adapter behind this bus rejects I2C messages longer than
 * 8 bytes (-EOPNOTSUPP on 16-byte reads, measured 2026-10-28), so every
 * access is chunked to 8 data bytes.  The device takes a 16-bit address
 * written as two bytes before the data phase.
 *
 * Exposed as an nvmem device; userspace reads
 * /sys/bus/nvmem/devices/erhai-cam-eeprom0/nvmem.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/property.h>

#define ERHAI_EE_CHUNK		8
#define ERHAI_EE_SIZE_DEFAULT	4096

static int erhai_eeprom_reg_read(void *priv, unsigned int off,
				 void *val, size_t bytes)
{
	struct i2c_client *client = priv;
	u8 *buf = val;
	int ret;

	while (bytes) {
		u8 addr_buf[2] = { off >> 8, off & 0xff };
		size_t n = min(bytes, (size_t)ERHAI_EE_CHUNK);
		struct i2c_msg msgs[2] = {
			{ .addr = client->addr, .flags = 0,
			  .len = sizeof(addr_buf), .buf = addr_buf },
			{ .addr = client->addr, .flags = I2C_M_RD,
			  .len = n, .buf = buf },
		};

		ret = i2c_transfer(client->adapter, msgs, 2);
		if (ret != 2)
			return ret < 0 ? ret : -EIO;

		buf += n;
		off += n;
		bytes -= n;
	}

	return 0;
}

static int erhai_eeprom_probe(struct i2c_client *client)
{
	struct nvmem_config config = {};
	struct nvmem_device *nvmem;
	u32 size = ERHAI_EE_SIZE_DEFAULT;

	device_property_read_u32(&client->dev, "size", &size);

	config.name = "erhai-cam-eeprom";
	config.owner = THIS_MODULE;
	config.dev = &client->dev;
	config.size = size;
	config.word_size = 1;
	config.stride = 1;
	config.read_only = true;
	config.priv = client;
	config.reg_read = erhai_eeprom_reg_read;

	nvmem = devm_nvmem_register(&client->dev, &config);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	dev_info(&client->dev, "%u byte camera module EEPROM\n", size);

	return 0;
}

static const struct i2c_device_id erhai_eeprom_id[] = {
	{ "erhai-cam-eeprom" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, erhai_eeprom_id);

static const struct of_device_id erhai_eeprom_of_match[] = {
	{ .compatible = "oplus,erhai-cam-eeprom" },
	{ }
};
MODULE_DEVICE_TABLE(of, erhai_eeprom_of_match);

static struct i2c_driver erhai_eeprom_driver = {
	.driver = {
		.name = "erhai-cam-eeprom",
		.of_match_table = erhai_eeprom_of_match,
	},
	.probe = erhai_eeprom_probe,
	.id_table = erhai_eeprom_id,
};
module_i2c_driver(erhai_eeprom_driver);

MODULE_AUTHOR("yading");
MODULE_DESCRIPTION("OPLUS erhai rear camera module calibration EEPROM");
MODULE_LICENSE("GPL");
