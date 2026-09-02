/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek display pipeline component helpers
 *
 * Copyright (c) 2026 BayLibre, SAS.
 * Author: Julien Stephan <jstephan@baylibre.com>
 */

#ifndef _MTK_DISP_COMP_H
#define _MTK_DISP_COMP_H

#include <clk.h>

struct udevice;

/*
 * Common state shared by all the simple display pipeline components:
 * a register window and a bulk of clocks. @larb is an optional SMI larb
 * enabled and disabled together with the component (used by the RDMAs).
 */
struct mtk_disp_comp_priv {
	void __iomem *base;
	struct clk_bulk clk_bulk;
	struct udevice *larb;
};

int mtk_disp_comp_probe(struct udevice *dev);
int mtk_disp_comp_enable(struct udevice *dev);
int mtk_disp_comp_disable(struct udevice *dev);
void mtk_disp_comp_write(struct udevice *dev, u32 offset, u32 val);

#endif /* _MTK_DISP_COMP_H */
