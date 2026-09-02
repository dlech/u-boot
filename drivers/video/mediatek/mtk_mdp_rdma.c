// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mediatek Video MDP RDMA Support
 *
 * Copyright (c) 2026 BayLibre, SAS.
 * Author: Julien Stephan <jstephan@baylibre.com>
 */

#include <dm.h>
#include <dm/device_compat.h>
#include <dm/ofnode.h>
#include <dt-bindings/memory/mtk-memory-port.h>
#include <linux/bitfield.h>
#include <video.h>

#include "mtk_disp_comp.h"
#include "mtk_mdp_rdma.h"

/* MDP RDMA registers (see Linux drivers/gpu/drm/mediatek/mtk_mdp_rdma.c) */
#define MDP_RDMA_EN			0x000
#define FLD_ROT_ENABLE				BIT(0)
#define MDP_RDMA_CON			0x020
#define FLD_SIMPLE_MODE				BIT(4)
#define MDP_RDMA_SRC_CON		0x030
#define FLD_OUTPUT_ARGB				BIT(25)
#define FLD_UNIFORM_CONFIG			BIT(17)
#define FLD_SRC_FORMAT				GENMASK(3, 0)
#define MDP_RDMA_MF_BKGD_SIZE_IN_BYTE	0x060
#define MDP_RDMA_MF_SRC_SIZE		0x070
#define FLD_MF_SRC_H				GENMASK(30, 16)
#define FLD_MF_SRC_W				GENMASK(14, 0)
#define MDP_RDMA_MF_CLIP_SIZE		0x078
#define FLD_MF_CLIP_H				GENMASK(30, 16)
#define FLD_MF_CLIP_W				GENMASK(14, 0)
#define MDP_RDMA_SRC_OFFSET_0		0x118
#define MDP_RDMA_TRANSFORM_0		0x200
#define MDP_RDMA_SRC_BASE_0		0xf00

/* Fields/values used by this driver but not present in the upstream driver */
#define FLD_INTERNAL_DCM_EN			GENMASK(23, 4)
#define MDP_RDMA_SRC_FORMAT_BGRA8888		2
#define FLD_BITEXTEND_ZERO			BIT(15)
#define FB_BYTES_PER_PIXEL			4

int mtk_mdp_rdma_config(struct udevice *dev, struct video_uc_plat *plat,
			const struct display_timing *timing,
			unsigned int pitch, bool apply_offset)
{
	u32 width = timing->hactive.typ;
	u32 height = timing->vactive.typ;
	int offset = apply_offset ? ((width / 2) * FB_BYTES_PER_PIXEL) : 0;
	u32 fb;

	/* the RDMA source address register is 32-bit only */
	if (plat->base != (u32)plat->base) {
		dev_err(dev, "framebuffer base %pa is above 4GB\n", &plat->base);
		return -EINVAL;
	}
	fb = plat->base;

	mtk_disp_comp_write(dev, MDP_RDMA_EN, FLD_INTERNAL_DCM_EN);

	/* set source format and output bit depth */
	mtk_disp_comp_write(dev, MDP_RDMA_SRC_CON,
			    FIELD_PREP(FLD_SRC_FORMAT, MDP_RDMA_SRC_FORMAT_BGRA8888) |
			    FLD_OUTPUT_ARGB |
			    FLD_UNIFORM_CONFIG);

	/* set base address */
	mtk_disp_comp_write(dev, MDP_RDMA_SRC_BASE_0, fb);

	/* stride comes from the framebuffer line length */
	mtk_disp_comp_write(dev, MDP_RDMA_MF_BKGD_SIZE_IN_BYTE, pitch);

	/*
	 * each of the two RDMAs reads one half of the frame: size and
	 * clip cover half the width, and the second RDMA (apply_offset)
	 * starts at the middle of the line
	 */
	mtk_disp_comp_write(dev, MDP_RDMA_MF_SRC_SIZE,
			    FIELD_PREP(FLD_MF_SRC_H, height) |
			    FIELD_PREP(FLD_MF_SRC_W, width / 2));
	mtk_disp_comp_write(dev, MDP_RDMA_MF_CLIP_SIZE,
			    FIELD_PREP(FLD_MF_CLIP_H, height) |
			    FIELD_PREP(FLD_MF_CLIP_W, width / 2));
	mtk_disp_comp_write(dev, MDP_RDMA_SRC_OFFSET_0, offset);

	/* simple mode + zero bit extension for 8bit -> 10bit */
	mtk_disp_comp_write(dev, MDP_RDMA_CON, FLD_SIMPLE_MODE);
	mtk_disp_comp_write(dev, MDP_RDMA_TRANSFORM_0, FLD_BITEXTEND_ZERO);

	/* enable engine */
	mtk_disp_comp_write(dev, MDP_RDMA_EN,
			    FLD_INTERNAL_DCM_EN | FLD_ROT_ENABLE);

	return 0;
}

/*
 * The RDMA is the DMA master, so the SMI larb feeding it must be ungated for
 * its DRAM access. The RDMA points at its IOMMU (M4U) port through "iommus";
 * MTK_M4U_TO_LARB() extracts the larb id from that port. Resolve the matching
 * larb in the IOMMU's "mediatek,larbs" list by its "mediatek,larb-id"; the
 * component helper then enables and disables it together with the RDMA.
 */
static int mtk_mdp_rdma_get_larb(struct udevice *dev)
{
	struct mtk_disp_comp_priv *priv = dev_get_priv(dev);
	struct ofnode_phandle_args args;
	ofnode iommu, node;
	u32 larb_id, id;
	int i, ret;

	ret = dev_read_phandle_with_args(dev, "iommus", "#iommu-cells", 0, 0,
					 &args);
	if (ret) {
		dev_err(dev, "cannot parse iommus: %d\n", ret);
		return ret;
	}
	iommu = args.node;
	larb_id = MTK_M4U_TO_LARB(args.args[0]);

	for (i = 0; ; i++) {
		node = ofnode_parse_phandle(iommu, "mediatek,larbs", i);
		if (!ofnode_valid(node)) {
			dev_err(dev, "no larb %u in iommu\n", larb_id);
			return -ENODEV;
		}
		if (!ofnode_read_u32(node, "mediatek,larb-id", &id) &&
		    id == larb_id)
			break;
	}

	return uclass_get_device_by_ofnode(UCLASS_MISC, node, &priv->larb);
}

static int mtk_mdp_rdma_probe(struct udevice *dev)
{
	int ret;

	ret = mtk_disp_comp_probe(dev);
	if (ret)
		return ret;

	return mtk_mdp_rdma_get_larb(dev);
}

static const struct udevice_id mtk_mdp_rdma_ids[] = {
	{ .compatible = "mediatek,mt8195-vdo1-rdma" },
	{}
};

U_BOOT_DRIVER(mtk_mdp_rdma) = {
	.name	   = "mtk_mdp_rdma",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_mdp_rdma_ids,
	.probe	   = mtk_mdp_rdma_probe,
	.priv_auto = sizeof(struct mtk_disp_comp_priv),
};
