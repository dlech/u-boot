// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek Display Data Path
 *
 * Copyright (c) 2022 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#include <asm/io.h>
#include <linux/kernel.h>

#define DISP_REG_CONFIG_DISP_OVL0_MOUT_EN	    0xf3c
#define DISP_REG_CONFIG_DISP_RDMA0_RSZ0_IN_SOUT_SEL 0xf48
#define DISP_REG_CONFIG_RDMA0_SOUT_SEL		    0xf4c
#define DISP_REG_CONFIG_DITHER0_MOUNT_EN	    0xf50
#define DISP_REG_CONFIG_DISP_PATH0_SEL_IN	    0xf54
#define DISP_REG_CONFIG_DISP_RDMA0_RSZ0_SEL_IN	    0xf60
#define DISP_REG_CONFIG_DISP_COLOR0_OUT_SEL_IN	    0xf64
#define DISP_REG_CONFIG_DSI0_SEL_IN		    0xf68
#define DISP_REG_CONFIG_DISP_RDMA1_SOUT_SEL	    0xfd0
#define DISP_REG_CONFIG_DISP_DPI0_SEL_IN	    0xfd8
#define DISP_REG_CONFIG_DISP_LVDS_SYS_CFG_00	    0xfdc

/*
 * mt8366-generation mmsys uses a multi-stage crossbar instead of the
 * simple mt8183-style direct mux registers above.
 */
#define MT8366_DISP_OVL0_BGCLR_MOUT_EN		    0xe24
#define MT8366_MOUT_OVL_TO_BLENDOUT		    BIT(0)
#define MT8366_DISP_OVL_BGCLR_MOUT_MASK	    0x3

#define MT8366_DISP_OVL0_OUT0_MOUT_EN		    0xc10
#define MT8366_MOUT_DISP_OVL0_TO_DISP_RDMA0	    BIT(1)
#define MT8366_DISP_OVL_OUT0_MOUT_MASK		    0x7

#define MT8366_DISP_RDMA0_SEL_IN		    0xe04
#define MT8366_SEL_IN_DISP_RDMA0_FROM_DISP_OVL0_OUT0_MOUT 1
#define MT8366_DISP_RDMA_SEL_IN_MASK		    0x1

#define MT8366_DISP_RDMA0_RSZ0_SOUT_SEL	    0xe00
#define MT8366_SOUT_DISP_RDMA0_RSZ0_TO_OVL_PQ_OUT_CROSSBAR1 0

#define MT8366_OVL_PQ_OUT_CROSSBAR1_MOUT_EN	    0xc78
#define MT8366_MOUT_OVL_PQ_OUT_CROSSBAR1_TO_COMP_OUT_CROSSBAR4 BIT(4)
#define MT8366_OVL_PQ_OUT_CROSSBAR_MOUT_MASK	    0x3f

#define MT8366_COMP_OUT_CROSSBAR4_MOUT_EN	    0xd80
#define MT8366_MOUT_COMP_OUT_CROSSBAR4_TO_DISP_DSI0 BIT(0)
#define MT8366_COMP_OUT_CROSSBAR_MOUT_MASK	    0x3f

static void mtk_ddp_mask(void __iomem *base, u32 offset, u32 val, u32 mask)
{
	u32 tmp = readl(base + offset) & ~mask;

	writel(tmp | (val & mask), base + offset);
}

static void mtk_ddp_ovl_to_dsi_color_pipeline(void __iomem *mmsys_base)
{
	/* OVL0 -> RDMA0 */
	writel(1, mmsys_base + DISP_REG_CONFIG_DISP_OVL0_MOUT_EN);
	writel(0, mmsys_base + DISP_REG_CONFIG_DISP_PATH0_SEL_IN);

	/* RDMA0 -> COLOR0 */
	writel(0, mmsys_base + DISP_REG_CONFIG_DISP_RDMA0_RSZ0_IN_SOUT_SEL);
	writel(0, mmsys_base + DISP_REG_CONFIG_DISP_RDMA0_RSZ0_SEL_IN);
	writel(1, mmsys_base + DISP_REG_CONFIG_RDMA0_SOUT_SEL);

	/* COLOR0 -> CCORR0 -> AAL0 -> GAMMA0 -> DITHER0 -> DSI0 */
	writel(0, mmsys_base + DISP_REG_CONFIG_DISP_COLOR0_OUT_SEL_IN);
	writel(1, mmsys_base + DISP_REG_CONFIG_DITHER0_MOUNT_EN);
	writel(1, mmsys_base + DISP_REG_CONFIG_DSI0_SEL_IN);
}

static void mtk_ddp_ovl_to_dsi_direct_path(void __iomem *mmsys_base)
{
	/* OVL0 -> RDMA0 */
	mtk_ddp_mask(mmsys_base, MT8366_DISP_OVL0_BGCLR_MOUT_EN,
		     MT8366_MOUT_OVL_TO_BLENDOUT, MT8366_DISP_OVL_BGCLR_MOUT_MASK);
	mtk_ddp_mask(mmsys_base, MT8366_DISP_OVL0_OUT0_MOUT_EN,
		     MT8366_MOUT_DISP_OVL0_TO_DISP_RDMA0, MT8366_DISP_OVL_OUT0_MOUT_MASK);
	mtk_ddp_mask(mmsys_base, MT8366_DISP_RDMA0_SEL_IN,
		     MT8366_SEL_IN_DISP_RDMA0_FROM_DISP_OVL0_OUT0_MOUT,
		     MT8366_DISP_RDMA_SEL_IN_MASK);

	/* RDMA0 -> COMP_OUT_CROSSBAR4 -> DSI0 */
	mtk_ddp_mask(mmsys_base, MT8366_DISP_RDMA0_RSZ0_SOUT_SEL,
		     MT8366_SOUT_DISP_RDMA0_RSZ0_TO_OVL_PQ_OUT_CROSSBAR1,
		     MT8366_OVL_PQ_OUT_CROSSBAR_MOUT_MASK);
	mtk_ddp_mask(mmsys_base, MT8366_OVL_PQ_OUT_CROSSBAR1_MOUT_EN,
		     MT8366_MOUT_OVL_PQ_OUT_CROSSBAR1_TO_COMP_OUT_CROSSBAR4,
		     MT8366_OVL_PQ_OUT_CROSSBAR_MOUT_MASK);
	mtk_ddp_mask(mmsys_base, MT8366_COMP_OUT_CROSSBAR4_MOUT_EN,
		     MT8366_MOUT_COMP_OUT_CROSSBAR4_TO_DISP_DSI0,
		     MT8366_COMP_OUT_CROSSBAR_MOUT_MASK);
}

void mtk_ddp_ovl_to_dsi(void __iomem *mmsys_base, bool has_color_pipeline)
{
	if (has_color_pipeline)
		mtk_ddp_ovl_to_dsi_color_pipeline(mmsys_base);
	else
		mtk_ddp_ovl_to_dsi_direct_path(mmsys_base);
}

void mtk_ddp_rdma_to_dpi(void __iomem *mmsys_base)
{
	writel(BIT(0), mmsys_base + DISP_REG_CONFIG_DISP_RDMA1_SOUT_SEL);

	writel(0, mmsys_base + DISP_REG_CONFIG_DISP_DPI0_SEL_IN);

	writel(BIT(0), mmsys_base + DISP_REG_CONFIG_DISP_LVDS_SYS_CFG_00);
}
