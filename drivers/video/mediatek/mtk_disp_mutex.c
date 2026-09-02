// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mediatek Video Disp Mutex Support
 *
 * Copyright (c) 2026 BayLibre, SAS.
 * Author: Julien Stephan <jstephan@baylibre.com>
 */

#include <dm.h>

#include "mtk_disp_comp.h"
#include "mtk_disp_mutex.h"

/*
 * Register layout of one mutex instance @id (as in the Linux mtk-mutex
 * driver): the block packs the per-mutex registers 0x20 apart. SOF holds
 * the start/end-of-frame source, MOD0/MOD1 the attached modules 0-31 and
 * 32-63. The offsets are index-based so a future DSI helper can drive a
 * different instance/controller.
 */
#define DISP_MUTEX_EN(id)	(0x20 + 0x20 * (id))
#define DISP_MUTEX_SOF(id)	(0x2c + 0x20 * (id))
#define DISP_MUTEX_MOD0(id)	(0x30 + 0x20 * (id))
#define DISP_MUTEX_MOD1(id)	(0x34 + 0x20 * (id))

/*
 * The DPI1 -> HDMI pipeline uses mutex instance 1 of the vdosys1 mutex
 * controller. Module bit indices are as in the Linux mtk-mutex driver
 * (MT8188_MUTEX_MOD_DISP1_*).
 */
#define MUTEX_HDMI_ID		1
#define MUTEX_SOF_DPI1		0x5
#define MUTEX_EOF_DPI1		(MUTEX_SOF_DPI1 << 7)
#define MUTEX_MOD0_MDP_RDMA4	BIT(4)
#define MUTEX_MOD0_MDP_RDMA5	BIT(5)
#define MUTEX_MOD0_PADDING4	BIT(12)
#define MUTEX_MOD0_PADDING5	BIT(13)
#define MUTEX_MOD0_VPP_MERGE2	BIT(22)
#define MUTEX_MOD0_VPP_MERGE4	BIT(24)
#define MUTEX_MOD0_DISP_MIXER	BIT(30)
#define MUTEX_MOD1_DPI1		BIT(38 - 32)

void mtk_disp_mutex_config_hdmi(struct udevice *dev)
{
	mtk_disp_comp_write(dev, DISP_MUTEX_SOF(MUTEX_HDMI_ID),
			    MUTEX_EOF_DPI1 | MUTEX_SOF_DPI1);
	mtk_disp_comp_write(dev, DISP_MUTEX_MOD0(MUTEX_HDMI_ID),
			    MUTEX_MOD0_MDP_RDMA4 | MUTEX_MOD0_MDP_RDMA5 |
			    MUTEX_MOD0_PADDING4 | MUTEX_MOD0_PADDING5 |
			    MUTEX_MOD0_VPP_MERGE2 | MUTEX_MOD0_VPP_MERGE4 |
			    MUTEX_MOD0_DISP_MIXER);
	mtk_disp_comp_write(dev, DISP_MUTEX_MOD1(MUTEX_HDMI_ID), MUTEX_MOD1_DPI1);
	mtk_disp_comp_write(dev, DISP_MUTEX_EN(MUTEX_HDMI_ID), 0x1);
}

static const struct udevice_id mtk_disp_mutex_ids[] = {
	{ .compatible = "mediatek,mt8188-disp-mutex" },
	{}
};

U_BOOT_DRIVER(mtk_disp_mutex) = {
	.name	   = "mtk_disp_mutex",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_disp_mutex_ids,
	.probe	   = mtk_disp_comp_probe,
	.priv_auto = sizeof(struct mtk_disp_comp_priv),
};
