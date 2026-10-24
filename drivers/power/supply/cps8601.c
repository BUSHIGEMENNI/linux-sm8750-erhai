// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the CPS8601 wireless pen charger found on the OnePlus Pad 2
 * Pro (erhai, SM8750).
 *
 * The chip is an 8051-core MCU driving the inductive TX coil that charges
 * the capacitive stylus. It sits behind a geni i2c-master-hub SE (0x98c000)
 * at I2C address 0x41 with five TLMM control lines. Register access is
 * plain 8-bit regmap, big-endian word assembly.
 *
 * Register map, IRQ bits, power-on/off sequences and the RX data-frame
 * protocol were reverse engineered from the vendor module
 * oplus_wireless_pen.ko (GPL v2, "Wireless Pen Charger Cps8601") extracted
 * from the stock vendor_dlkm; see ~/erhai-pen/disasm-keyfuncs.txt. The
 * vendor driver continues initialization when the optional SRAM update
 * image is missing, so this port relies on the factory firmware and does
 * not require wireless_pen/0/cps8601_firmware.bin.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/pm_wakeup.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/soc/qcom/pmic_glink.h>
#include "erhai_otg_glink.h"

#define CPS8601_REG_CHIP_ID		0x00	/* 2B, expect 0x8601 */
#define CPS8601_REG_FW_VER		0x02	/* 2B */
#define CPS8601_REG_IRQ_CLEAR		0x05	/* 2B, write 0xffff */
#define CPS8601_REG_IRQ_STATUS		0x07	/* 2B */
#define CPS8601_REG_IRQ_ACK		0x09	/* 2B, write back status */
#define CPS8601_REG_CTRL			0x0b	/* 1B, RMW command bits */
#define CPS8601_REG_PING_CNT		0x10	/* 1B */
#define CPS8601_REG_PING_WIDTH		0x11	/* 2B */
#define CPS8601_REG_OCP_TH			0x28	/* 2B W, DT qcom,ocp-threshold-us */
#define CPS8601_REG_LVP_TH			0x2c	/* 2B W, DT lvp */
#define CPS8601_REG_OVP_TH			0x2a	/* 2B W, DT ovp */
#define CPS8601_REG_FOD_TH			0x2f	/* 2B W, DT fod */
#define CPS8601_REG_TX_VOLTAGE		0x34	/* 2B R */
#define CPS8601_REG_TX_CURRENT		0x38	/* 2B R */
#define CPS8601_REG_CHG_STATUS		0x3e	/* 1B R */
#define CPS8601_REG_RX_FRAME		0x40	/* 11B burst */

#define CPS8601_CHIP_ID			0x8601

/*
 * The pen coil rail is fed by the PMIC hboost converter, gated on the ADSP
 * side: the chip stays dead on I2C until the rail is requested over
 * pmic_glink (stock property qcom,pen_is_supply_by_hboost). Vendor message:
 * owner 0x8011, PMIC_GLINK_REQ_RESP, opcode 0x10007, one voltage code byte
 * computed as (mV - 2000) / 50 - attach voltage 10010 mV -> code 160.
 */
#define CPS8601_HBOOST_OWNER		0x8011
#define CPS8601_HBOOST_OPCODE		0x10007
#define CPS8601_HBOOST_ATTACH_CODE	160	/* 10010 mV */
#define CPS8601_HBOOST_DEFAULT_CODE	100	/* 7000 mV */

struct cps8601_hboost_msg {
	struct pmic_glink_hdr	hdr;
	u8			code;
	u8			reserved[3];
} __packed;

/* IRQ status bits (reg 0x07-0x08), from the vendor handler bit tests */
#define CPS8601_IRQ_Q_CALI		BIT(0)
#define CPS8601_IRQ_PING_OK		BIT(2)
#define CPS8601_IRQ_RX_FRAME		BIT(5)
#define CPS8601_IRQ_CHG_STATE		BIT(6)
#define CPS8601_IRQ_RX_ATTACH		BIT(9)
#define CPS8601_IRQ_RX_DETACH		BIT(10)
#define CPS8601_IRQ_TX_WAKEUP		BIT(12)
#define CPS8601_IRQ_MISLOCATED		BIT(13)

/* chg status anomalies (reg 0x3e) */
#define CPS8601_CHG_ERR_MASK		0xec0

struct cps8601 {
	struct i2c_client	*client;
	struct regmap		*regmap;
	struct device		*dev;

	struct gpio_desc	*int_gpiod;
	struct gpio_desc	*sleep_gpiod;
	struct gpio_desc	*scan_gpiod;
	struct gpio_desc	*boost_en_gpiod;
	struct gpio_desc	*sw_en_gpiod;

	struct work_struct	init_work;
	struct delayed_work	ready_work;
	struct wakeup_source	*ws;

	int			irq;
	bool			irq_armed;
	unsigned int		init_retries;

	u16			fw_version;
	u16			chip_id;
	int			pen_soc;
	bool			present;
	bool			ready;

	u32			ocp_uv;
	u32			ovp_uv;
	u32			lvp_uv;
	u32			fod_uv;
	u16			debug_reg;
};

static int cps8601_hboost_req(struct cps8601 *chip, u8 code)
{
	struct cps8601_hboost_msg msg = { };
	int ret;

	/* ride on the erhai_otg client: a dedicated 0x8011 client never gets
	 * a PDR attachment and blocks in pmic_glink_client_register */
	msg.hdr.owner = cpu_to_le32(CPS8601_HBOOST_OWNER);
	msg.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP);
	msg.hdr.opcode = cpu_to_le32(CPS8601_HBOOST_OPCODE);
	msg.code = code;

	ret = erhai_otg_glink_send_raw(&msg, sizeof(msg));
	if (ret)
		return ret;
	/* the ACK lands in the otg callback; the rail itself settles fast */
	msleep(300);
	return 0;
}

/*
 * Vendor power-on (init_work_func after firmware check):
 *   sw_en high -> wait 2.5 s for the MCU to boot -> poll chip ID
 *   -> read fw version -> write protection thresholds
 *   -> sleep line high, one scan burst, enable IRQ.
 */
static void cps8601_power_on(struct cps8601 *chip)
{
	/*
	 * Vendor order (init_work_func +0x264): raise the boost rail and
	 * the scan line first, then sw_en, settle 2.5 s for the MCU. The
	 * stock DT calls this line "off_state" but the vendor drives it
	 * high for the whole session: it is the boost enable that powers
	 * the MCU, not an off request.
	 */
	gpiod_set_value(chip->boost_en_gpiod, 1);
	gpiod_set_value(chip->scan_gpiod, 1);
	gpiod_set_value(chip->sleep_gpiod, 0);
	gpiod_set_value(chip->sw_en_gpiod, 1);
	msleep(2500);
}

static void cps8601_power_off(struct cps8601 *chip)
{
	/*
	 * Deliberately NOT the vendor shutdown register writes (fod 3000,
	 * ctrl 0x02|0x08|0x10, 0x4b=31, 0x4c=172): they latch the MCU off
	 * and only a real rail power cycle revives it, which the ADSP does
	 * not grant on a warm re-request. Gate the control lines instead;
	 * the MCU idles, and the next probe re-runs the default->attach
	 * rail dance.
	 */
	gpiod_set_value(chip->sw_en_gpiod, 0);
	gpiod_set_value(chip->sleep_gpiod, 0);
	gpiod_set_value(chip->scan_gpiod, 0);
	gpiod_set_value(chip->boost_en_gpiod, 0);
}

static void cps8601_scan(struct cps8601 *chip)
{
	gpiod_set_value(chip->scan_gpiod, 1);
	msleep(50);
	gpiod_set_value(chip->scan_gpiod, 0);
}

static int cps8601_read16(struct cps8601 *chip, u16 reg, u16 *val)
{
	unsigned int v;
	int ret;

	/* multi-byte values are little-endian: the first register carries
	 * the low byte (chip id bytes read back 01,86 -> 0x8601) */
	ret = regmap_read(chip->regmap, reg, &v);
	if (ret)
		return ret;
	*val = v;
	ret = regmap_read(chip->regmap, reg + 1, &v);
	if (ret)
		return ret;
	*val |= v << 8;
	return 0;
}

static int cps8601_write16(struct cps8601 *chip, u16 reg, u16 val)
{
	int ret;

	/* little-endian like the reads */
	ret = regmap_write(chip->regmap, reg, val & 0xff);
	if (ret)
		return ret;
	return regmap_write(chip->regmap, reg + 1, val >> 8);
}

static void cps8601_handle_rx_frame(struct cps8601 *chip)
{
	u8 frame[11];
	unsigned int v;
	int i;

	/* frame FIFO at 0x40..0x4a; head (0x28,0x17) carries the pen SOC */
	for (i = 0; i < 11; i++) {
		if (regmap_read(chip->regmap, CPS8601_REG_RX_FRAME + i, &v))
			return;
		frame[i] = v;
	}

	if (frame[0] == 0x28 && frame[1] == 0x17) {
		chip->pen_soc = frame[2];
		if (chip->pen_soc >= 100)
			dev_info(chip->dev, "pen full\n");
		else
			dev_info(chip->dev, "pen soc %d%%\n", chip->pen_soc);
	} else if (frame[0] == 0x48 && (frame[1] == 0xb6 || frame[1] == 0xc1)) {
		dev_dbg(chip->dev, "mac frame %*ph\n", 11, frame);
	}
}

static irqreturn_t cps8601_irq(int irq, void *data)
{
	struct cps8601 *chip = data;
	u16 status = 0;
	u16 active;

	__pm_stay_awake(chip->ws);

	/* vendor order: clear pending first, then read status, then ack by
	 * writing the status back (cps_comm_reg e3/e4/e5) */
	cps8601_write16(chip, CPS8601_REG_IRQ_CLEAR, 0xffff);
	if (cps8601_read16(chip, CPS8601_REG_IRQ_STATUS, &status)) {
		__pm_relax(chip->ws);
		return IRQ_HANDLED;
	}
	active = status;
	cps8601_write16(chip, CPS8601_REG_IRQ_ACK, status);

	if (active & CPS8601_IRQ_RX_ATTACH) {
		u16 width = 0;

		cps8601_read16(chip, CPS8601_REG_PING_WIDTH, &width);
		chip->present = true;
		dev_info(chip->dev, "pen attached (ping width %d)\n", width);
	}
	if (active & CPS8601_IRQ_RX_DETACH) {
		chip->present = false;
		chip->pen_soc = -1;
		dev_info(chip->dev, "pen removed\n");
	}
	if (active & CPS8601_IRQ_RX_FRAME)
		cps8601_handle_rx_frame(chip);
	if (active & CPS8601_IRQ_CHG_STATE) {
		unsigned int chg = 0;

		regmap_read(chip->regmap, CPS8601_REG_CHG_STATUS, &chg);
		if (chg & CPS8601_CHG_ERR_MASK)
			dev_warn(chip->dev, "charge anomaly, status %#x\n", chg);
		else
			dev_dbg(chip->dev, "charge status %#x\n", chg);
	}
	if (active & CPS8601_IRQ_PING_OK)
		dev_dbg(chip->dev, "ping detected\n");
	if (active & CPS8601_IRQ_MISLOCATED)
		dev_warn(chip->dev, "pen mislocated\n");

	__pm_relax(chip->ws);
	return IRQ_HANDLED;
}

static void cps8601_init_work(struct work_struct *work)
{
	struct cps8601 *chip = container_of(work, struct cps8601, init_work);
	u16 id = 0;
	int ret, tries;

	__pm_stay_awake(chip->ws);

	/* power the coil rail through the ADSP before touching the MCU.
	 * Drop it to code 0 first: a warm rail request is a no-op on the
	 * ADSP side, and an MCU latched in its shutdown registers only
	 * wakes on a real power cycle. */
	ret = cps8601_hboost_req(chip, CPS8601_HBOOST_DEFAULT_CODE);
	if (ret)
		dev_warn(chip->dev, "hboost default failed: %d\n", ret);
	msleep(400);
	ret = cps8601_hboost_req(chip, CPS8601_HBOOST_ATTACH_CODE);
	if (ret)
		dev_warn(chip->dev, "hboost rail request failed: %d\n", ret);
	msleep(500);

	cps8601_power_on(chip);

	for (tries = 0; tries < 5; tries++) {
		ret = cps8601_read16(chip, CPS8601_REG_CHIP_ID, &id);
		if (!ret && id == CPS8601_CHIP_ID)
			break;
		msleep(100);
	}
	if (id != CPS8601_CHIP_ID) {
		dev_warn(chip->dev,
			 "chip id mismatch: %#x (ret %d), retry %u/3 in 15 s\n",
			 id, ret, chip->init_retries + 1);
		if (chip->init_retries++ < 3) {
			gpiod_set_value(chip->sw_en_gpiod, 0);
			gpiod_set_value(chip->scan_gpiod, 0);
			gpiod_set_value(chip->boost_en_gpiod, 0);
			schedule_delayed_work(&chip->ready_work,
					      msecs_to_jiffies(15000));
		}
		goto out_nopm;
	}

	cps8601_read16(chip, CPS8601_REG_FW_VER, &chip->fw_version);

	if (chip->ocp_uv && chip->ocp_uv <= 3000)
		cps8601_write16(chip, CPS8601_REG_OCP_TH, chip->ocp_uv);
	if (chip->lvp_uv && chip->lvp_uv <= 5000)
		cps8601_write16(chip, CPS8601_REG_LVP_TH, chip->lvp_uv);
	if (chip->ovp_uv >= 3000 && chip->ovp_uv <= 20000)
		cps8601_write16(chip, CPS8601_REG_OVP_TH, chip->ovp_uv);
	if (chip->fod_uv)
		cps8601_write16(chip, CPS8601_REG_FOD_TH, chip->fod_uv);

	/* vendor raises the sleep line only after the thresholds are set */
	gpiod_set_value(chip->sleep_gpiod, 1);

	chip->ready = true;
	dev_info(chip->dev, "CPS8601 ready, fw 0x%04x\n", chip->fw_version);

	/* IRQ is armed only once the chip is up: the 2.5 s MCU boot window
	 * would otherwise feed INT glitches into the handler before the
	 * chip can answer, and the vendor driver registers it here too. */
	ret = request_threaded_irq(chip->irq, NULL, cps8601_irq,
				   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				   "cps8601", chip);
	if (ret)
		dev_err(chip->dev, "irq request failed: %d\n", ret);
	else
		chip->irq_armed = true;

	/*
	 * Vendor post-ready sequence (init_work_func +0x5a4..): drop the
	 * boost line, then after 1.25 s release sw_en and pulse scan 0->1
	 * with a 50 ms gap. Holding sw_en/boost high (as the bring-up v1
	 * did) leaves the MCU in its setup state; only this handoff lets it
	 * enter ping/TX mode.
	 */
	gpiod_set_value(chip->boost_en_gpiod, 0);
	schedule_delayed_work(&chip->ready_work, msecs_to_jiffies(1250));
	__pm_relax(chip->ws);
	return;

out_fail:
	gpiod_set_value(chip->sw_en_gpiod, 0);
out_nopm:
	__pm_relax(chip->ws);
}

static void cps8601_ready_work(struct work_struct *work)
{
	struct cps8601 *chip = container_of(work, struct cps8601,
					    ready_work.work);

	if (!chip->ready) {
		/* retry entry: the first power-on raced a slow-starting
		 * ADSP (hboost service swallows early rail requests), so
		 * re-run the whole rail + power-on + probe sequence */
		schedule_work(&chip->init_work);
		return;
	}

	__pm_stay_awake(chip->ws);
	gpiod_set_value(chip->sw_en_gpiod, 0);
	gpiod_set_value(chip->scan_gpiod, 0);
	msleep(50);
	gpiod_set_value(chip->scan_gpiod, 1);
	dev_info(chip->dev, "scan armed (vendor post-ready state)\n");
	__pm_relax(chip->ws);
}

static ssize_t present_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct cps8601 *chip = i2c_get_clientdata(to_i2c_client(dev));

	return sysfs_emit(buf, "%d\n", chip->present);
}
static DEVICE_ATTR_RO(present);

static ssize_t pen_soc_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct cps8601 *chip = i2c_get_clientdata(to_i2c_client(dev));

	return sysfs_emit(buf, "%d\n", chip->pen_soc);
}
static DEVICE_ATTR_RO(pen_soc);

static ssize_t fw_version_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct cps8601 *chip = i2c_get_clientdata(to_i2c_client(dev));

	return sysfs_emit(buf, "0x%04x\n", chip->fw_version);
}
static DEVICE_ATTR_RO(fw_version);

static ssize_t tx_voltage_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct cps8601 *chip = i2c_get_clientdata(to_i2c_client(dev));
	u16 v = 0;

	cps8601_read16(chip, CPS8601_REG_TX_VOLTAGE, &v);
	return sysfs_emit(buf, "%u\n", v);
}
static DEVICE_ATTR_RO(tx_voltage);

static ssize_t tx_current_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct cps8601 *chip = i2c_get_clientdata(to_i2c_client(dev));
	u16 v = 0;

	cps8601_read16(chip, CPS8601_REG_TX_CURRENT, &v);
	return sysfs_emit(buf, "%u\n", v);
}
static DEVICE_ATTR_RO(tx_current);

static ssize_t scan_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct cps8601 *chip = i2c_get_clientdata(to_i2c_client(dev));
	bool v;
	int ret;

	ret = kstrtobool(buf, &v);
	if (ret)
		return ret;
	if (v)
		cps8601_scan(chip);
	return count;
}
static DEVICE_ATTR_WO(scan);

static ssize_t wireless_reg_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct cps8601 *chip = i2c_get_clientdata(to_i2c_client(dev));
	u16 val = 0;

	cps8601_read16(chip, chip->debug_reg, &val);
	return sysfs_emit(buf, "0x%04x = 0x%04x\n", chip->debug_reg, val);
}

static ssize_t wireless_reg_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct cps8601 *chip = i2c_get_clientdata(to_i2c_client(dev));
	unsigned int reg, val;

	if (sscanf(buf, "%x %x", &reg, &val) == 2)
		cps8601_write16(chip, reg, val);
	else if (sscanf(buf, "%x", &reg) == 1)
		chip->debug_reg = reg;
	else
		return -EINVAL;
	return count;
}
static DEVICE_ATTR_RW(wireless_reg);

static struct attribute *cps8601_attrs[] = {
	&dev_attr_present.attr,
	&dev_attr_pen_soc.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_tx_voltage.attr,
	&dev_attr_tx_current.attr,
	&dev_attr_scan.attr,
	&dev_attr_wireless_reg.attr,
	NULL
};
ATTRIBUTE_GROUPS(cps8601);

static const struct regmap_config cps8601_regmap_cfg = {
	.reg_bits	= 16,
	.val_bits	= 8,
	.max_register	= 0xffff,
};

static int cps8601_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct cps8601 *chip;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;
	chip->dev = dev;
	chip->pen_soc = -1;
	i2c_set_clientdata(client, chip);

	chip->regmap = devm_regmap_init_i2c(client, &cps8601_regmap_cfg);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(dev, PTR_ERR(chip->regmap),
				     "regmap init failed\n");

	chip->int_gpiod = devm_gpiod_get(dev, "int", GPIOD_IN);
	chip->sleep_gpiod = devm_gpiod_get(dev, "sleep", GPIOD_OUT_LOW);
	chip->scan_gpiod = devm_gpiod_get(dev, "scan", GPIOD_OUT_LOW);
	chip->boost_en_gpiod = devm_gpiod_get(dev, "boost-en", GPIOD_OUT_LOW);
	chip->sw_en_gpiod = devm_gpiod_get(dev, "sw-en", GPIOD_OUT_LOW);
	if (IS_ERR(chip->int_gpiod) || IS_ERR(chip->sleep_gpiod) ||
	    IS_ERR(chip->scan_gpiod) || IS_ERR(chip->boost_en_gpiod) ||
	    IS_ERR(chip->sw_en_gpiod))
		return dev_err_probe(dev, -EINVAL, "control gpio missing\n");

	device_property_read_u32(dev, "oplus,ocp-threshold", &chip->ocp_uv);
	device_property_read_u32(dev, "oplus,ovp-threshold", &chip->ovp_uv);
	device_property_read_u32(dev, "oplus,lvp-threshold", &chip->lvp_uv);
	device_property_read_u32(dev, "oplus,fod-threshold", &chip->fod_uv);

	chip->irq = gpiod_to_irq(chip->int_gpiod);
	if (chip->irq < 0)
		return dev_err_probe(dev, chip->irq, "no irq for int line\n");

	/* the IRQ itself is requested from init_work once the chip is ready */

	chip->ws = wakeup_source_register(dev, "cps8601");
	INIT_WORK(&chip->init_work, cps8601_init_work);
	INIT_DELAYED_WORK(&chip->ready_work, cps8601_ready_work);
	schedule_work(&chip->init_work);

	return 0;
}

static void cps8601_remove(struct i2c_client *client)
{
	struct cps8601 *chip = i2c_get_clientdata(client);

	cancel_work_sync(&chip->init_work);
	cancel_delayed_work_sync(&chip->ready_work);
	/* free the IRQ before touching the power lines: shutting the chip
	 * down edges INT, and the handler must not run against an
	 * unregistered wakeup source afterwards */
	if (chip->irq_armed)
		free_irq(chip->irq, chip);
	wakeup_source_unregister(chip->ws);
	if (chip->ready)
		cps8601_power_off(chip);
	else
		gpiod_set_value(chip->sw_en_gpiod, 0);
	cps8601_hboost_req(chip, 0);
}

static const struct of_device_id cps8601_of_match[] = {
	{ .compatible = "oplus,wls-charger-cps8601" },
	{ }
};
MODULE_DEVICE_TABLE(of, cps8601_of_match);

static struct i2c_driver cps8601_driver = {
	.driver = {
		.name		= "cps8601-pen-charger",
		.of_match_table	= cps8601_of_match,
		.dev_groups	= cps8601_groups,
	},
	.probe		= cps8601_probe,
	.remove		= cps8601_remove,
};
module_i2c_driver(cps8601_driver);

MODULE_AUTHOR("erhai mainline port");
MODULE_DESCRIPTION("CPS8601 wireless pen charger driver");
MODULE_LICENSE("GPL v2");
