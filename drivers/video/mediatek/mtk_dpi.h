/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mediatek MT8188 DPI Support
 *
 * Copyright (c) 2026 BayLibre, SAS.
 * Author: Julien Stephan <jstephan@baylibre.com>
 */

#ifndef _MTK_DPI_H
#define _MTK_DPI_H

struct display_timing;
struct udevice;

void mtk_dpi_hw_enable(struct udevice *dev);
void mtk_dpi_hw_disable(struct udevice *dev);
void mtk_dpi_config(struct udevice *dev, const struct display_timing *timing,
		    bool rgb);

#endif
