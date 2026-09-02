// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek SMI local arbiter (larb) driver
 *
 * Copyright (c) 2026 BayLibre, SAS.
 * Author: Julien Stephan <jstephan@baylibre.com>
 */

#include <dm.h>

#include "mtk_disp_comp.h"

/*
 * The SMI larbs gate a subsystem's access to DRAM. They have no dedicated
 * driver upstream (the SMI/IOMMU framework handles them), so bind a minimal
 * driver that just ungates their clocks, reusing the display component
 * helpers for the register window and clock bulk.
 */
static const struct udevice_id mtk_smi_larb_ids[] = {
	{ .compatible = "mediatek,mt8188-smi-larb" },
	{}
};

U_BOOT_DRIVER(mtk_smi_larb) = {
	.name	   = "mtk_smi_larb",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_smi_larb_ids,
	.probe	   = mtk_disp_comp_probe,
	.priv_auto = sizeof(struct mtk_disp_comp_priv),
};
