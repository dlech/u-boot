/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#ifndef _MTK_MIPI_TX_H
#define _MTK_MIPI_TX_H

void mtk_mipi_tx_power_on(struct udevice *dev, u32 data_rate);
void mtk_mipi_tx_power_off(struct udevice *dev);

#endif
