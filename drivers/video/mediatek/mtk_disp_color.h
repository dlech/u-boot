/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#ifndef _MTK_DISP_COLOR_H
#define _MTK_DISP_COLOR_H

void mtk_color_config(struct udevice *dev, struct display_timing timing);
void mtk_color_start(struct udevice *dev);
int mtk_color_enable(struct udevice *dev);
int mtk_color_disable(struct udevice *dev);

#endif
