/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#ifndef _MTK_DISP_CCORR_H
#define _MTK_DISP_CCORR_H

void mtk_ccorr_config(struct udevice *dev, struct display_timing timing);
void mtk_ccorr_start(struct udevice *dev);
int mtk_ccorr_enable(struct udevice *dev);
int mtk_ccorr_disable(struct udevice *dev);

#endif
