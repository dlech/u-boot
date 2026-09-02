/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#ifndef _MTK_DISP_DITHER_H
#define _MTK_DISP_DITHER_H

void mtk_dither_config(struct udevice *dev, struct display_timing timing);
void mtk_dither_start(struct udevice *dev);
int mtk_dither_enable(struct udevice *dev);
int mtk_dither_disable(struct udevice *dev);

#endif
