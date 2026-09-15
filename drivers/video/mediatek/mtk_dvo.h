/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MT8189 Digital Video Output (DVO) support
 *
 * Copyright (c) 2026 BayLibre, SAS.
 * Author: David Lechner <dlechner@baylibre.com>
 */

#ifndef _MTK_DVO_H
#define _MTK_DVO_H

struct display_timing;
struct udevice;

void mtk_dvo_hw_enable(struct udevice *dev);
void mtk_dvo_hw_disable(struct udevice *dev);
int mtk_dvo_config(struct udevice *dev, const struct display_timing *timing);

#endif
