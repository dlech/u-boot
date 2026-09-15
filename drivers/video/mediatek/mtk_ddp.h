/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#ifndef _MTK_DDP_H
#define _MTK_DDP_H

void mtk_ddp_ovl_to_dsi(void __iomem *mmsys_base, bool has_color_pipeline);
void mtk_ddp_ovl_to_dvo(void __iomem *mmsys_base);
void mtk_ddp_rdma_to_dpi(void __iomem *mmsys_base);

#endif
