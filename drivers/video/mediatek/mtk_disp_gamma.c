// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek Video GAMMA support
 *
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#include <asm/io.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>

#define DISP_GAMMA_EN	 0x00
#define GAMMA_EN	       BIT(0)

#define DISP_GAMMA_SIZE	 0x30

struct mtk_disp_gamma_priv {
	void __iomem *base;
	struct udevice *dev;
	struct clk clk;
};

void mtk_gamma_config(struct udevice *dev, struct display_timing timing)
{
	struct mtk_disp_gamma_priv *gamma = dev_get_priv(dev);
	u32 width = timing.hactive.typ;
	u32 height = timing.vactive.typ;

	writel(height << 16 | width, gamma->base + DISP_GAMMA_SIZE);
}

void mtk_gamma_start(struct udevice *dev)
{
	struct mtk_disp_gamma_priv *gamma = dev_get_priv(dev);

	writel(GAMMA_EN, gamma->base + DISP_GAMMA_EN);
}

int mtk_gamma_enable(struct udevice *dev)
{
	struct mtk_disp_gamma_priv *gamma = dev_get_priv(dev);
	int ret;

	ret = clk_enable(&gamma->clk);
	if (ret)
		dev_err(gamma->dev, "failed to enable clk: %s\n", errno_str(ret));

	return ret;
}

int mtk_gamma_disable(struct udevice *dev)
{
	struct mtk_disp_gamma_priv *gamma = dev_get_priv(dev);
	int ret;

	ret = clk_disable(&gamma->clk);
	if (ret)
		dev_err(gamma->dev, "failed to disable clk: %s\n", errno_str(ret));

	return ret;
}

static int mtk_disp_gamma_probe(struct udevice *dev)
{
	struct mtk_disp_gamma_priv *gamma = dev_get_priv(dev);
	int ret;

	gamma->dev = dev;

	gamma->base = dev_remap_addr(dev);
	if (IS_ERR(gamma->base))
		return PTR_ERR(gamma->base);

	ret = clk_get_by_index(dev, 0, &gamma->clk);
	if (ret)
		return ret;

	return 0;
}

static const struct udevice_id mtk_disp_gamma_ids[] = {
	{ .compatible = "mediatek,mt8183-disp-gamma" },
	{}
};

U_BOOT_DRIVER(mtk_disp_gamma) = {
	.name	   = "mtk_disp_gamma",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_disp_gamma_ids,
	.probe	   = mtk_disp_gamma_probe,
	.priv_auto = sizeof(struct mtk_disp_gamma_priv),
};
