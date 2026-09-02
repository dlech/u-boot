// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek Video Overlay support
 *
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#include <asm/io.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>
#include <video.h>

#include "mtk_smi.h"

#define DISP_REG_OVL_INTSTA	   0x008

#define DISP_REG_OVL_EN		   0x00c
#define OVL_EN				  BIT(0)

#define DISP_REG_OVL_RST	   0x014
#define DISP_REG_OVL_ROI_SIZE	   0x020
#define DISP_REG_OVL_ROI_BGCLR	   0x028
#define OVL_COLOR_ALPHA			  0xff000000
#define DISP_REG_OVL_DATAPATH_CON  0x024
#define OVL_LAYER_SMI_ID_EN	  BIT(0)
#define OVL_OUTPUT_CLAMP	  BIT(26)

#define DISP_REG_OVL_SRC_CON	   0x02c
#define L0_EN				  BIT(0)

#define DISP_REG_OVL_CON	   0x030
#define OVL_CON_CLRFMT_ARGB8888		  (2 << 12)

#define DISP_REG_OVL_SRC_SIZE	   0x038
#define DISP_REG_OVL_ADDR	   0xf40
#define DISP_REG_OVL_PITCH	   0x044
#define OVL_CONST_BLEND			  BIT(28)

#define DISP_REG_OVL_RDMA0_CTRL	   0x0c0
#define RDMA0_EN			  BIT(0)
#define DISP_REG_OVL_RDMA_GMC	   0x0c8
#define GMC_THRESHOLD_BITS	   16
#define GMC_THRESHOLD_HIGH	   ((1 << GMC_THRESHOLD_BITS) / 4)
#define GMC_THRESHOLD_LOW	   ((1 << GMC_THRESHOLD_BITS) / 8)

#define DISP_REG_OVL_CLRFMT_EXT	   0x2d0
#define OVL_CON_CLRFMT_BIT_DEPTH_MASK(idx)	  (0xff << (4 * (idx)))
#define OVL_CON_CLRFMT_8_BIT		  0x00

struct mtk_disp_ovl_data {
	bool smi_id_en;
	bool supports_clrfmt_ext;
	unsigned int gmc_bits;
};

struct mtk_disp_ovl_priv {
	void __iomem *base;
	struct udevice *dev;
	struct udevice *larb;
	struct clk clk;
	const struct mtk_disp_ovl_data *data;
};

void mtk_ovl_config(struct udevice *dev, struct display_timing timing)
{
	struct mtk_disp_ovl_priv *ovl = dev_get_priv(dev);
	u32 width = timing.hactive.typ;
	u32 height = timing.vactive.typ;

	if (width != 0 && height != 0)
		writel(height << 16 | width, ovl->base + DISP_REG_OVL_ROI_SIZE);

	/* Background must be opaque black, or alpha blending has no effect. */
	writel(OVL_COLOR_ALPHA, ovl->base + DISP_REG_OVL_ROI_BGCLR);

	writel(1, ovl->base + DISP_REG_OVL_RST);
	writel(0, ovl->base + DISP_REG_OVL_RST);
}

/*
 * Layer 0 is always fed our fixed 8-bit-per-channel XRGB8888 framebuffer, so
 * the 10-bit-format check real Linux does here never applies to us.
 */
static void mtk_ovl_set_bit_depth(struct mtk_disp_ovl_priv *ovl)
{
	u32 reg;

	if (!ovl->data->supports_clrfmt_ext)
		return;

	reg = readl(ovl->base + DISP_REG_OVL_CLRFMT_EXT);
	reg &= ~OVL_CON_CLRFMT_BIT_DEPTH_MASK(0);
	writel(reg, ovl->base + DISP_REG_OVL_CLRFMT_EXT);
}

static void mtk_ovl_layer_on(struct mtk_disp_ovl_priv *ovl)
{
	u32 reg, gmc_thrshd_l, gmc_thrshd_h;

	writel(RDMA0_EN, ovl->base + DISP_REG_OVL_RDMA0_CTRL);

	gmc_thrshd_l = GMC_THRESHOLD_LOW >> (GMC_THRESHOLD_BITS - ovl->data->gmc_bits);
	gmc_thrshd_h = GMC_THRESHOLD_HIGH >> (GMC_THRESHOLD_BITS - ovl->data->gmc_bits);
	if (ovl->data->gmc_bits == 10)
		reg = gmc_thrshd_h | gmc_thrshd_h << 16;
	else
		reg = gmc_thrshd_l | gmc_thrshd_l << 8 | gmc_thrshd_h << 16 | gmc_thrshd_h << 24;
	writel(reg, ovl->base + DISP_REG_OVL_RDMA_GMC);

	reg = readl(ovl->base + DISP_REG_OVL_SRC_CON);
	reg |= L0_EN;
	writel(reg, ovl->base + DISP_REG_OVL_SRC_CON);
}

void mtk_ovl_layer(struct udevice *dev, struct video_uc_plat *plat, struct display_timing timing)
{
	struct mtk_disp_ovl_priv *ovl = dev_get_priv(dev);
	u32 width = timing.hactive.typ;
	u32 height = timing.vactive.typ;
	u32 src_size = (height << 16) | width;

	writel(OVL_CON_CLRFMT_ARGB8888, ovl->base + DISP_REG_OVL_CON);

	/*
	 * The framebuffer has no real per-pixel alpha channel, so without
	 * OVL_CONST_BLEND the hardware reads the (zero) alpha byte from
	 * memory and blends the whole layer as fully transparent.
	 */
	writel(width * VNBYTES(VIDEO_BPP32) | OVL_CONST_BLEND, ovl->base + DISP_REG_OVL_PITCH);

	writel(src_size, ovl->base + DISP_REG_OVL_SRC_SIZE);

	writel(plat->base, ovl->base + DISP_REG_OVL_ADDR);

	mtk_ovl_set_bit_depth(ovl);

	mtk_ovl_layer_on(ovl);
}

void mtk_ovl_start(struct udevice *dev)
{
	struct mtk_disp_ovl_priv *ovl = dev_get_priv(dev);
	u32 reg;

	if (ovl->data->smi_id_en) {
		reg = readl(ovl->base + DISP_REG_OVL_DATAPATH_CON);
		reg |= OVL_LAYER_SMI_ID_EN | OVL_OUTPUT_CLAMP;
		writel(reg, ovl->base + DISP_REG_OVL_DATAPATH_CON);
	}

	writel(OVL_EN, ovl->base + DISP_REG_OVL_EN);
}

int mtk_ovl_enable(struct udevice *dev)
{
	struct mtk_disp_ovl_priv *ovl = dev_get_priv(dev);
	int ret;

	ret = clk_enable(&ovl->clk);
	if (ret)
		dev_err(ovl->dev, "failed to enable clk: %s\n", errno_str(ret));

	return ret;
}

int mtk_ovl_disable(struct udevice *dev)
{
	struct mtk_disp_ovl_priv *ovl = dev_get_priv(dev);
	u32 reg;
	int ret;

	if (ovl->data->smi_id_en) {
		reg = readl(ovl->base + DISP_REG_OVL_DATAPATH_CON);
		reg &= ~OVL_LAYER_SMI_ID_EN;
		writel(reg, ovl->base + DISP_REG_OVL_DATAPATH_CON);
	}

	ret = clk_disable(&ovl->clk);
	if (ret)
		dev_err(ovl->dev, "failed to disable clk: %s\n", errno_str(ret));

	return ret;
}

int mtk_ovl_smi_enable(struct udevice *dev)
{
	struct mtk_disp_ovl_priv *ovl = dev_get_priv(dev);
	int ret;

	ret = mtk_smi_larb_enable(ovl->larb);
	if (ret) {
		dev_err(ovl->dev, "failed to enable larb: %s\n", errno_str(ret));
		return ret;
	}

	mtk_smi_enable_pa(ovl->larb, M4U_PORT_DISP_OVL0);

	return 0;
}

int mtk_ovl_smi_disable(struct udevice *dev)
{
	struct mtk_disp_ovl_priv *ovl = dev_get_priv(dev);
	int ret;

	ret = mtk_smi_larb_disable(ovl->larb);
	if (ret) {
		dev_err(ovl->dev, "failed to disable larb: %s\n", errno_str(ret));
		return ret;
	}

	mtk_smi_disable_pa(ovl->larb, M4U_PORT_DISP_OVL0);

	return 0;
}

static int mtk_disp_ovl_probe(struct udevice *dev)
{
	struct mtk_disp_ovl_priv *ovl = dev_get_priv(dev);
	int ret;

	ovl->dev = dev;
	ovl->data = (const struct mtk_disp_ovl_data *)dev_get_driver_data(dev);

	ovl->base = dev_remap_addr(dev);
	if (IS_ERR(ovl->base))
		return PTR_ERR(ovl->base);

	ret = clk_get_by_index(dev, 0, &ovl->clk);
	if (ret)
		return ret;

	ret = uclass_get_device_by_of_path(UCLASS_MISC, "larb0", &ovl->larb);
	if (ret) {
		dev_err(dev, "cannot get larb device: %s\n", errno_str(ret));
		return ret;
	}

	return 0;
}

static const struct mtk_disp_ovl_data mt8189_ovl_driver_data = {
	.smi_id_en = true,
	.supports_clrfmt_ext = true,
	.gmc_bits = 10,
};

static const struct mtk_disp_ovl_data mt8192_ovl_driver_data = {
	.smi_id_en = true,
	.gmc_bits = 10,
};

static const struct mtk_disp_ovl_data mt8366_ovl_driver_data = {
	.smi_id_en = true,
	.supports_clrfmt_ext = true,
	.gmc_bits = 10,
};

static const struct udevice_id mtk_disp_ovl_ids[] = {
	{ .compatible = "mediatek,mt8189-disp-ovl", .data = (ulong)&mt8189_ovl_driver_data },
	{ .compatible = "mediatek,mt8192-disp-ovl", .data = (ulong)&mt8192_ovl_driver_data },
	{ .compatible = "mediatek,mt8366-disp-ovl", .data = (ulong)&mt8366_ovl_driver_data },
	{}
};

U_BOOT_DRIVER(mtk_disp_ovl) = {
	.name	   = "mtk_disp_ovl",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_disp_ovl_ids,
	.probe	   = mtk_disp_ovl_probe,
	.priv_auto = sizeof(struct mtk_disp_ovl_priv),
};
