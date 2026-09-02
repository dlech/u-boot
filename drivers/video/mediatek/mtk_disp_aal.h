/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#ifndef _MTK_DISP_AAL_H
#define _MTK_DISP_AAL_H

void mtk_aal_config(struct udevice *dev, struct display_timing timing);
void mtk_aal_start(struct udevice *dev);
int mtk_aal_enable(struct udevice *dev);
int mtk_aal_disable(struct udevice *dev);

#endif
