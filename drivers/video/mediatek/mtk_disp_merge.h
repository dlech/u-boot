/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mediatek Video Disp Merge Support
 *
 * Copyright (c) 2026 BayLibre, SAS.
 * Author: Julien Stephan <jstephan@baylibre.com>
 */

#ifndef _MTK_DISP_MERGE_H
#define _MTK_DISP_MERGE_H

struct udevice;

void mtk_disp_merge_config(struct udevice *dev,
			   u16 width1, u16 height1,
			   u16 width2, u16 height2,
			   u16 output_width, u16 output_height);

#endif
