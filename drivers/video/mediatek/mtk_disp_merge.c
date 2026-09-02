// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mediatek Video Disp Merge Support
 *
 * Copyright (c) 2026 BayLibre, SAS.
 * Author: Julien Stephan <jstephan@baylibre.com>
 */

#include <dm.h>

#include "mtk_disp_comp.h"
#include "mtk_disp_merge.h"

#define DISP_REG_MERGE_CTRL		0x000
#define MERGE_EN				1
#define DISP_REG_MERGE_CFG_0		0x010
#define DISP_REG_MERGE_CFG_1		0x014
#define DISP_REG_MERGE_CFG_4		0x020
#define DISP_REG_MERGE_CFG_10		0x038
#define DISP_REG_MERGE_CFG_12		0x040
#define CFG_10_10_2PI_2PO_BUF_MODE	8
#define CFG_11_10_1PI_2PO_MERGE		18
#define DISP_REG_MERGE_CFG_24		0x070
#define DISP_REG_MERGE_CFG_25		0x074
#define DISP_REG_MERGE_CFG_26		0x078
#define DISP_REG_MERGE_CFG_27		0x07c
#define DISP_REG_MERGE_MUTE_0		0xf00

/* CFG_30 (0x088, "12 bit lsb off") is not present in the upstream driver */
#define DISP_REG_MERGE_CFG_30		0x088

void mtk_disp_merge_config(struct udevice *dev,
			   u16 width1, u16 height1,
			   u16 width2, u16 height2,
			   u16 output_width, u16 output_height)
{
	bool dual_input = (width2 != 0 && height2 != 0);

	/* input 1 */
	if (width1 != 0 && height1 != 0)
		mtk_disp_comp_write(dev, DISP_REG_MERGE_CFG_0,
				    height1 << 16 | width1);

	/* input 2 */
	if (dual_input)
		mtk_disp_comp_write(dev, DISP_REG_MERGE_CFG_1,
				    height2 << 16 | width2);

	/* output */
	mtk_disp_comp_write(dev, DISP_REG_MERGE_CFG_4,
			    output_height << 16 | output_width);

	/* no pixel/channel swap */
	mtk_disp_comp_write(dev, DISP_REG_MERGE_CFG_10, 0x0);

	if (dual_input)
		mtk_disp_comp_write(dev, DISP_REG_MERGE_CFG_12,
				    CFG_11_10_1PI_2PO_MERGE);
	else
		mtk_disp_comp_write(dev, DISP_REG_MERGE_CFG_12,
				    CFG_10_10_2PI_2PO_BUF_MODE);

	/* size in sram of inputs 0 and 1 */
	if (width1 != 0 && height1 != 0)
		mtk_disp_comp_write(dev, DISP_REG_MERGE_CFG_24,
				    height1 << 16 | width1);

	if (dual_input)
		mtk_disp_comp_write(dev, DISP_REG_MERGE_CFG_25,
				    height2 << 16 | width2);
	else
		mtk_disp_comp_write(dev, DISP_REG_MERGE_CFG_25,
				    height1 << 16 | width1);

	/* merged size of inputs 0 and 1 */
	if (width1 != 0 && height1 != 0)
		mtk_disp_comp_write(dev, DISP_REG_MERGE_CFG_26,
				    height1 << 16 | width1);

	if (dual_input)
		mtk_disp_comp_write(dev, DISP_REG_MERGE_CFG_27,
				    height2 << 16 | width2);
	else
		mtk_disp_comp_write(dev, DISP_REG_MERGE_CFG_27,
				    height1 << 16 | width1);

	/* 12 bit lsb off */
	mtk_disp_comp_write(dev, DISP_REG_MERGE_CFG_30, 0x0);

	/* unmute */
	mtk_disp_comp_write(dev, DISP_REG_MERGE_MUTE_0, 0x0);

	/* enable */
	mtk_disp_comp_write(dev, DISP_REG_MERGE_CTRL, MERGE_EN);
}

static const struct udevice_id mtk_disp_merge_ids[] = {
	{ .compatible = "mediatek,mt8195-disp-merge" },
	{}
};

U_BOOT_DRIVER(mtk_disp_merge) = {
	.name	   = "mtk_disp_merge",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_disp_merge_ids,
	.probe	   = mtk_disp_comp_probe,
	.priv_auto = sizeof(struct mtk_disp_comp_priv),
};
