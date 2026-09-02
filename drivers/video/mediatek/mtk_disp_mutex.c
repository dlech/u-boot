// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mediatek Video Disp Mutex Support
 *
 * Copyright (c) 2022 BayLibre, SAS.
 * Copyright (c) 2026 BayLibre, SAS.
 * Authors: Julien Masson <jmasson@baylibre.com>
 *          Julien Stephan <jstephan@baylibre.com>
 */

#include <dm.h>
#include <linux/bitops.h>

#include "mtk_disp_comp.h"
#include "mtk_disp_mutex.h"

/*
 * Register layout of one mutex instance @id (as in the Linux mtk-mutex
 * driver): the block packs the per-mutex registers 0x20 apart. SOF holds
 * the start/end-of-frame source, MOD0/MOD1 the attached modules 0-31 and
 * 32-63.
 */
#define DISP_MUTEX_CFG		0x08
#define MUTEX_DISABLE_CLK_GATING       0

#define DISP_MUTEX_EN(id)	(0x20 + 0x20 * (id))
#define MUTEX_EN		       BIT(0)
#define DISP_MUTEX_SOF(id)	(0x2c + 0x20 * (id))
#define DISP_MUTEX_MOD0(id)	(0x30 + 0x20 * (id))
#define DISP_MUTEX_MOD1(id)	(0x34 + 0x20 * (id))

/* The OVL0 -> DSI0 pipeline always uses mutex instance 0. */
#define MUTEX_DSI_ID		0

/*
 * mt8365/mt8183-generation "simple" SOF layout, where SOF and EOF are
 * plain independent bit flags rather than selector values.
 */
#define MUTEX_SOF_FLAG_DSI0	       BIT(0)
#define MUTEX_EOF_FLAG_DSI0	       BIT(6)

/*
 * mt8188-generation SOF layout: the register packs the SOF selector in the
 * low bits with the matching EOF selector shifted by 7.
 */
#define MUTEX_SOF_SEL_DSI0	       1
#define MUTEX_EOF_SEL(sof)	       ((sof) << 7)

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

/*
 * MOD0 bits of the components taking part in the OVL0 -> DSI0 path. The
 * mt8365 generation places each module at a fixed BIT() position and has no
 * DSI0 module bit at all, while later generations address the modules by
 * plain index and do have one.
 */
struct mtk_disp_mutex_mod {
	u32 ovl0;
	u32 rdma0;
	u32 color0;
	u32 ccorr0;
	u32 aal0;
	u32 gamma0;
	u32 dither0;
	u32 dsi0;
};

struct mtk_disp_mutex_data {
	const struct mtk_disp_mutex_mod *mod;
	/* SOF/EOF are bit flags instead of selector values */
	bool sof_is_flag;
};

static const struct mtk_disp_mutex_mod mt8365_mutex_mod = {
	.ovl0	 = BIT(7),
	.rdma0	 = BIT(9),
	.color0	 = BIT(12),
	.ccorr0	 = BIT(13),
	.aal0	 = BIT(14),
	.gamma0	 = BIT(15),
	.dither0 = BIT(16),
};

static const struct mtk_disp_mutex_mod mt8366_mutex_mod = {
	.ovl0	 = BIT(0),
	.rdma0	 = BIT(4),
	.dsi0	 = BIT(13),
};

static const struct mtk_disp_mutex_mod mt8189_mutex_mod = {
	.ovl0	 = BIT(0),
	.rdma0	 = BIT(4),
	.dsi0	 = BIT(22),
};

static const struct mtk_disp_mutex_data mt8365_mutex_data = {
	.mod = &mt8365_mutex_mod,
	.sof_is_flag = true,
};

static const struct mtk_disp_mutex_data mt8366_mutex_data = {
	.mod = &mt8366_mutex_mod,
};

static const struct mtk_disp_mutex_data mt8189_mutex_data = {
	.mod = &mt8189_mutex_mod,
};

int mtk_disp_mutex_ovl_dsi_enable(struct udevice *dev)
{
	const struct mtk_disp_mutex_data *data =
		(const struct mtk_disp_mutex_data *)dev_get_driver_data(dev);
	const struct mtk_disp_mutex_mod *mod = data->mod;
	u32 sof;
	int ret;

	ret = mtk_disp_comp_enable(dev);
	if (ret)
		return ret;

	mtk_disp_comp_write(dev, DISP_MUTEX_CFG, MUTEX_DISABLE_CLK_GATING);

	mtk_disp_comp_write(dev, DISP_MUTEX_MOD0(MUTEX_DSI_ID),
			    mod->ovl0 | mod->rdma0 | mod->color0 | mod->ccorr0 |
			    mod->aal0 | mod->gamma0 | mod->dither0 | mod->dsi0);

	if (data->sof_is_flag)
		sof = MUTEX_SOF_FLAG_DSI0 | MUTEX_EOF_FLAG_DSI0;
	else
		sof = MUTEX_SOF_SEL_DSI0 | MUTEX_EOF_SEL(MUTEX_SOF_SEL_DSI0);

	mtk_disp_comp_write(dev, DISP_MUTEX_SOF(MUTEX_DSI_ID), sof);

	mtk_disp_comp_write(dev, DISP_MUTEX_EN(MUTEX_DSI_ID), MUTEX_EN);

	return 0;
}

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
	mtk_disp_comp_write(dev, DISP_MUTEX_EN(MUTEX_HDMI_ID), MUTEX_EN);
}

static const struct udevice_id mtk_disp_mutex_ids[] = {
	{ .compatible = "mediatek,mt8188-disp-mutex" },
	{ .compatible = "mediatek,mt8189-disp-mutex",
	  .data = (ulong)&mt8189_mutex_data },
	{ .compatible = "mediatek,mt8365-disp-mutex",
	  .data = (ulong)&mt8365_mutex_data },
	{ .compatible = "mediatek,mt8366-disp-mutex",
	  .data = (ulong)&mt8366_mutex_data },
	{}
};

U_BOOT_DRIVER(mtk_disp_mutex) = {
	.name	   = "mtk_disp_mutex",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_disp_mutex_ids,
	.probe	   = mtk_disp_comp_probe,
	.priv_auto = sizeof(struct mtk_disp_comp_priv),
};
