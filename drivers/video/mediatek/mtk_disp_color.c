// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek Video Color support
 *
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#include <asm/io.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>

#define DISP_COLOR_CFG_MAIN  0x400
#define COLOR_BYPASS_ALL	    BIT(7)
#define COLOR_SEQ_SEL		    BIT(13)

#define DISP_COLOR_START     0xc00
#define DISP_COLOR_WIDTH     0xc50
#define DISP_COLOR_HEIGHT    0xc54

struct mtk_disp_color_priv {
	void __iomem *base;
	struct udevice *dev;
	struct clk clk;
};

void mtk_color_config(struct udevice *dev, struct display_timing timing)
{
	struct mtk_disp_color_priv *color = dev_get_priv(dev);
	u32 width = timing.hactive.typ;
	u32 height = timing.vactive.typ;

	writel(width, color->base + DISP_COLOR_WIDTH);
	writel(height, color->base + DISP_COLOR_HEIGHT);
}

void mtk_color_start(struct udevice *dev)
{
	struct mtk_disp_color_priv *color = dev_get_priv(dev);

	writel(COLOR_BYPASS_ALL | COLOR_SEQ_SEL, color->base + DISP_COLOR_CFG_MAIN);
	writel(1, color->base + DISP_COLOR_START);
}

int mtk_color_enable(struct udevice *dev)
{
	struct mtk_disp_color_priv *color = dev_get_priv(dev);
	int ret;

	ret = clk_enable(&color->clk);
	if (ret)
		dev_err(color->dev, "failed to enable clk: %s\n", errno_str(ret));

	return ret;
}

int mtk_color_disable(struct udevice *dev)
{
	struct mtk_disp_color_priv *color = dev_get_priv(dev);
	int ret;

	ret = clk_disable(&color->clk);
	if (ret)
		dev_err(color->dev, "failed to disable clk: %s\n", errno_str(ret));

	return ret;
}

static int mtk_disp_color_probe(struct udevice *dev)
{
	struct mtk_disp_color_priv *color = dev_get_priv(dev);
	int ret;

	color->dev = dev;

	color->base = dev_remap_addr(dev);
	if (!color->base)
		return -EINVAL;

	ret = clk_get_by_index(dev, 0, &color->clk);
	if (ret)
		return ret;

	return 0;
}

static const struct udevice_id mtk_disp_color_ids[] = {
	{ .compatible = "mediatek,mt8173-disp-color" },
	{}
};

U_BOOT_DRIVER(mtk_disp_color) = {
	.name	   = "mtk_disp_color",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_disp_color_ids,
	.probe	   = mtk_disp_color_probe,
	.priv_auto = sizeof(struct mtk_disp_color_priv),
};
