/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#ifndef _MTK_DISP_OVL_H
#define _MTK_DISP_OVL_H

#include "mtk_smi.h"

void mtk_ovl_enable_vblank(struct udevice *dev);
void mtk_ovl_config(struct udevice *dev, struct display_timing timing);
void mtk_ovl_start(struct udevice *dev);
void mtk_ovl_layer(struct udevice *dev, struct video_uc_plat *plat, struct display_timing timing);
int mtk_ovl_enable(struct udevice *dev);
int mtk_ovl_disable(struct udevice *dev);
int mtk_ovl_smi_enable(struct udevice *dev);
int mtk_ovl_smi_disable(struct udevice *dev);

#endif
