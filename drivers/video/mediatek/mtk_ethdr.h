/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mediatek Video ETHDR support
 *
 * Copyright (c) 2026 BayLibre, SAS.
 * Author: Julien Stephan <jstephan@baylibre.com>
 */

#ifndef _MTK_ETHDR_H
#define _MTK_ETHDR_H

struct udevice;

void mtk_ethdr_config(struct udevice *dev, u16 width, u16 height);

#endif
