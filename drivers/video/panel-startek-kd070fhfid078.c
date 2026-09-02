// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 MediaTek Inc.
 * Copyright (C) 2026 BayLibre, SAS
 *
 * Authors:
 * - Guillaume La Roque <glaroque@baylibre.com>
 * - Jitao Shi <jitao.shi@mediatek.com>
 * - David Lechner <dlechner@baylibre.com>
 */

#include <asm-generic/gpio.h>
#include <backlight.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>
#include <linux/delay.h>
#include <mipi_dsi.h>
#include <panel.h>
#include <power/regulator.h>

struct stk078_panel {
	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;
	struct mipi_dsi_device *dsi;
	struct udevice *iovcc;
	struct udevice *vdd;
	struct udevice *vddp;
	struct udevice *backlight;
};

static const struct drm_display_mode default_mode = {
	.clock = 156458,
	.hdisplay = 1200,
	.hsync_start = 1200 + 50,
	.hsync_end = 1200 + 50 + 24,
	.htotal = 1200 + 50 + 24 + 66,
	.vdisplay = 1920,
	.vsync_start = 1920 + 14,
	.vsync_end = 1920 + 14 + 2,
	.vtotal = 1920 + 14 + 2 + 10,
};

static int stk078_panel_init(struct stk078_panel *stk)
{
	struct mipi_dsi_device *dsi = stk->dsi;
	int ret;

	mipi_dsi_generic_write_seq(dsi, 0xB0, 0x05);
	mipi_dsi_generic_write_seq(dsi, 0xB3, 0x52);
	mipi_dsi_generic_write_seq(dsi, 0xB8, 0x7F);
	mipi_dsi_generic_write_seq(dsi, 0xBC, 0x20);
	mipi_dsi_generic_write_seq(dsi, 0xD6, 0x7F);
	mipi_dsi_generic_write_seq(dsi, 0xB0, 0x01);
	mipi_dsi_generic_write_seq(dsi, 0xC0, 0x0D);
	mipi_dsi_generic_write_seq(dsi, 0xC1, 0x0D);
	mipi_dsi_generic_write_seq(dsi, 0xC2, 0x06);
	mipi_dsi_generic_write_seq(dsi, 0xC3, 0x06);
	mipi_dsi_generic_write_seq(dsi, 0xC4, 0x08);
	mipi_dsi_generic_write_seq(dsi, 0xC5, 0x08);
	mipi_dsi_generic_write_seq(dsi, 0xC6, 0x0A);
	mipi_dsi_generic_write_seq(dsi, 0xC7, 0x0A);
	mipi_dsi_generic_write_seq(dsi, 0xC8, 0x0C);
	mipi_dsi_generic_write_seq(dsi, 0xC9, 0x0C);
	mipi_dsi_generic_write_seq(dsi, 0xCA, 0x00);
	mipi_dsi_generic_write_seq(dsi, 0xCB, 0x00);
	mipi_dsi_generic_write_seq(dsi, 0xCC, 0x0E);
	mipi_dsi_generic_write_seq(dsi, 0xCD, 0x0E);
	mipi_dsi_generic_write_seq(dsi, 0xCE, 0x01);
	mipi_dsi_generic_write_seq(dsi, 0xCF, 0x01);
	mipi_dsi_generic_write_seq(dsi, 0xD0, 0x04);
	mipi_dsi_generic_write_seq(dsi, 0xD1, 0x04);
	mipi_dsi_generic_write_seq(dsi, 0xD2, 0x00);
	mipi_dsi_generic_write_seq(dsi, 0xD3, 0x00);
	mipi_dsi_generic_write_seq(dsi, 0xD4, 0x0D);
	mipi_dsi_generic_write_seq(dsi, 0xD5, 0x0D);
	mipi_dsi_generic_write_seq(dsi, 0xD6, 0x05);
	mipi_dsi_generic_write_seq(dsi, 0xD7, 0x05);
	mipi_dsi_generic_write_seq(dsi, 0xD8, 0x07);
	mipi_dsi_generic_write_seq(dsi, 0xD9, 0x07);
	mipi_dsi_generic_write_seq(dsi, 0xDA, 0x09);
	mipi_dsi_generic_write_seq(dsi, 0xDB, 0x09);
	mipi_dsi_generic_write_seq(dsi, 0xDC, 0x0B);
	mipi_dsi_generic_write_seq(dsi, 0xDD, 0x0B);
	mipi_dsi_generic_write_seq(dsi, 0xDE, 0x00);
	mipi_dsi_generic_write_seq(dsi, 0xDF, 0x00);
	mipi_dsi_generic_write_seq(dsi, 0xE0, 0x0E);
	mipi_dsi_generic_write_seq(dsi, 0xE1, 0x0E);
	mipi_dsi_generic_write_seq(dsi, 0xE2, 0x01);
	mipi_dsi_generic_write_seq(dsi, 0xE3, 0x01);
	mipi_dsi_generic_write_seq(dsi, 0xE4, 0x03);
	mipi_dsi_generic_write_seq(dsi, 0xE5, 0x03);
	mipi_dsi_generic_write_seq(dsi, 0xE6, 0x00);
	mipi_dsi_generic_write_seq(dsi, 0xE7, 0x00);
	mipi_dsi_generic_write_seq(dsi, 0xB0, 0x03);
	mipi_dsi_generic_write_seq(dsi, 0xBA, 0xF0);
	mipi_dsi_generic_write_seq(dsi, 0xC8, 0x07);
	mipi_dsi_generic_write_seq(dsi, 0xC9, 0x03);
	mipi_dsi_generic_write_seq(dsi, 0xCA, 0x41);
	mipi_dsi_generic_write_seq(dsi, 0xD2, 0x01);
	mipi_dsi_generic_write_seq(dsi, 0xD3, 0x05);
	mipi_dsi_generic_write_seq(dsi, 0xD4, 0x05);
	mipi_dsi_generic_write_seq(dsi, 0xD5, 0x8A);
	mipi_dsi_generic_write_seq(dsi, 0xE4, 0xC0);
	mipi_dsi_generic_write_seq(dsi, 0xE5, 0x00);
	mipi_dsi_generic_write_seq(dsi, 0xB0, 0x00);
	mipi_dsi_generic_write_seq(dsi, 0xBF, 0x1F);
	mipi_dsi_generic_write_seq(dsi, 0xC0, 0x12);
	mipi_dsi_generic_write_seq(dsi, 0xC2, 0x1E);
	mipi_dsi_generic_write_seq(dsi, 0xC4, 0x1E);
	mipi_dsi_generic_write_seq(dsi, 0xB0, 0x06);
	mipi_dsi_generic_write_seq(dsi, 0xB8, 0xA5);
	mipi_dsi_generic_write_seq(dsi, 0xC0, 0xA5);
	mipi_dsi_generic_write_seq(dsi, 0xBC, 0x11);
	mipi_dsi_generic_write_seq(dsi, 0xD5, 0x48);
	mipi_dsi_generic_write_seq(dsi, 0xB8, 0x00);
	mipi_dsi_generic_write_seq(dsi, 0xC0, 0x00);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0)
		return ret;

	mdelay(120);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0)
		return ret;

	mdelay(20);

	return 0;
}

static int stk078_panel_enable_backlight(struct udevice *dev)
{
	struct stk078_panel *stk = dev_get_priv(dev);
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct mipi_dsi_device *dsi = plat->device;
	int ret;

	stk->dsi = dsi;
	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach failed: %d\n", ret);
		return ret;
	}

	dm_gpio_set_value(stk->reset_gpio, 0);
	if (stk->enable_gpio)
		dm_gpio_set_value(stk->enable_gpio, 0);
	mdelay(1);

	/*
	 * VDDP and IOVCC come up together, then VDD once they have settled.
	 * Boards that switch VDD and IOVCC from a single regulator simply see
	 * the second regulator_enable() as a reference count.
	 */
	if (stk->vddp) {
		ret = regulator_enable(stk->vddp);
		if (ret < 0) {
			dev_err(dev, "enable vddp failed: %d\n", ret);
			goto out_gpio;
		}
	}

	ret = regulator_enable(stk->iovcc);
	if (ret < 0) {
		dev_err(dev, "enable iovcc failed: %d\n", ret);
		goto out_vddp;
	}

	mdelay(10);

	ret = regulator_enable(stk->vdd);
	if (ret < 0) {
		dev_err(dev, "enable vdd failed: %d\n", ret);
		goto out_iovcc;
	}

	mdelay(15);
	if (stk->enable_gpio)
		dm_gpio_set_value(stk->enable_gpio, 1);
	mdelay(10);
	dm_gpio_set_value(stk->reset_gpio, 1);
	mdelay(10);

	ret = stk078_panel_init(stk);
	if (ret < 0) {
		dev_err(dev, "panel init sequence failed: %d\n", ret);
		goto out_power;
	}

	ret = backlight_enable(stk->backlight);
	if (ret < 0) {
		dev_err(dev, "enable backlight failed: %d\n", ret);
		goto out_power;
	}

	return 0;

out_power:
	regulator_disable(stk->vdd);
out_iovcc:
	regulator_disable(stk->iovcc);
out_vddp:
	if (stk->vddp)
		regulator_disable(stk->vddp);
out_gpio:
	dm_gpio_set_value(stk->reset_gpio, 0);
	if (stk->enable_gpio)
		dm_gpio_set_value(stk->enable_gpio, 0);

	return ret;
}

static int stk078_panel_add(struct udevice *dev)
{
	struct stk078_panel *stk = dev_get_priv(dev);
	int ret;

	ret = device_get_supply_regulator(dev, "iovcc-supply", &stk->iovcc);
	if (ret) {
		dev_err(dev, "Failed to get iovcc regulator: %d\n", ret);
		return ret;
	}

	ret = device_get_supply_regulator(dev, "vdd-supply", &stk->vdd);
	if (ret) {
		dev_err(dev, "Failed to get vdd regulator: %d\n", ret);
		return ret;
	}

	/*
	 * Boards that gate the panel's VDDP rail separately from VDD describe
	 * it with an extra vddp-supply. Most wire VDDP to VDD and omit it.
	 */
	ret = device_get_supply_regulator(dev, "vddp-supply", &stk->vddp);
	if (ret == -ENOENT) {
		stk->vddp = NULL;
	} else if (ret) {
		dev_err(dev, "Failed to get vddp regulator: %d\n", ret);
		return ret;
	}

	stk->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_IS_OUT);
	if (IS_ERR(stk->reset_gpio)) {
		ret = PTR_ERR(stk->reset_gpio);
		dev_err(dev, "cannot get reset-gpios %d\n", ret);
		return ret;
	}

	stk->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_IS_OUT);
	if (IS_ERR(stk->enable_gpio)) {
		ret = PTR_ERR(stk->enable_gpio);
		dev_err(dev, "cannot get enable-gpios %d\n", ret);
		return ret;
	}

	ret = uclass_get_device_by_phandle(UCLASS_PANEL_BACKLIGHT, dev,
					   "backlight", &stk->backlight);
	if (ret) {
		dev_err(dev, "failed to get backlight: %d\n", ret);
		return ret;
	}

	return 0;
}

static int stk078_panel_probe(struct udevice *dev)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	int ret;

	plat->lanes = 4;
	plat->format = MIPI_DSI_FMT_RGB888;
	plat->mode_flags = MIPI_DSI_MODE_VIDEO |
			   MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			   MIPI_DSI_MODE_LPM |
			   MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ret = stk078_panel_add(dev);
	if (ret < 0)
		return ret;

	return 0;
}

static int stk078_panel_get_modes(struct udevice *dev,
				  const struct drm_display_mode **modes)
{
	*modes = &default_mode;

	return 1;
}

static const struct panel_ops stk078_panel_ops = {
	.enable_backlight = stk078_panel_enable_backlight,
	.get_modes = stk078_panel_get_modes,
};

static const struct udevice_id stk078_of_match[] = {
	{ .compatible = "startek,kd070fhfid078" },
	{ }
};

U_BOOT_DRIVER(stk078_panel_driver) = {
	.name		= "panel-startek-kd070fhfid078",
	.id		= UCLASS_PANEL,
	.of_match	= stk078_of_match,
	.ops		= &stk078_panel_ops,
	.probe		= stk078_panel_probe,
	.plat_auto	= sizeof(struct mipi_dsi_panel_plat),
	.priv_auto	= sizeof(struct stk078_panel),
};
