// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek Video Color Correction Engine support
 *
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#include <asm/io.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>

#define DISP_CCORR_EN	  0x00
#define CCORR_EN		BIT(0)

#define DISP_CCORR_CFG	  0x20
#define CCORR_ENGINE_EN		BIT(1)

#define DISP_CCORR_SIZE	  0x30

struct mtk_disp_ccorr_priv {
	void __iomem *base;
	struct udevice *dev;
	struct clk clk;
};

void mtk_ccorr_config(struct udevice *dev, struct display_timing timing)
{
	struct mtk_disp_ccorr_priv *ccorr = dev_get_priv(dev);
	u32 width = timing.hactive.typ;
	u32 height = timing.vactive.typ;

	writel(width << 16 | height, ccorr->base + DISP_CCORR_SIZE);
	writel(CCORR_ENGINE_EN, ccorr->base + DISP_CCORR_CFG);
}

void mtk_ccorr_start(struct udevice *dev)
{
	struct mtk_disp_ccorr_priv *ccorr = dev_get_priv(dev);

	writel(CCORR_EN, ccorr->base + DISP_CCORR_EN);
}

int mtk_ccorr_enable(struct udevice *dev)
{
	struct mtk_disp_ccorr_priv *ccorr = dev_get_priv(dev);
	int ret;

	ret = clk_enable(&ccorr->clk);
	if (ret)
		dev_err(ccorr->dev, "failed to enable clk: %s\n", errno_str(ret));

	return ret;
}

int mtk_ccorr_disable(struct udevice *dev)
{
	struct mtk_disp_ccorr_priv *ccorr = dev_get_priv(dev);
	int ret;

	ret = clk_disable(&ccorr->clk);
	if (ret)
		dev_err(ccorr->dev, "failed to disable clk: %s\n", errno_str(ret));

	return ret;
}

static int mtk_disp_ccorr_probe(struct udevice *dev)
{
	struct mtk_disp_ccorr_priv *ccorr = dev_get_priv(dev);
	int ret;

	ccorr->dev = dev;

	ccorr->base = dev_remap_addr(dev);
	if (IS_ERR(ccorr->base))
		return PTR_ERR(ccorr->base);

	ret = clk_get_by_index(dev, 0, &ccorr->clk);
	if (ret)
		return ret;

	return 0;
}

static const struct udevice_id mtk_disp_ccorr_ids[] = {
	{ .compatible = "mediatek,mt8183-disp-ccorr" },
	{}
};

U_BOOT_DRIVER(mtk_disp_ccorr) = {
	.name	   = "mtk_disp_ccorr",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_disp_ccorr_ids,
	.probe	   = mtk_disp_ccorr_probe,
	.priv_auto = sizeof(struct mtk_disp_ccorr_priv),
};
