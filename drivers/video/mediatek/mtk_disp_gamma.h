/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#ifndef _MTK_DISP_GAMMA_H
#define _MTK_DISP_GAMMA_H

void mtk_gamma_config(struct udevice *dev, struct display_timing timing);
void mtk_gamma_start(struct udevice *dev);
int mtk_gamma_enable(struct udevice *dev);
int mtk_gamma_disable(struct udevice *dev);

#endif
