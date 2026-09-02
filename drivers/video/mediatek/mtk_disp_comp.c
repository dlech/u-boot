// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek display pipeline component helpers
 *
 * Copyright (c) 2026 BayLibre, SAS.
 * Author: Julien Stephan <jstephan@baylibre.com>
 */

#include <asm/io.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>

#include "mtk_disp_comp.h"

void mtk_disp_comp_write(struct udevice *dev, u32 offset, u32 val)
{
	struct mtk_disp_comp_priv *priv = dev_get_priv(dev);

	writel(val, priv->base + offset);
}

int mtk_disp_comp_enable(struct udevice *dev)
{
	struct mtk_disp_comp_priv *priv = dev_get_priv(dev);
	int ret;

	/* ungate the feeding SMI larb first, if any */
	if (priv->larb) {
		ret = mtk_disp_comp_enable(priv->larb);
		if (ret)
			return ret;
	}

	ret = clk_enable_bulk(&priv->clk_bulk);
	if (ret) {
		dev_err(dev, "failed to enable clocks: %d\n", ret);
		if (priv->larb)
			mtk_disp_comp_disable(priv->larb);
		return ret;
	}

	return 0;
}

int mtk_disp_comp_disable(struct udevice *dev)
{
	struct mtk_disp_comp_priv *priv = dev_get_priv(dev);
	int ret;

	ret = clk_disable_bulk(&priv->clk_bulk);
	if (ret)
		dev_err(dev, "failed to disable clocks: %d\n", ret);

	if (priv->larb)
		mtk_disp_comp_disable(priv->larb);

	return ret;
}

int mtk_disp_comp_probe(struct udevice *dev)
{
	struct mtk_disp_comp_priv *priv = dev_get_priv(dev);
	int ret;

	priv->base = dev_remap_addr(dev);
	if (!priv->base)
		return -EINVAL;

	ret = clk_get_bulk(dev, &priv->clk_bulk);
	if (ret) {
		dev_err(dev, "failed to get clocks: %d\n", ret);
		return ret;
	}

	return 0;
}
