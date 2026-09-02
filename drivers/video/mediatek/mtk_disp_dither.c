// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek Video Dither support
 *
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#include <asm/io.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>

#define DISP_DITHER_EN	  0x00
#define DITHER_EN		BIT(0)

#define DISP_DITHER_CFG	  0x20
#define DITHER_ENGINE_EN	BIT(1)

#define DISP_DITHER_SIZE  0x30

struct mtk_disp_dither_priv {
	void __iomem *base;
	struct udevice *dev;
	struct clk clk;
};

void mtk_dither_config(struct udevice *dev, struct display_timing timing)
{
	struct mtk_disp_dither_priv *dither = dev_get_priv(dev);
	u32 width = timing.hactive.typ;
	u32 height = timing.vactive.typ;

	writel(height << 16 | width, dither->base + DISP_DITHER_SIZE);
	writel(DITHER_ENGINE_EN, dither->base + DISP_DITHER_CFG);
}

void mtk_dither_start(struct udevice *dev)
{
	struct mtk_disp_dither_priv *dither = dev_get_priv(dev);

	writel(DITHER_EN, dither->base + DISP_DITHER_EN);
}

int mtk_dither_enable(struct udevice *dev)
{
	struct mtk_disp_dither_priv *dither = dev_get_priv(dev);
	int ret;

	ret = clk_enable(&dither->clk);
	if (ret)
		dev_err(dither->dev, "failed to enable clk: %s\n", errno_str(ret));

	return ret;
}

int mtk_dither_disable(struct udevice *dev)
{
	struct mtk_disp_dither_priv *dither = dev_get_priv(dev);
	int ret;

	ret = clk_disable(&dither->clk);
	if (ret)
		dev_err(dither->dev, "failed to disable clk: %s\n", errno_str(ret));

	return ret;
}

static int mtk_disp_dither_probe(struct udevice *dev)
{
	struct mtk_disp_dither_priv *dither = dev_get_priv(dev);
	int ret;

	dither->dev = dev;

	dither->base = dev_remap_addr(dev);
	if (!dither->base)
		return -EINVAL;

	ret = clk_get_by_index(dev, 0, &dither->clk);
	if (ret)
		return ret;

	return 0;
}

static const struct udevice_id mtk_disp_dither_ids[] = {
	{ .compatible = "mediatek,mt8183-disp-dither" },
	{}
};

U_BOOT_DRIVER(mtk_disp_dither) = {
	.name	   = "mtk_disp_dither",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_disp_dither_ids,
	.probe	   = mtk_disp_dither_probe,
	.priv_auto = sizeof(struct mtk_disp_dither_priv),
};
