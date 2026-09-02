// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek Display MUTEX
 *
 * Copyright (c) 2022 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#include <asm/io.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>

#define DISP_MUTEX_CFG		  0x08
#define MUTEX_DISABLE_CLK_GATING	0

#define DISP_MUTEX0_EN		  0x20
#define MUTEX_EN			BIT(0)

/*
 * mt8365/mt8183-generation "simple" mutex layout, where SOF/EOF and MOD are
 * plain independent bit flags.
 */
#define DISP_MUTEX0_CTL		  0x2c
#define MUTEX_SOF_DSI0			BIT(0)
#define MUTEX_SOF_DPI0			BIT(1)
#define MUTEX_EOF_DSI0			BIT(6)
#define MUTEX_EOF_DPI0			BIT(7)

#define DISP_MUTEX0_MOD0	  0x30
#define MUTEX_MOD_DISP_OVL0		BIT(7)
#define MUTEX_MOD_DISP_RDMA0		BIT(9)
#define MUTEX_MOD_DISP_RDMA1		BIT(10)
#define MUTEX_MOD_DISP_COLOR0		BIT(12)
#define MUTEX_MOD_DISP_CCORR0		BIT(13)
#define MUTEX_MOD_DISP_AAL0		BIT(14)
#define MUTEX_MOD_DISP_GAMMA0		BIT(15)
#define MUTEX_MOD_DISP_DITHER0		BIT(16)

/*
 * mt8366-generation mutex layout: MOD bits are addressed by plain index
 * (not a fixed BIT() position), and the SOF register packs the SOF
 * selector in the low bits with the matching EOF selector shifted by 7.
 */
#define MT8366_DISP_MUTEX0_SOF	  0x2c
#define MT8366_MUTEX_SOF_DSI0		1
#define MT8366_MUTEX_EOF_DSI0		(MT8366_MUTEX_SOF_DSI0 << 7)

#define MT8366_DISP_MUTEX0_MOD0  0x30
#define MT8366_MUTEX_MOD_DISP_OVL0	0
#define MT8366_MUTEX_MOD_DISP_RDMA0	4
#define MT8366_MUTEX_MOD_DISP_DSI0	13

struct mtk_disp_mutex_priv {
	void __iomem *base;
	struct clk clk;
	const struct mtk_disp_mutex_data *data;
};

struct mtk_disp_mutex_data {
	void (*ovl_dsi_enable)(struct mtk_disp_mutex_priv *mutex);
	bool has_clk;
};

static void mtk_disp_mutex_ovl_dsi_enable_color_pipeline(struct mtk_disp_mutex_priv *mutex)
{
	writel(MUTEX_DISABLE_CLK_GATING, mutex->base + DISP_MUTEX_CFG);

	writel(MUTEX_MOD_DISP_OVL0   |
	       MUTEX_MOD_DISP_RDMA0  |
	       MUTEX_MOD_DISP_COLOR0 |
	       MUTEX_MOD_DISP_CCORR0 |
	       MUTEX_MOD_DISP_AAL0   |
	       MUTEX_MOD_DISP_GAMMA0 |
	       MUTEX_MOD_DISP_DITHER0,
	       mutex->base + DISP_MUTEX0_MOD0);

	writel(MUTEX_SOF_DSI0 | MUTEX_EOF_DSI0, mutex->base + DISP_MUTEX0_CTL);

	writel(MUTEX_EN, mutex->base + DISP_MUTEX0_EN);
}

static void mtk_disp_mutex_ovl_dsi_enable_direct_path(struct mtk_disp_mutex_priv *mutex)
{
	writel(MUTEX_DISABLE_CLK_GATING, mutex->base + DISP_MUTEX_CFG);

	writel(BIT(MT8366_MUTEX_MOD_DISP_OVL0) |
	       BIT(MT8366_MUTEX_MOD_DISP_RDMA0) |
	       BIT(MT8366_MUTEX_MOD_DISP_DSI0),
	       mutex->base + MT8366_DISP_MUTEX0_MOD0);

	writel(MT8366_MUTEX_SOF_DSI0 | MT8366_MUTEX_EOF_DSI0,
	       mutex->base + MT8366_DISP_MUTEX0_SOF);

	writel(MUTEX_EN, mutex->base + DISP_MUTEX0_EN);
}

static const struct mtk_disp_mutex_data mt8365_mutex_data = {
	.ovl_dsi_enable = mtk_disp_mutex_ovl_dsi_enable_color_pipeline,
};

static const struct mtk_disp_mutex_data mt8366_mutex_data = {
	.ovl_dsi_enable = mtk_disp_mutex_ovl_dsi_enable_direct_path,
	.has_clk = true,
};

void mtk_disp_mutex_ovl_dsi_enable(struct udevice *dev)
{
	struct mtk_disp_mutex_priv *mutex = dev_get_priv(dev);

	mutex->data->ovl_dsi_enable(mutex);
}

void mtk_disp_mutex_rdma_dpi_enable(struct udevice *dev)
{
	struct mtk_disp_mutex_priv *mutex = dev_get_priv(dev);

	writel(MUTEX_DISABLE_CLK_GATING, mutex->base + DISP_MUTEX_CFG);

	writel(MUTEX_MOD_DISP_RDMA1, mutex->base + DISP_MUTEX0_MOD0);

	writel(MUTEX_EOF_DPI0 | MUTEX_SOF_DPI0, mutex->base + DISP_MUTEX0_CTL);

	writel(MUTEX_EN, mutex->base + DISP_MUTEX0_EN);
}

static int mtk_disp_mutex_probe(struct udevice *dev)
{
	struct mtk_disp_mutex_priv *mutex = dev_get_priv(dev);
	int ret;

	mutex->data = (const struct mtk_disp_mutex_data *)dev_get_driver_data(dev);
	if (!mutex->data)
		return -EINVAL;

	mutex->base = dev_remap_addr(dev);
	if (IS_ERR(mutex->base))
		return PTR_ERR(mutex->base);

	if (!mutex->data->has_clk)
		return 0;

	ret = clk_get_by_index(dev, 0, &mutex->clk);
	if (ret)
		return ret;

	return clk_enable(&mutex->clk);
}

static const struct udevice_id mtk_disp_mutex_ids[] = {
	{ .compatible = "mediatek,mt8365-disp-mutex", .data = (ulong)&mt8365_mutex_data },
	{ .compatible = "mediatek,mt8366-disp-mutex", .data = (ulong)&mt8366_mutex_data },
	{}
};

U_BOOT_DRIVER(mtk_disp_mutex_mt8365) = {
	.name	   = "mtk_disp_mutex_mt8365",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_disp_mutex_ids,
	.probe	   = mtk_disp_mutex_probe,
	.priv_auto = sizeof(struct mtk_disp_mutex_priv),
};
