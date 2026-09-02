/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#ifndef _MTK_SMI_H
#define _MTK_SMI_H

/*
 * Local port index within a given larb's own SMI_LARB_NONSEC_CON array.
 * larb0 and larb1 each have their own OVLx/RDMAx/FAKE_ENGx at these same
 * local indices, so mtk_smi_enable_pa()/mtk_smi_disable_pa() only need the
 * local index -- the larb is already selected by which larb device is
 * passed in.
 */
enum m4u_port_disp {
	M4U_PORT_DISP_OVL0 = 0,
	M4U_PORT_DISP_RDMA0,
	M4U_PORT_DISP_WDMA0,
	M4U_PORT_DISP_FAKE0,
	M4U_PORT_DISP_RDMA1 = M4U_PORT_DISP_RDMA0,
};

int mtk_smi_larb_enable(struct udevice *dev);
int mtk_smi_larb_disable(struct udevice *dev);
void mtk_smi_enable_pa(struct udevice *dev, enum m4u_port_disp mu4_port);
void mtk_smi_disable_pa(struct udevice *dev, enum m4u_port_disp mu4_port);

#endif
