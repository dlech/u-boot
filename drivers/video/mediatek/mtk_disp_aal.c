// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek Video Adaptive Ambient Light support
 *
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#include <asm/io.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>
#include <linux/bitfield.h>

#define DISP_AAL_EN    0x00
#define AAL_EN		     BIT(0)

#define DISP_AAL_SIZE  0x30
#define AAL_SIZE_HSIZE		GENMASK(28, 16)
#define AAL_SIZE_VSIZE		GENMASK(12, 0)

#define DISP_AAL_OUTPUT_SIZE 0x4d8

struct mtk_disp_aal_priv {
	void __iomem *base;
	struct udevice *dev;
	struct clk clk;
};

void mtk_aal_config(struct udevice *dev, struct display_timing timing)
{
	struct mtk_disp_aal_priv *aal = dev_get_priv(dev);
	u32 sz;

	sz = FIELD_PREP(AAL_SIZE_HSIZE, timing.hactive.typ) |
	     FIELD_PREP(AAL_SIZE_VSIZE, timing.vactive.typ);

	writel(sz, aal->base + DISP_AAL_SIZE);
	writel(sz, aal->base + DISP_AAL_OUTPUT_SIZE);
}

void mtk_aal_start(struct udevice *dev)
{
	struct mtk_disp_aal_priv *aal = dev_get_priv(dev);

	writel(AAL_EN, aal->base + DISP_AAL_EN);
}

int mtk_aal_enable(struct udevice *dev)
{
	struct mtk_disp_aal_priv *aal = dev_get_priv(dev);
	int ret;

	ret = clk_enable(&aal->clk);
	if (ret)
		dev_err(aal->dev, "failed to enable clk: %s\n", errno_str(ret));

	return ret;
}

int mtk_aal_disable(struct udevice *dev)
{
	struct mtk_disp_aal_priv *aal = dev_get_priv(dev);
	int ret;

	ret = clk_disable(&aal->clk);
	if (ret)
		dev_err(aal->dev, "failed to disable clk: %s\n", errno_str(ret));

	return ret;
}

static int mtk_disp_aal_probe(struct udevice *dev)
{
	struct mtk_disp_aal_priv *aal = dev_get_priv(dev);
	int ret;

	aal->dev = dev;

	aal->base = dev_remap_addr(dev);
	if (!aal->base)
		return -EINVAL;

	ret = clk_get_by_index(dev, 0, &aal->clk);
	if (ret)
		return ret;

	return 0;
}

static const struct udevice_id mtk_disp_aal_ids[] = {
	{ .compatible = "mediatek,mt8183-disp-aal" },
	{}
};

U_BOOT_DRIVER(mtk_disp_aal) = {
	.name	   = "mtk_disp_aal",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_disp_aal_ids,
	.probe	   = mtk_disp_aal_probe,
	.priv_auto = sizeof(struct mtk_disp_aal_priv),
};
