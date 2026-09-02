// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek Memory Controller module
 *
 * Copyright (c) 2022 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#include <asm/io.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>
#include <linux/arm-smccc.h>
#include <linux/string.h>

#include "mtk_smi.h"

#define SMI_LARB_NONSEC_CON(id)	 (0x380 + ((id) * 4))
#define MTK_M4U_ID(larb, port)	 (((larb) << 5) | (port))

/*
 * mt8366-generation larbs gate the real mmu_en state behind ATF: per the real
 * Linux driver, "the mmu_en bits of LARB_NONSEC_CON have no effect" unless this
 * SMC call has told secure world which ports (if any) on this larb use real
 * IOMMU translation. We never set up page tables, so we always ask for none
 * (mmu bitmask 0), matching our physical-address bypass design.
 */
#define MTK_SIP_KERNEL_IOMMU_CONTROL	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
							   ARM_SMCCC_SMC_64, \
							   ARM_SMCCC_OWNER_SIP, 0x514)
#define IOMMU_ATF_CMD_CONFIG_SMI_LARB	0

static int mtk_smi_larb_bypass_iommu(u32 larbid)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_KERNEL_IOMMU_CONTROL, IOMMU_ATF_CMD_CONFIG_SMI_LARB,
		      larbid, 0, 0, 0, 0, 0, &res);

	return res.a0 ? -EINVAL : 0;
}

struct mtk_smi_common_data {
	bool has_gals;
};

static const struct mtk_smi_common_data mtk_smi_common_mt8365_data = {
	.has_gals = true,
};

static const struct mtk_smi_common_data mtk_smi_common_mt8366_data = {
	.has_gals = false,
};

struct mtk_smi_common_priv {
	struct udevice *dev;
	struct clk *common_clk;
	struct clk *comm0_clk;
	struct clk *comm1_clk;
};

static int mtk_smi_common_enable(struct udevice *dev)
{
	struct mtk_smi_common_priv *smi = dev_get_priv(dev);
	int ret;

	ret = clk_enable(smi->common_clk);
	if (ret) {
		dev_err(smi->dev, "failed to enable common clk: %s\n", errno_str(ret));
		goto common_clk_failed;
	}

	ret = clk_enable(smi->comm0_clk);
	if (ret) {
		dev_err(smi->dev, "failed to enable comm0 clk: %s\n", errno_str(ret));
		goto comm0_clk_failed;
	}

	ret = clk_enable(smi->comm1_clk);
	if (ret) {
		dev_err(smi->dev, "failed to enable comm1 clk: %s\n", errno_str(ret));
		goto comm1_clk_failed;
	}

	return 0;

comm1_clk_failed:
	clk_disable(smi->comm1_clk);
comm0_clk_failed:
	clk_disable(smi->comm0_clk);
common_clk_failed:
	clk_disable(smi->common_clk);

	return ret;
}

static int mtk_smi_common_disable(struct udevice *dev)
{
	struct mtk_smi_common_priv *smi = dev_get_priv(dev);
	int ret = 0;

	ret += clk_disable(smi->common_clk);
	if (ret)
		dev_err(smi->dev, "failed to disable common clk: %s\n", errno_str(ret));

	ret += clk_disable(smi->comm0_clk);
	if (ret)
		dev_err(smi->dev, "failed to disable comm0 clk: %s\n", errno_str(ret));

	ret += clk_disable(smi->comm1_clk);
	if (ret)
		dev_err(smi->dev, "failed to disable comm1 clk: %s\n", errno_str(ret));

	return ret;
}

static int mtk_smi_common_probe(struct udevice *dev)
{
	struct mtk_smi_common_priv *smi = dev_get_priv(dev);
	const struct mtk_smi_common_data *data =
		(const struct mtk_smi_common_data *)dev_get_driver_data(dev);

	smi->dev = dev;

	smi->common_clk = devm_clk_get(dev, "apb");
	if (IS_ERR(smi->common_clk))
		return PTR_ERR(smi->common_clk);

	if (data->has_gals) {
		smi->comm0_clk = devm_clk_get(dev, "gals0");
		if (IS_ERR(smi->comm0_clk))
			return PTR_ERR(smi->comm0_clk);

		smi->comm1_clk = devm_clk_get(dev, "gals1");
		if (IS_ERR(smi->comm1_clk))
			return PTR_ERR(smi->comm1_clk);
	}

	return 0;
}

static const struct udevice_id mtk_smi_common_ids[] = {
	{ .compatible = "mediatek,mt8365-smi-common",
	  .data = (ulong)&mtk_smi_common_mt8365_data },
	{ .compatible = "mediatek,mt8366-smi-common",
	  .data = (ulong)&mtk_smi_common_mt8366_data },
	{}
};

U_BOOT_DRIVER(mtk_smi_common) = {
	.name	   = "mtk_smi_common",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_smi_common_ids,
	.probe	   = mtk_smi_common_probe,
	.priv_auto = sizeof(struct mtk_smi_common_priv),
};

struct mtk_smi_larb_data {
	bool needs_iommu_bypass;
};

static const struct mtk_smi_larb_data mtk_smi_larb_mt8365_data = {
	.needs_iommu_bypass = false,
};

static const struct mtk_smi_larb_data mtk_smi_larb_mt8366_data = {
	.needs_iommu_bypass = true,
};

struct mtk_smi_larb_priv {
	void __iomem *base;
	struct udevice *dev;
	struct udevice *smi;
	struct clk clk;
	const struct mtk_smi_larb_data *data;
	u32 larbid;
};

int mtk_smi_larb_enable(struct udevice *dev)
{
	struct mtk_smi_larb_priv *larb = dev_get_priv(dev);
	int ret;

	ret = mtk_smi_common_enable(larb->smi);
	if (ret) {
		dev_err(larb->dev, "failed to enable smi common: %s\n", errno_str(ret));
		return ret;
	}

	ret = clk_enable(&larb->clk);
	if (ret) {
		dev_err(larb->dev, "failed to enable larb clk: %s\n", errno_str(ret));
		mtk_smi_common_disable(larb->smi);
	}

	return ret;
}

int mtk_smi_larb_disable(struct udevice *dev)
{
	struct mtk_smi_larb_priv *larb = dev_get_priv(dev);
	int ret = 0;

	ret += clk_disable(&larb->clk);
	if (ret)
		dev_err(larb->dev, "failed to disable larb clk: %s\n", errno_str(ret));

	ret += mtk_smi_common_disable(larb->smi);
	if (ret)
		dev_err(larb->dev, "failed to disable smi common: %s\n", errno_str(ret));

	return ret;
}

void mtk_smi_enable_pa(struct udevice *dev, enum m4u_port_disp mu4_port)
{
	struct mtk_smi_larb_priv *larb = dev_get_priv(dev);
	u32 offset = SMI_LARB_NONSEC_CON(MTK_M4U_ID(0, mu4_port));

	/* Agent transaction to Physical Address */
	writel(0, larb->base + offset);
}

void mtk_smi_disable_pa(struct udevice *dev, enum m4u_port_disp mu4_port)
{
	struct mtk_smi_larb_priv *larb = dev_get_priv(dev);

	/* Agent transaction to Virtual Address (default) */
	writel(1, larb->base + SMI_LARB_NONSEC_CON(MTK_M4U_ID(0, mu4_port)));
}

static int mtk_smi_larb_probe(struct udevice *dev)
{
	struct mtk_smi_larb_priv *larb = dev_get_priv(dev);
	int ret;

	larb->dev = dev;
	larb->data = (const struct mtk_smi_larb_data *)dev_get_driver_data(dev);

	larb->base = dev_remap_addr(dev);
	if (IS_ERR(larb->base))
		return PTR_ERR(larb->base);

	ret = clk_get_by_index(dev, 0, &larb->clk);
	if (ret)
		return ret;

	ret = uclass_get_device_by_phandle(UCLASS_MISC, dev, "mediatek,smi", &larb->smi);
	if (ret) {
		dev_err(dev, "cannot get smi device: %s\n", errno_str(ret));
		return ret;
	}

	larb->larbid = (dev->name && strstr(dev->name, "larb1")) ? 1 : 0;

	if (larb->data->needs_iommu_bypass) {
		ret = mtk_smi_larb_bypass_iommu(larb->larbid);
		if (ret)
			dev_dbg(dev, "iommu bypass smc failed: %d\n", ret);
	}

	return 0;
}

static const struct udevice_id mtk_smi_larb_ids[] = {
	{ .compatible = "mediatek,mt8365-smi-larb",
	  .data = (ulong)&mtk_smi_larb_mt8365_data },
	{ .compatible = "mediatek,mt8366-smi-larb",
	  .data = (ulong)&mtk_smi_larb_mt8366_data },
	{}
};

U_BOOT_DRIVER(mtk_smi_larb_mt8365) = {
	.name	   = "mtk_smi_larb_mt8365",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_smi_larb_ids,
	.probe	   = mtk_smi_larb_probe,
	.priv_auto = sizeof(struct mtk_smi_larb_priv),
};
