// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek Video DSI support
 *
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#include <asm/io.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <linux/math64.h>

#define MIPITX_LANE_CON		0x00c
#define RG_DSI_BG_LPF_EN	       BIT(6)
#define RG_DSI_BG_CORE_EN	       BIT(7)
#define RG_DSI_PAD_TIEL_SEL	       BIT(8)

#define MIPITX_VOLTAGE_SEL	0x010
#define RG_DSI_HSTX_LDO_REF_SEL	       (0xf << 6)

#define MIPITX_PLL_PWR		0x028
#define AD_DSI_PLL_SDM_PWR_ON	       BIT(0)
#define AD_DSI_PLL_SDM_ISO_EN	       BIT(1)

#define MIPITX_PLL_CON0		0x02c

#define MIPITX_PLL_CON1		0x030
#define RG_DSI_PLL_EN		       BIT(4)
#define RG_DSI_PLL_POSDIV	       (0x7 << 8)

#define MIPITX_PLL_CON4		0x03c
#define RG_DSI_PLL_IBIAS	       (3 << 10)

#define MIPITX_D2P_RTCODE	0x100
#define MIPITX_D2_SW_CTL_EN	0x144
#define MIPITX_D0_SW_CTL_EN	0x244
#define MIPITX_CK_CKMODE_EN	0x328
#define DSI_CK_CKMODE_EN	       BIT(0)
#define MIPITX_CK_SW_CTL_EN	0x344
#define MIPITX_D1_SW_CTL_EN	0x444
#define MIPITX_D3_SW_CTL_EN	0x544
#define DSI_SW_CTL_EN		       BIT(0)

struct mtk_mipi_tx_priv {
	void __iomem *base;
	struct udevice *dev;
	struct clk *pll_clk;
};

static void mtk_mipi_tx_clear_bits(struct mtk_mipi_tx_priv *mipi_tx, u32 offset,
				   u32 bits)
{
	u32 tmp = readl(mipi_tx->base + offset);

	writel(tmp & ~bits, mipi_tx->base + offset);
}

static void mtk_mipi_tx_set_bits(struct mtk_mipi_tx_priv *mipi_tx, u32 offset,
				 u32 bits)
{
	u32 tmp = readl(mipi_tx->base + offset);

	writel(tmp | bits, mipi_tx->base + offset);
}

static void mtk_mipi_tx_update_bits(struct mtk_mipi_tx_priv *mipi_tx, u32 offset,
				    u32 mask, u32 data)
{
	u32 tmp = readl(mipi_tx->base + offset);

	writel((tmp & ~mask) | (data & mask), mipi_tx->base + offset);
}

/*
 * There is no "calibration-data" nvmem cell for this board, matching real
 * Linux's own fallback path: force a fixed mid-scale RT trim code on every
 * lane's impedance-calibration bits instead of loading an efuse value.
 */
static void mtk_mipi_tx_config_calibration_data(struct mtk_mipi_tx_priv *mipi_tx)
{
	u32 rt_code = 0x210;
	int i, j;

	for (i = 0; i < 5; i++)
		for (j = 0; j < 10; j++)
			mtk_mipi_tx_update_bits(mipi_tx, MIPITX_D2P_RTCODE * (i + 1) + j * 4,
						1, rt_code >> j & 1);
}

static void mtk_mipi_tx_power_on_signal(struct mtk_mipi_tx_priv *mipi_tx)
{
	/* BG_LPF_EN / BG_CORE_EN */
	writel(RG_DSI_PAD_TIEL_SEL | RG_DSI_BG_CORE_EN, mipi_tx->base + MIPITX_LANE_CON);
	udelay(100);
	writel(RG_DSI_BG_CORE_EN | RG_DSI_BG_LPF_EN, mipi_tx->base + MIPITX_LANE_CON);

	/* Switch OFF each Lane */
	mtk_mipi_tx_clear_bits(mipi_tx, MIPITX_D0_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_mipi_tx_clear_bits(mipi_tx, MIPITX_D1_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_mipi_tx_clear_bits(mipi_tx, MIPITX_D2_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_mipi_tx_clear_bits(mipi_tx, MIPITX_D3_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_mipi_tx_clear_bits(mipi_tx, MIPITX_CK_SW_CTL_EN, DSI_SW_CTL_EN);

	/*
	 * No "drive-strength-microamp" DT property on this board, matching
	 * real Linux's own default of 4600uA -> field value (4600-3000)/200.
	 */
	mtk_mipi_tx_update_bits(mipi_tx, MIPITX_VOLTAGE_SEL, RG_DSI_HSTX_LDO_REF_SEL, 8 << 6);

	mtk_mipi_tx_config_calibration_data(mipi_tx);

	mtk_mipi_tx_set_bits(mipi_tx, MIPITX_CK_CKMODE_EN, DSI_CK_CKMODE_EN);
}

static void mtk_mipi_tx_power_off_signal(struct mtk_mipi_tx_priv *mipi_tx)
{
	/* Switch ON each Lane */
	mtk_mipi_tx_set_bits(mipi_tx, MIPITX_D0_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_mipi_tx_set_bits(mipi_tx, MIPITX_D1_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_mipi_tx_set_bits(mipi_tx, MIPITX_D2_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_mipi_tx_set_bits(mipi_tx, MIPITX_D3_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_mipi_tx_set_bits(mipi_tx, MIPITX_CK_SW_CTL_EN, DSI_SW_CTL_EN);

	writel(RG_DSI_PAD_TIEL_SEL | RG_DSI_BG_CORE_EN, mipi_tx->base + MIPITX_LANE_CON);
	writel(RG_DSI_PAD_TIEL_SEL, mipi_tx->base + MIPITX_LANE_CON);
}

static int mtk_mipi_tx_pll_enable(struct mtk_mipi_tx_priv *mipi_tx, u64 data_rate)
{
	u32 txdiv, txdiv0;
	u64 pcw;

	if (data_rate >= 2000000000) {
		txdiv = 1;
		txdiv0 = 0;
	} else if (data_rate >= 1000000000) {
		txdiv = 2;
		txdiv0 = 1;
	} else if (data_rate >= 500000000) {
		txdiv = 4;
		txdiv0 = 2;
	} else if (data_rate > 250000000) {
		txdiv = 8;
		txdiv0 = 3;
	} else if (data_rate >= 125000000) {
		txdiv = 16;
		txdiv0 = 4;
	} else {
		return -EINVAL;
	}

	mtk_mipi_tx_clear_bits(mipi_tx, MIPITX_PLL_CON4, RG_DSI_PLL_IBIAS);

	mtk_mipi_tx_set_bits(mipi_tx, MIPITX_PLL_PWR, AD_DSI_PLL_SDM_PWR_ON);
	mtk_mipi_tx_clear_bits(mipi_tx, MIPITX_PLL_CON1, RG_DSI_PLL_EN);
	udelay(1);
	mtk_mipi_tx_clear_bits(mipi_tx, MIPITX_PLL_PWR, AD_DSI_PLL_SDM_ISO_EN);
	pcw = div_u64(((u64)data_rate * txdiv) << 24, 26000000);
	writel(pcw, mipi_tx->base + MIPITX_PLL_CON0);
	mtk_mipi_tx_update_bits(mipi_tx, MIPITX_PLL_CON1, RG_DSI_PLL_POSDIV, txdiv0 << 8);
	mtk_mipi_tx_set_bits(mipi_tx, MIPITX_PLL_CON1, RG_DSI_PLL_EN);

	return 0;
}

static void mtk_mipi_tx_pll_disable(struct mtk_mipi_tx_priv *mipi_tx)
{
	mtk_mipi_tx_clear_bits(mipi_tx, MIPITX_PLL_CON1, RG_DSI_PLL_EN);

	mtk_mipi_tx_set_bits(mipi_tx, MIPITX_PLL_PWR, AD_DSI_PLL_SDM_ISO_EN);
	mtk_mipi_tx_clear_bits(mipi_tx, MIPITX_PLL_PWR, AD_DSI_PLL_SDM_PWR_ON);
}

void mtk_mipi_tx_power_on(struct udevice *dev, u32 data_rate)
{
	struct mtk_mipi_tx_priv *mipi_tx = dev_get_priv(dev);

	mtk_mipi_tx_pll_enable(mipi_tx, data_rate);

	mtk_mipi_tx_power_on_signal(mipi_tx);
}

void mtk_mipi_tx_power_off(struct udevice *dev)
{
	struct mtk_mipi_tx_priv *mipi_tx = dev_get_priv(dev);

	mtk_mipi_tx_power_off_signal(mipi_tx);

	mtk_mipi_tx_pll_disable(mipi_tx);
}

static int mtk_mipi_tx_probe(struct udevice *dev)
{
	struct mtk_mipi_tx_priv *mipi_tx = dev_get_priv(dev);

	mipi_tx->dev = dev;

	mipi_tx->base = dev_remap_addr(dev);
	if (!mipi_tx->base)
		return -EINVAL;

	//printf("--------> %s %i\n", __func__, __LINE__);
	//mipi_tx->pll_clk = devm_clk_get(dev, "pll");
	//printf("--------> %s %i\n", __func__, __LINE__);
	//if (IS_ERR(mipi_tx->pll_clk))
	//	return PTR_ERR(mipi_tx->pll_clk);

	return 0;
}

static const struct udevice_id mtk_mipi_tx_ids[] = {
	{ .compatible = "mediatek,mt8183-mipi-tx" },
	{}
};

U_BOOT_DRIVER(mtk_mipi_tx) = {
	.name	   = "mtk_mipi_tx",
	.id	   = UCLASS_MISC,
	.of_match  = mtk_mipi_tx_ids,
	.probe	   = mtk_mipi_tx_probe,
	.priv_auto = sizeof(struct mtk_mipi_tx_priv),
};
