/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mediatek Video MDP RDMA Support
 *
 * Copyright (c) 2026 BayLibre, SAS.
 * Author: Julien Stephan <jstephan@baylibre.com>
 */

#ifndef _MTK_MDP_RDMA_H
#define _MTK_MDP_RDMA_H

struct udevice;
struct video_uc_plat;
struct display_timing;

int mtk_mdp_rdma_config(struct udevice *dev, struct video_uc_plat *plat,
			const struct display_timing *timing,
			unsigned int pitch, bool apply_offset);

#endif
