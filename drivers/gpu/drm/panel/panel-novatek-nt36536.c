// SPDX-License-Identifier: GPL-2.0-only
/*
 * Novatek NT36536 dual-DSI video mode panel driver
 * Copyright (c) 2026, Erwin
 *
 * Dual-DSI handling based on panel-himax-hx83121a.c
 */

#include <linux/backlight.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>
#include <linux/seq_file.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct novatek_nt36536 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi[2];
	struct drm_dsc_config dsc;
	struct gpio_desc *reset_gpio;
	struct regulator *vddio;
	struct regulator *vsp;
	struct regulator *vsn;
	struct dentry *dbg_dir;
};

static inline struct novatek_nt36536 *to_novatek_nt36536(struct drm_panel *panel)
{
	return container_of(panel, struct novatek_nt36536, panel);
}

static void novatek_nt36536_reset(struct novatek_nt36536 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int novatek_nt36536_on(struct novatek_nt36536 *ctx)
{
	/* Commands are synced to both DSI links via the DSI_1 device */
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[1] };

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x07, 0x46);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbb, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x24);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x98, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x89, 0xa4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe6, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x27);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd0, 0x31);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd1, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xde, 0x40);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xdf, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3b, 0x03, 0xe8, 0x1a, 0x04,
				     0x04, 0x00, 0x80, 0x36, 0x36);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb3, 0x40);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51, 0x07, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x53, 0x2c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x55, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x00, 0x60);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x07, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x09, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x11, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x12, 0xa0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x13, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x15, 0xea);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x16, 0x17);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x0a, 0x8e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x0b, 0x8e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x0c, 0x8e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x0d, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x19, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1a, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1b, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1c, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1d, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1e, 0x14);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1f, 0x18);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x20, 0x1c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x21, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x22, 0x24);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x23, 0x28);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x24, 0x2c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x25, 0x30);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x26, 0x34);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x27, 0x38);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x28, 0x3c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2a, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2b, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x30, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x31, 0xfd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x32, 0xfc);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x33, 0xfa);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x34, 0xf8);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x35, 0xf6);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x36, 0xf4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x37, 0xf2);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x38, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x39, 0xee);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3a, 0xec);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3b, 0xea);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3d, 0xe9);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3f, 0xe8);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x40, 0xe7);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x41, 0xe6);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x45, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x46, 0xf9);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x47, 0xf6);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x48, 0xf2);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x49, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x4a, 0xec);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x4b, 0xe8);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x4c, 0xe4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x4d, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x4e, 0xde);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x4f, 0xd9);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x50, 0xd6);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51, 0xd4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x52, 0xc2);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x53, 0xd0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x54, 0xcd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x58, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x59, 0xf6);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5a, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5b, 0xeb);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5c, 0xe8);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5d, 0xe5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5e, 0xe3);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5f, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x60, 0xde);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x61, 0xda);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x62, 0xd7);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x63, 0xd4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x64, 0xd2);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x65, 0xd0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x66, 0xcc);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x67, 0xc8);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6e, 0xb3);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0xd0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x70, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x71, 0xb3);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x90, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x91, 0x89, 0xa8, 0x00, 0x14,
				     0xd2, 0x00, 0x00, 0x00, 0x02, 0x38,
				     0x00, 0x0b, 0x05, 0x7a, 0x03, 0x37);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x92, 0x10, 0xe0);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 10);

	return dsi_ctx.accum_err;
}

static int novatek_nt36536_off(struct novatek_nt36536 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[1] };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 10);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 65);

	return dsi_ctx.accum_err;
}

/*
 * Supply sequence of the XN242 module, taken from the vendor kernel: the panel
 * power sequence (vddio, then 2 ms; 5 ms before vddio goes off) and the
 * KTZ8868 bias switching (OUTP, 8 ms, OUTN, 10 ms on the way up; 2 ms, OUTN,
 * 8 ms, OUTP, 10 ms on the way down).
 */
static int novatek_nt36536_power_on(struct novatek_nt36536 *ctx)
{
	int ret;

	ret = regulator_enable(ctx->vddio);
	if (ret < 0)
		return ret;
	usleep_range(2000, 3000);

	ret = regulator_enable(ctx->vsp);
	if (ret < 0)
		goto err_vddio;
	usleep_range(8000, 9000);

	ret = regulator_enable(ctx->vsn);
	if (ret < 0)
		goto err_vsp;
	usleep_range(10000, 11000);

	return 0;

err_vsp:
	regulator_disable(ctx->vsp);
	usleep_range(10000, 11000);
err_vddio:
	usleep_range(5000, 6000);
	regulator_disable(ctx->vddio);

	return ret;
}

static void novatek_nt36536_power_off(struct novatek_nt36536 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(2000, 3000);

	regulator_disable(ctx->vsn);
	usleep_range(8000, 9000);

	/* 10 ms after OUTP, then the 5 ms the panel needs before vddio goes */
	regulator_disable(ctx->vsp);
	usleep_range(15000, 16000);

	regulator_disable(ctx->vddio);
}

static int novatek_nt36536_prepare(struct drm_panel *panel)
{
	struct novatek_nt36536 *ctx = to_novatek_nt36536(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[1] };
	int ret;

	ret = novatek_nt36536_power_on(ctx);
	if (ret < 0)
		return ret;

	novatek_nt36536_reset(ctx);

	/*
	 * The stock sequence configures the panel's DSC decoder solely through
	 * the vendor 0x91 blob in the on-commands; it never receives a DCS PPS
	 * or a compression-mode packet. Sending the standard PPS afterwards
	 * re-programmed the decoder with full-width (3392) semantics while each
	 * DSI link only carries 1696 px, so the decoder starved -- black screen.
	 */
	ret = novatek_nt36536_on(ctx);
	if (ret < 0)
		goto err;

	if (dsi_ctx.accum_err) {
		ret = dsi_ctx.accum_err;
		goto err;
	}

	return 0;

err:
	novatek_nt36536_power_off(ctx);

	return ret;
}

static int novatek_nt36536_unprepare(struct drm_panel *panel)
{
	struct novatek_nt36536 *ctx = to_novatek_nt36536(panel);
	struct device *dev = &ctx->dsi[0]->dev;
	int ret;

	ret = novatek_nt36536_off(ctx);
	if (ret < 0)
		dev_err(dev, "failed to off panel: %d\n", ret);

	novatek_nt36536_power_off(ctx);

	return 0;
}

static const struct drm_display_mode novatek_nt36536_mode = {
	.clock = (1696 + 50 + 32 + 64) * 2 * (2400 + 26 + 2 + 230) * 120 / 1000,
	.hdisplay = 1696 * 2,
	.hsync_start = (1696 + 50) * 2,
	.hsync_end = (1696 + 50 + 32) * 2,
	.htotal = (1696 + 50 + 32 + 64) * 2,
	.vdisplay = 2400,
	.vsync_start = 2400 + 26,
	.vsync_end = 2400 + 26 + 2,
	.vtotal = 2400 + 26 + 2 + 230,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int novatek_nt36536_get_modes(struct drm_panel *panel,
				     struct drm_connector *connector)
{
	connector->display_info.width_mm = 274;
	connector->display_info.height_mm = 193;
	connector->display_info.bpc = 8;

	return drm_connector_helper_get_modes_fixed(connector,
						    &novatek_nt36536_mode);
}

static const struct drm_panel_funcs novatek_nt36536_panel_funcs = {
	.prepare = novatek_nt36536_prepare,
	.unprepare = novatek_nt36536_unprepare,
	.get_modes = novatek_nt36536_get_modes,
};

#if defined(CONFIG_DEBUG_FS)
/*
 * TEMP bring-up diagnostic (drop before erhai/main): DCS readback over each
 * DSI link separately, to tell "panel dead" apart from "link/decoding
 * broken" while the panel shows nothing. RDDPM tells whether the panel
 * believes it is powered and displaying, RDNUMER counts link errors, and a
 * moving GETSCANLINE proves the panel is actually scanning frames.
 */
static void nt36536_dbg_dcs(struct seq_file *s, struct mipi_dsi_device *dsi,
			    u8 cmd, size_t len, const char *name)
{
	u8 buf[4] = { 0 };
	ssize_t ret;

	ret = mipi_dsi_dcs_read(dsi, cmd, buf, len);
	if (ret < 0) {
		seq_printf(s, "  %s(0x%02x): read failed %zd\n", name, cmd, ret);
		return;
	}
	seq_printf(s, "  %s(0x%02x): len=%zd val=%*ph\n", name, cmd, ret,
		   (int)ret, buf);
}

static int nt36536_dbg_status_show(struct seq_file *s, void *unused)
{
	struct novatek_nt36536 *ctx = s->private;
	int i;

	for (i = 0; i < 2; i++) {
		u8 scan_a[2] = { 0 }, scan_b[2] = { 0 };
		ssize_t ra, rb;

		seq_printf(s, "link%d (%s):\n", i, dev_name(&ctx->dsi[i]->dev));
		nt36536_dbg_dcs(s, ctx->dsi[i], 0xda, 1, "RDID1");
		nt36536_dbg_dcs(s, ctx->dsi[i], 0x0a, 1, "RDDPM");
		nt36536_dbg_dcs(s, ctx->dsi[i], 0x05, 1, "RDNUMER");
		ra = mipi_dsi_dcs_read(ctx->dsi[i], 0x45, scan_a, 2);
		msleep(50);
		rb = mipi_dsi_dcs_read(ctx->dsi[i], 0x45, scan_b, 2);
		if (ra == 2 && rb == 2)
			seq_printf(s, "  GETSCANLINE: %u -> %u (%s)\n",
				   (scan_a[1] << 8) | scan_a[0],
				   (scan_b[1] << 8) | scan_b[0],
				   scan_a[0] != scan_b[0] || scan_a[1] != scan_b[1] ?
				   "SCANNING" : "static");
		else
			seq_printf(s, "  GETSCANLINE: read failed %zd/%zd\n", ra, rb);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nt36536_dbg_status);

static void nt36536_debugfs_init(struct novatek_nt36536 *ctx)
{
	struct device *dev = &ctx->dsi[0]->dev;

	ctx->dbg_dir = debugfs_create_dir("nt36536-panel", NULL);
	debugfs_create_file("status", 0444, ctx->dbg_dir, ctx,
			    &nt36536_dbg_status_fops);
	dev_info(dev, "TEMP debugfs probe at /sys/kernel/debug/nt36536-panel/status\n");
}

static void nt36536_debugfs_exit(struct novatek_nt36536 *ctx)
{
	debugfs_remove_recursive(ctx->dbg_dir);
}
#else
static inline void nt36536_debugfs_init(struct novatek_nt36536 *ctx) { }
static inline void nt36536_debugfs_exit(struct novatek_nt36536 *ctx) { }
#endif

static int novatek_nt36536_probe(struct mipi_dsi_device *dsi)
{
	struct mipi_dsi_device_info dsi_info = { "dsi-secondary", 0, NULL };
	struct mipi_dsi_host *dsi1_host;
	struct device *dev = &dsi->dev;
	struct device_node *dsi1;
	struct novatek_nt36536 *ctx;
	int ret, i;

	ctx = devm_drm_panel_alloc(dev, struct novatek_nt36536, panel,
				   &novatek_nt36536_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->vddio = devm_regulator_get(dev, "vddio");
	if (IS_ERR(ctx->vddio))
		return dev_err_probe(dev, PTR_ERR(ctx->vddio),
				     "failed to get vddio regulator\n");

	ctx->vsp = devm_regulator_get(dev, "vsp");
	if (IS_ERR(ctx->vsp))
		return dev_err_probe(dev, PTR_ERR(ctx->vsp),
				     "failed to get vsp regulator\n");

	ctx->vsn = devm_regulator_get(dev, "vsn");
	if (IS_ERR(ctx->vsn))
		return dev_err_probe(dev, PTR_ERR(ctx->vsn),
				     "failed to get vsn regulator\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset-gpios\n");

	/* port@1 of the panel node points at the secondary DSI host */
	dsi1 = of_graph_get_remote_node(dsi->dev.of_node, 1, -1);
	if (!dsi1)
		return dev_err_probe(dev, -ENODEV,
				     "cannot get secondary DSI node\n");

	dsi1_host = of_find_mipi_dsi_host_by_node(dsi1);
	of_node_put(dsi1);
	if (!dsi1_host)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "cannot get secondary DSI host\n");

	ctx->dsi[1] = devm_mipi_dsi_device_register_full(dev, dsi1_host,
							 &dsi_info);
	if (IS_ERR(ctx->dsi[1]))
		return dev_err_probe(dev, PTR_ERR(ctx->dsi[1]),
				     "cannot get secondary DSI device\n");

	ctx->dsi[0] = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);
	mipi_dsi_set_drvdata(ctx->dsi[1], ctx);

	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	/* This panel only supports DSC; unconditionally enable it */
	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 2;
	ctx->dsc.slice_height = 20;
	ctx->dsc.slice_width = 848;
	ctx->dsc.slice_count = 1696 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;

	for (i = 0; i < 2; i++) {
		ctx->dsi[i]->lanes = 4;
		ctx->dsi[i]->format = MIPI_DSI_FMT_RGB888;
		/* Vendor stock keeps the DSI clock lane in continuous HS (no
		 * NON_CONTINUOUS property in the panel dtsi); this DSC video
		 * panel loses receiver lock if the clock lane drops to LP
		 * during blanking, so match the vendor and run continuous. */
		ctx->dsi[i]->mode_flags = MIPI_DSI_MODE_VIDEO |
					  MIPI_DSI_MODE_LPM;
		ctx->dsi[i]->dsc = &ctx->dsc;
		/* vendor packs two 848-px slices into each video packet */
		ctx->dsi[i]->dsc_slice_per_pkt = 2;

		ret = devm_mipi_dsi_attach(dev, ctx->dsi[i]);
		if (ret < 0) {
			drm_panel_remove(&ctx->panel);
			return dev_err_probe(dev, ret,
					     "failed to attach to DSI host\n");
		}
	}

	nt36536_debugfs_init(ctx);

	return 0;
}

static void novatek_nt36536_remove(struct mipi_dsi_device *dsi)
{
	struct novatek_nt36536 *ctx = mipi_dsi_get_drvdata(dsi);

	nt36536_debugfs_exit(ctx);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id novatek_nt36536_of_match[] = {
	{ .compatible = "novatek,nt36536" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, novatek_nt36536_of_match);

static struct mipi_dsi_driver novatek_nt36536_driver = {
	.probe = novatek_nt36536_probe,
	.remove = novatek_nt36536_remove,
	.driver = {
		.name = "panel-novatek-nt36536",
		.of_match_table = novatek_nt36536_of_match,
	},
};
module_mipi_dsi_driver(novatek_nt36536_driver);

MODULE_AUTHOR("Erwin <yzddmr7@gmail.com>");
MODULE_DESCRIPTION("Novatek NT36536 dual-DSI video mode panel driver");
MODULE_LICENSE("GPL");
