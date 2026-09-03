// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek Video DSI support
 *
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#include <asm/io.h>
#include <asm/system.h>
#include <backlight.h>
#include <clk.h>
#include <div64.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dm/lists.h>
#include <dsi_host.h>
#include <errno.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <panel.h>
#include <regmap.h>
#include <syscon.h>
#include <video.h>
#include <video_bridge.h>

#include "mtk_ddp.h"
#include "mtk_disp_aal.h"
#include "mtk_disp_ccorr.h"
#include "mtk_disp_color.h"
#include "mtk_disp_dither.h"
#include "mtk_disp_gamma.h"
#include "mtk_disp_mutex.h"
#include "mtk_disp_ovl.h"
#include "mtk_disp_rdma.h"
#include "mtk_mipi_tx.h"

#define MTK_DSI_MAX_WIDTH  3840
#define MTK_DSI_MAX_HEIGHT 2160

#define DSI_START		0x00

#define DSI_INTEN		0x08
#define LPRX_RD_RDY_INT_FLAG	      BIT(0)
#define CMD_DONE_INT_FLAG	      BIT(1)
#define VM_DONE_INT_FLAG	      BIT(3)

#define DSI_CON_CTRL		0x10
#define DSI_RESET		      BIT(0)
#define DSI_EN			      BIT(1)
#define DPHY_RESET		      BIT(2)

#define DSI_MODE_CTRL		0x14
#define CMD_MODE		      0
#define SYNC_PULSE_MODE		      1
#define SYNC_EVENT_MODE		      2
#define BURST_MODE		      3

#define DSI_TXRX_CTRL		0x18

#define DSI_PSCTRL		0x1c
#define DSI_PS_WC		      0x3fff
#define PACKED_PS_16BIT_RGB565	      (0 << 16)
#define LOOSELY_PS_18BIT_RGB666	      (1 << 16)
#define PACKED_PS_18BIT_RGB666	      (2 << 16)
#define PACKED_PS_24BIT_RGB888	      (3 << 16)

#define DSI_VSA_NL		0x20
#define DSI_VBP_NL		0x24
#define DSI_VFP_NL		0x28
#define DSI_VACT_NL		0x2C
#define DSI_SIZE_CON		0x38
#define DSI_HSA_WC		0x50
#define DSI_HBP_WC		0x54
#define DSI_HFP_WC		0x58
#define DSI_HSTX_CKL_WC		0x64

#define VM_CMD_EN		       BIT(0)
#define TS_VFP_EN		       BIT(5)

#define DSI_PHY_LCCON		0x104
#define LC_HS_TX_EN		       BIT(0)
#define LC_ULPM_EN		       BIT(1)
#define LC_WAKEUP_EN		       BIT(2)

#define DSI_PHY_LD0CON		0x108
#define LD0_ULPM_EN		       BIT(1)
#define LD0_WAKEUP_EN		       BIT(2)

#define DSI_PHY_TIMECON0	0x110
#define LPX			       GENMASK(7, 0)
#define HS_PREP			       GENMASK(15, 8)
#define HS_ZERO			       GENMASK(23, 16)
#define HS_TRAIL		       GENMASK(31, 24)

#define DSI_PHY_TIMECON1	0x114
#define TA_GO			       GENMASK(7, 0)
#define TA_SURE			       GENMASK(15, 8)
#define TA_GET			       GENMASK(23, 16)
#define DA_HS_EXIT		       GENMASK(31, 24)

#define DSI_PHY_TIMECON2	0x118
#define DA_HS_SYNC		       GENMASK(15, 8)
#define CLK_ZERO		       GENMASK(23, 16)
#define CLK_TRAIL		       GENMASK(31, 24)

#define DSI_PHY_TIMECON3	0x11c
#define CLK_HS_PREP		       GENMASK(7, 0)
#define CLK_HS_POST		       GENMASK(15, 8)
#define CLK_HS_EXIT		       GENMASK(23, 16)

/* mt8183 register offset; mt8366 and mt8189 share the mt8188 layout instead. */
#define DSI_SHADOW_DEBUG_MT8183	0x190
#define DSI_SHADOW_DEBUG_MT8188	0xc00
#define FORCE_COMMIT		       BIT(0)
#define BYPASS_SHADOW		       BIT(1)

struct mtk_dsi_priv {
	void __iomem *base;
	struct udevice *dev;
	const struct mtk_dsi_data *data;
	struct udevice *ovl;
	struct udevice *rdma;
	struct udevice *color;
	struct udevice *ccorr;
	struct udevice *aal;
	struct udevice *gamma;
	struct udevice *dither;
	struct udevice *mutex;
	void __iomem *mmsys_base;
	struct udevice *phy;
	struct udevice *panel;
	struct udevice *host;
	struct mipi_dsi_device mipi;
	struct clk *engine_clk;
	struct clk *digital_clk;
	struct clk *hs_clk;
	u64 data_rate;
};

struct mtk_dsi_data {
	bool has_color_pipeline;
	bool has_shadow_ctl;
	u32 shadow_dbg_off;
	u32 vm_cmdq_off;
	u32 host_cmdq_off;
};

static const struct mtk_dsi_data mt8183_dsi_data = {
	.has_color_pipeline = true,
	.has_shadow_ctl = true,
	.shadow_dbg_off = DSI_SHADOW_DEBUG_MT8183,
	.vm_cmdq_off = 0x130,
	.host_cmdq_off = 0x200,
};

static const struct mtk_dsi_data mt8188_dsi_data = {
	.has_color_pipeline = false,
	.has_shadow_ctl = false,
	.shadow_dbg_off = DSI_SHADOW_DEBUG_MT8188,
	.vm_cmdq_off = 0x200,
	.host_cmdq_off = 0xd00,
};

static void mtk_dsi_mask(struct mtk_dsi_priv *dsi, u32 offset, u32 val, u32 mask)
{
	void __iomem *addr = dsi->base + offset;
	u32 tmp = readl(addr) & ~mask;

	tmp |= (val & mask);
	writel(tmp, addr);
}

static void mtk_dsi_shadow_debug(struct mtk_dsi_priv *dsi)
{
	writel(FORCE_COMMIT | BYPASS_SHADOW, dsi->base + dsi->data->shadow_dbg_off);
}

static void mtk_dsi_enable(struct mtk_dsi_priv *dsi)
{
	mtk_dsi_mask(dsi, DSI_CON_CTRL, DSI_EN, DSI_EN);
}

static void mtk_dsi_reset_engine(struct mtk_dsi_priv *dsi)
{
	mtk_dsi_mask(dsi, DSI_CON_CTRL, DSI_RESET, DSI_RESET);
	mtk_dsi_mask(dsi, DSI_CON_CTRL, 0, DSI_RESET);
}

static void mtk_dsi_reset_dphy(struct mtk_dsi_priv *dsi)
{
	mtk_dsi_mask(dsi, DSI_CON_CTRL, DPHY_RESET, DPHY_RESET);
	mtk_dsi_mask(dsi, DSI_CON_CTRL, 0, DPHY_RESET);
}

static void mtk_dsi_clk_ulp_mode_leave(struct mtk_dsi_priv *dsi)
{
	mtk_dsi_mask(dsi, DSI_PHY_LCCON, 0, LC_ULPM_EN);
	mtk_dsi_mask(dsi, DSI_PHY_LCCON, LC_WAKEUP_EN, LC_WAKEUP_EN);
	mtk_dsi_mask(dsi, DSI_PHY_LCCON, 0, LC_WAKEUP_EN);
}

static void mtk_dsi_lane0_ulp_mode_leave(struct mtk_dsi_priv *dsi)
{
	mtk_dsi_mask(dsi, DSI_PHY_LD0CON, 0, LD0_ULPM_EN);
	mtk_dsi_mask(dsi, DSI_PHY_LD0CON, LD0_WAKEUP_EN, LD0_WAKEUP_EN);
	mtk_dsi_mask(dsi, DSI_PHY_LD0CON, 0, LD0_WAKEUP_EN);
}

static bool mtk_dsi_clk_hs_state(struct mtk_dsi_priv *dsi)
{
	u32 tmp = readl(dsi->base + DSI_PHY_LCCON);

	return ((tmp & LC_HS_TX_EN) == 1) ? true : false;
}

static void mtk_dsi_clk_hs_mode(struct mtk_dsi_priv *dsi, bool enter)
{
	if (enter && !mtk_dsi_clk_hs_state(dsi))
		mtk_dsi_mask(dsi, DSI_PHY_LCCON, LC_HS_TX_EN, LC_HS_TX_EN);
	else if (!enter && mtk_dsi_clk_hs_state(dsi))
		mtk_dsi_mask(dsi, DSI_PHY_LCCON, 0, LC_HS_TX_EN);
}

static void mtk_dsi_phy_timconfig(struct mtk_dsi_priv *dsi)
{
	u32 data_rate_mhz = DIV_ROUND_UP_ULL(dsi->data_rate, 1000000);
	u32 lpx, da_hs_prepare, da_hs_zero, da_hs_trail;
	u32 ta_go, ta_sure, ta_get, da_hs_exit;
	u32 clk_hs_prepare, clk_hs_post, clk_hs_trail, clk_hs_zero, clk_hs_exit;
	u32 timcon0, timcon1, timcon2, timcon3;

	lpx = (60 * data_rate_mhz / (8 * 1000)) + 1;
	da_hs_prepare = (80 * data_rate_mhz + 4 * 1000) / 8000;
	da_hs_zero = (170 * data_rate_mhz + 10 * 1000) / 8000 + 1 - da_hs_prepare;
	da_hs_trail = da_hs_prepare + 1;

	ta_go = 4 * lpx - 2;
	ta_sure = lpx + 2;
	ta_get = 4 * lpx;
	da_hs_exit = 2 * lpx + 1;

	clk_hs_prepare = 70 * data_rate_mhz / (8 * 1000);
	clk_hs_post = clk_hs_prepare + 8;
	clk_hs_trail = clk_hs_prepare;
	clk_hs_zero = clk_hs_trail * 4;
	clk_hs_exit = 2 * clk_hs_trail;

	timcon0 = FIELD_PREP(LPX, lpx) | FIELD_PREP(HS_PREP, da_hs_prepare) |
		  FIELD_PREP(HS_ZERO, da_hs_zero) | FIELD_PREP(HS_TRAIL, da_hs_trail);

	timcon1 = FIELD_PREP(TA_GO, ta_go) | FIELD_PREP(TA_SURE, ta_sure) |
		  FIELD_PREP(TA_GET, ta_get) | FIELD_PREP(DA_HS_EXIT, da_hs_exit);

	timcon2 = FIELD_PREP(DA_HS_SYNC, 1) | FIELD_PREP(CLK_ZERO, clk_hs_zero) |
		  FIELD_PREP(CLK_TRAIL, clk_hs_trail);

	timcon3 = FIELD_PREP(CLK_HS_PREP, clk_hs_prepare) |
		  FIELD_PREP(CLK_HS_POST, clk_hs_post) |
		  FIELD_PREP(CLK_HS_EXIT, clk_hs_exit);

	writel(timcon0, dsi->base + DSI_PHY_TIMECON0);
	writel(timcon1, dsi->base + DSI_PHY_TIMECON1);
	writel(timcon2, dsi->base + DSI_PHY_TIMECON2);
	writel(timcon3, dsi->base + DSI_PHY_TIMECON3);
}

static void mtk_dsi_set_interrupt_enable(struct mtk_dsi_priv *dsi)
{
	u32 inten = LPRX_RD_RDY_INT_FLAG | CMD_DONE_INT_FLAG | VM_DONE_INT_FLAG;

	writel(inten, dsi->base + DSI_INTEN);
}

static void mtk_dsi_set_vm_cmd(struct mtk_dsi_priv *dsi)
{
	mtk_dsi_mask(dsi, dsi->data->vm_cmdq_off, VM_CMD_EN, VM_CMD_EN);
	mtk_dsi_mask(dsi, dsi->data->vm_cmdq_off, TS_VFP_EN, TS_VFP_EN);
}

static void mtk_dsi_rxtx_control(struct mtk_dsi_priv *dsi)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dsi->panel);
	u32 reg;

	switch (plat->lanes) {
	case 1:
		reg = 1 << 2;
		break;
	case 2:
		reg = 3 << 2;
		break;
	case 3:
		reg = 7 << 2;
		break;
	case 4:
		reg = 0xf << 2;
		break;
	default:
		reg = 0xf << 2;
		break;
	}

	reg |= (plat->mode_flags & MIPI_DSI_CLOCK_NON_CONTINUOUS) << 6;
	reg |= (plat->mode_flags & MIPI_DSI_MODE_EOT_PACKET) >> 3;

	writel(reg, dsi->base + DSI_TXRX_CTRL);
}

static void mtk_dsi_lane_ready(struct mtk_dsi_priv *dsi)
{
	mtk_dsi_rxtx_control(dsi);
	udelay(100);
	mtk_dsi_reset_dphy(dsi);
	mtk_dsi_clk_ulp_mode_leave(dsi);
	mtk_dsi_lane0_ulp_mode_leave(dsi);
	mtk_dsi_clk_hs_mode(dsi, false);
	udelay(3000);
}

static void mtk_dsi_ps_control(struct mtk_dsi_priv *dsi,
			       struct display_timing timing)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dsi->panel);
	u32 bpp;
	u32 ps_wc;
	u32 reg;

	switch (plat->format) {
	case MIPI_DSI_FMT_RGB888:
		reg = PACKED_PS_24BIT_RGB888;
		bpp = 3;
		break;
	case MIPI_DSI_FMT_RGB666:
		reg = LOOSELY_PS_18BIT_RGB666;
		bpp = 3;
		break;
	case MIPI_DSI_FMT_RGB666_PACKED:
		reg = PACKED_PS_18BIT_RGB666;
		bpp = 3;
		break;
	case MIPI_DSI_FMT_RGB565:
		reg = PACKED_PS_16BIT_RGB565;
		bpp = 2;
		break;
	default:
		reg = PACKED_PS_24BIT_RGB888;
		bpp = 3;
		break;
	}

	ps_wc = timing.hactive.typ * bpp & DSI_PS_WC;

	/*
	 * The clock lane needs the same word count as the pixel stream, or it
	 * never sustains HS for the whole data burst and no video frame ever
	 * completes.
	 */
	writel(ps_wc, dsi->base + DSI_HSTX_CKL_WC);

	writel(reg | ps_wc, dsi->base + DSI_PSCTRL);
}

/*
 * Number of D-PHY HS byte-clock cycles the panel needs to switch the data
 * lanes from LP to HS and back, derived from the same lpx/da_hs_prepare/
 * da_hs_zero/da_hs_exit values mtk_dsi_phy_timconfig() programs into
 * DSI_PHY_TIMECON0/1, so it must be subtracted out of the HFP/HBP word
 * counts below or the horizontal blanking period ends up far too long.
 */
static u32 mtk_dsi_data_phy_cycles(u64 data_rate)
{
	u32 data_rate_mhz = DIV_ROUND_UP_ULL(data_rate, 1000000);
	u32 lpx = (60 * data_rate_mhz / (8 * 1000)) + 1;
	u32 da_hs_prepare = (80 * data_rate_mhz + 4 * 1000) / 8000;
	u32 da_hs_zero = (170 * data_rate_mhz + 10 * 1000) / 8000 + 1 - da_hs_prepare;
	u32 da_hs_exit = 2 * lpx + 1;

	return lpx + da_hs_prepare + da_hs_zero + da_hs_exit + 3;
}

static void mtk_dsi_config_vdo_timing(struct mtk_dsi_priv *dsi,
				      struct display_timing timing)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dsi->panel);
	u32 hsa, hbp, hfp, bpp;
	u32 delta, front_back, data_phy_cycles_byte;
	u32 data_phy_cycles = mtk_dsi_data_phy_cycles(dsi->data_rate);

	bpp = (plat->format == MIPI_DSI_FMT_RGB565) ? 2 : 3;

	writel(timing.vsync_len.typ, dsi->base + DSI_VSA_NL);
	writel(timing.vback_porch.typ, dsi->base + DSI_VBP_NL);
	writel(timing.vfront_porch.typ, dsi->base + DSI_VFP_NL);
	writel(timing.vactive.typ, dsi->base + DSI_VACT_NL);

	writel(timing.vactive.typ << 16 | timing.hactive.typ, dsi->base + DSI_SIZE_CON);

	hsa = (timing.hsync_len.typ * bpp - 10);

	if (plat->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
		hbp = timing.hback_porch.typ * bpp - 10;
	else
		hbp = (timing.hback_porch.typ + timing.hsync_len.typ) * bpp - 10;

	/*
	 * delta is the burst/sync-pulse base overhead (18/12) plus 2 for the
	 * EOT packet, which none of our panels currently disable.
	 */
	delta = (plat->mode_flags & MIPI_DSI_MODE_VIDEO_BURST) ? 18 : 12;
	delta += 2;

	/*
	 * The d-phy allowance is shared between the two porches in proportion
	 * to their word counts, so scale by the byte values rather than by the
	 * pixel counts they were derived from.
	 */
	hfp = timing.hfront_porch.typ * bpp;
	front_back = hfp + hbp;
	data_phy_cycles_byte = data_phy_cycles * plat->lanes + delta;

	if (front_back > data_phy_cycles_byte) {
		hfp -= data_phy_cycles_byte * hfp / front_back;
		hbp -= data_phy_cycles_byte * hbp / front_back;
	} else {
		dev_warn(dsi->dev, "HFP + HBP less than d-phy, FPS will under 60Hz\n");
	}

	writel(hsa, dsi->base + DSI_HSA_WC);
	writel(hbp, dsi->base + DSI_HBP_WC);
	writel(hfp, dsi->base + DSI_HFP_WC);

	mtk_dsi_ps_control(dsi, timing);
}

static void mtk_dsi_set_display_mode(struct mtk_dsi_priv *dsi,
				     struct display_timing timing)
{
	struct video_priv *priv = dev_get_uclass_priv(dsi->dev);

	priv->xsize = timing.hactive.typ;
	priv->ysize = timing.vactive.typ;
	priv->bpix = VIDEO_BPP32;

	mtk_dsi_rxtx_control(dsi);
	udelay(100);
	mtk_dsi_config_vdo_timing(dsi, timing);
}

static void mtk_dsi_set_mode(struct mtk_dsi_priv *dsi)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dsi->panel);
	u32 vid_mode = CMD_MODE;

	if (plat->mode_flags & MIPI_DSI_MODE_VIDEO) {
		if (plat->mode_flags & MIPI_DSI_MODE_VIDEO_BURST)
			vid_mode = BURST_MODE;
		else if (plat->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
			vid_mode = SYNC_PULSE_MODE;
		else
			vid_mode = SYNC_EVENT_MODE;
	}

	writel(vid_mode, dsi->base + DSI_MODE_CTRL);
}

static void mtk_dsi_start(struct mtk_dsi_priv *dsi)
{
	writel(0, dsi->base + DSI_START);
	writel(1, dsi->base + DSI_START);
}

static void mtk_dsi_output_enable(struct mtk_dsi_priv *dsi)
{
	mtk_dsi_set_mode(dsi);

	/* The panel's DCS init goes out in LP, so raise the clock lane only now. */
	mtk_dsi_clk_hs_mode(dsi, true);

	mtk_dsi_start(dsi);
}

static int mtk_dsi_power_on(struct mtk_dsi_priv *dsi, struct display_timing timing)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dsi->panel);
	int ret, bit_per_pixel;

	switch (plat->format) {
	case MIPI_DSI_FMT_RGB565:
		bit_per_pixel = 16;
		break;
	case MIPI_DSI_FMT_RGB666_PACKED:
		bit_per_pixel = 18;
		break;
	case MIPI_DSI_FMT_RGB666:
	case MIPI_DSI_FMT_RGB888:
	default:
		bit_per_pixel = 24;
		break;
	}

	dsi->data_rate = DIV_ROUND_UP_ULL(timing.pixelclock.typ * bit_per_pixel,
					  plat->lanes);

	mtk_mipi_tx_power_on(dsi->phy, dsi->data_rate);

	ret = clk_enable(dsi->engine_clk);
	if (ret) {
		dev_err(dsi->dev, "failed to enable engine clk: %d\n", ret);
		goto engine_clk_failed;
	}

	ret = clk_enable(dsi->digital_clk);
	if (ret) {
		dev_err(dsi->dev, "failed to enable digital clk: %d\n", ret);
		goto digital_clk_failed;
	}

	ret = mtk_ovl_enable(dsi->ovl);
	if (ret) {
		dev_err(dsi->dev, "failed to enable ovl: %d\n", ret);
		goto ovl_enable_failed;
	}

	ret = mtk_ovl_smi_enable(dsi->ovl);
	if (ret) {
		dev_err(dsi->dev, "failed to enable ovl smi: %d\n", ret);
		goto ovl_smi_enable_failed;
	}

	ret = mtk_rdma_enable(dsi->rdma);
	if (ret) {
		dev_err(dsi->dev, "failed to enable rdma: %d\n", ret);
		goto rdma_enable_failed;
	}

	ret = mtk_rdma_smi_enable(dsi->rdma);
	if (ret) {
		dev_err(dsi->dev, "failed to enable rdma smi: %d\n", ret);
		goto rdma_smi_enable_failed;
	}

	if (dsi->data->has_color_pipeline) {
		ret = mtk_color_enable(dsi->color);
		if (ret) {
			dev_err(dsi->dev, "failed to enable color: %d\n", ret);
			goto color_enable_failed;
		}

		ret = mtk_ccorr_enable(dsi->ccorr);
		if (ret) {
			dev_err(dsi->dev, "failed to enable ccorr: %d\n", ret);
			goto ccorr_enable_failed;
		}

		ret = mtk_aal_enable(dsi->aal);
		if (ret) {
			dev_err(dsi->dev, "failed to enable aal: %d\n", ret);
			goto aal_enable_failed;
		}

		ret = mtk_gamma_enable(dsi->gamma);
		if (ret) {
			dev_err(dsi->dev, "failed to enable gamma: %d\n", ret);
			goto gamma_enable_failed;
		}

		ret = mtk_dither_enable(dsi->dither);
		if (ret) {
			dev_err(dsi->dev, "failed to enable dither: %d\n", ret);
			goto dither_enable_failed;
		}
	}

	mtk_dsi_enable(dsi);

	if (dsi->data->has_shadow_ctl)
		mtk_dsi_shadow_debug(dsi);

	mtk_dsi_reset_engine(dsi);

	mtk_dsi_phy_timconfig(dsi);

	mtk_dsi_set_vm_cmd(dsi);

	mtk_dsi_set_interrupt_enable(dsi);

	return 0;

dither_enable_failed:
	mtk_dither_disable(dsi->dither);
gamma_enable_failed:
	mtk_gamma_disable(dsi->gamma);
aal_enable_failed:
	mtk_aal_disable(dsi->aal);
ccorr_enable_failed:
	mtk_ccorr_disable(dsi->ccorr);
color_enable_failed:
	mtk_color_disable(dsi->color);
rdma_smi_enable_failed:
	mtk_rdma_smi_disable(dsi->rdma);
rdma_enable_failed:
	mtk_rdma_disable(dsi->rdma);
ovl_smi_enable_failed:
	mtk_ovl_smi_disable(dsi->ovl);
ovl_enable_failed:
	mtk_ovl_disable(dsi->ovl);
digital_clk_failed:
	clk_disable(dsi->digital_clk);
engine_clk_failed:
	clk_disable(dsi->engine_clk);

	mtk_mipi_tx_power_off(dsi->phy);
	return ret;
}

static int mtk_dsi_host_init(struct mtk_dsi_priv *dsi)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dsi->panel);
	struct mipi_dsi_device *device = &dsi->mipi;

	/*
	 * Populate the mipi_dsi_device the panel will use. Without ->dev the
	 * panel's dev_err()/logging prints "(NULL udevice *)", and the lane/
	 * format/mode_flags are needed by mipi_dsi helpers.
	 */
	device->dev = dsi->dev;
	device->lanes = plat->lanes;
	device->format = plat->format;
	device->mode_flags = plat->mode_flags;

	plat->device = device;

	return dsi_host_init(dsi->host, device, NULL, 0, NULL);
}

static int mtk_dsi_probe(struct udevice *dev)
{
	struct mtk_dsi_priv *dsi = dev_get_priv(dev);
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct display_timing timing;
	struct udevice *mmsys_dev;
	struct regmap *regmap;
	int ret;

	/* Before relocation we don't need to do anything */
	if (!(gd->flags & GD_FLG_RELOC))
		return 0;

	dsi->dev = dev;
	dsi->data = (const struct mtk_dsi_data *)dev_get_driver_data(dev);
	if (!dsi->data) {
		dev_err(dev, "missing dsi match data\n");
		return -EINVAL;
	}

	dsi->base = dev_remap_addr(dev);
	if (!dsi->base)
		return -EINVAL;

	dsi->engine_clk = devm_clk_get(dev, "engine");
	if (IS_ERR(dsi->engine_clk))
		return PTR_ERR(dsi->engine_clk);

	dsi->digital_clk = devm_clk_get(dev, "digital");
	if (IS_ERR(dsi->digital_clk))
		return PTR_ERR(dsi->digital_clk);

	//	this is handle directly in drivers/video/mediatek/mtk_mipi_tx.c
//	dsi->hs_clk = devm_clk_get(dev, "hs");
//	if (IS_ERR(dsi->hs_clk))
//		return PTR_ERR(dsi->hs_clk);
//
	ret = uclass_get_device_by_of_path(UCLASS_MISC, "ovl0", &dsi->ovl);
	if (ret) {
		dev_err(dev, "cannot get ovl device: %s\n", errno_str(ret));
		return ret;
	}

	ret = uclass_get_device_by_of_path(UCLASS_MISC, "rdma0", &dsi->rdma);
	if (ret) {
		dev_err(dev, "cannot get rdma device: %s\n", errno_str(ret));
		return ret;
	}

	if (dsi->data->has_color_pipeline) {
		ret = uclass_get_device_by_of_path(UCLASS_MISC, "color0", &dsi->color);
		if (ret) {
			dev_err(dev, "cannot get color device: %s\n", errno_str(ret));
			return ret;
		}

		ret = uclass_get_device_by_of_path(UCLASS_MISC, "ccorr0", &dsi->ccorr);
		if (ret) {
			dev_err(dev, "cannot get ccorr device: %s\n", errno_str(ret));
			return ret;
		}

		ret = uclass_get_device_by_of_path(UCLASS_MISC, "aal0", &dsi->aal);
		if (ret) {
			dev_err(dev, "cannot get aal device: %s\n", errno_str(ret));
			return ret;
		}

		ret = uclass_get_device_by_of_path(UCLASS_MISC, "gamma0", &dsi->gamma);
		if (ret) {
			dev_err(dev, "cannot get gamma device: %s\n", errno_str(ret));
			return ret;
		}

		ret = uclass_get_device_by_of_path(UCLASS_MISC, "dither0", &dsi->dither);
		if (ret) {
			dev_err(dev, "cannot get dither device: %s\n", errno_str(ret));
			return ret;
		}
	}

	ret = uclass_get_device_by_of_path(UCLASS_MISC, "mutex0", &dsi->mutex);
	if (ret) {
		dev_err(dev, "cannot get mutex device: %s\n", errno_str(ret));
		return ret;
	}

	/*
	 * No standalone "mmsys" phandle exists in the DSI binding; the
	 * "digital" clock already comes from the mmsys config block, so
	 * reuse its clock provider device to reach the same regmap.
	 */
	mmsys_dev = dsi->digital_clk->dev;

	regmap = syscon_node_to_regmap(dev_ofnode(mmsys_dev));
	if (IS_ERR(regmap)) {
		dev_err(dev, "cannot get mmsys regmap: %ld\n", PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}

	dsi->mmsys_base = regmap_get_range(regmap, 0);
	if (!dsi->mmsys_base) {
		dev_err(dev, "cannot get mmsys base\n");
		return -ENOENT;
	}

	ret = uclass_get_device_by_phandle(UCLASS_MISC, dev, "phys", &dsi->phy);
	if (ret) {
		dev_err(dev, "cannot get phy device: %s\n", errno_str(ret));
		return ret;
	}

	ret = uclass_first_device_err(UCLASS_PANEL, &dsi->panel);
	if (ret) {
		dev_err(dev, "cannot get panel: %s\n", errno_str(ret));
		return ret;
	}

	ret = uclass_get_device(UCLASS_DSI_HOST, 0, &dsi->host);
	if (ret) {
		dev_err(dev, "cannot get dsi host: %s\n", errno_str(ret));
		return ret;
	}

	ret = mtk_dsi_host_init(dsi);
	if (ret) {
		dev_err(dev, "failed to initialize mipi dsi host\n");
		return ret;
	}

	ret = mtk_disp_mutex_ovl_dsi_enable(dsi->mutex);
	if (ret) {
		dev_err(dev, "failed to enable mutex\n");
		return ret;
	}

	mtk_ddp_ovl_to_dsi(dsi->mmsys_base, dsi->data->has_color_pipeline);

	ret = panel_get_display_timing(dsi->panel, &timing);
	if (ret) {
		dev_err(dev, "cannot get display timing\n");
		return ret;
	}

	mtk_ovl_config(dsi->ovl, timing);
	mtk_ovl_start(dsi->ovl);

	mtk_rdma_config(dsi->rdma, timing);
	mtk_rdma_start(dsi->rdma);

	if (dsi->data->has_color_pipeline) {
		mtk_color_config(dsi->color, timing);
		mtk_color_start(dsi->color);

		mtk_ccorr_config(dsi->ccorr, timing);
		mtk_ccorr_start(dsi->ccorr);

		mtk_aal_config(dsi->aal, timing);
		mtk_aal_start(dsi->aal);

		mtk_gamma_config(dsi->gamma, timing);
		mtk_gamma_start(dsi->gamma);

		mtk_dither_config(dsi->dither, timing);
		mtk_dither_start(dsi->dither);
	}

	ret = mtk_dsi_power_on(dsi, timing);
	if (ret) {
		dev_err(dev, "failed to power on\n");
		return ret;
	}

	mtk_dsi_set_display_mode(dsi, timing);

	mtk_dsi_lane_ready(dsi);

	mtk_ovl_config(dsi->ovl, timing);

	mtk_ovl_layer(dsi->ovl, plat, timing);

	mmu_set_region_dcache_behaviour(plat->base, ALIGN(plat->size, MMU_SECTION_SIZE),
					DCACHE_WRITEBACK);
	video_set_flush_dcache(dev, true);

	ret = panel_set_backlight(dsi->panel, BACKLIGHT_DEFAULT);
	if (ret && ret != -ENOSYS) {
		dev_err(dev, "failed to set backlight\n");
		return ret;
	}

	ret = panel_enable_backlight(dsi->panel);
	if (ret) {
		dev_err(dev, "failed to enable backlight\n");
		return ret;
	}

	mtk_dsi_output_enable(dsi);

	return 0;
}

static int mtk_dsi_remove(struct udevice *dev)
{
	struct mtk_dsi_priv *dsi = dev_get_priv(dev);

	/* Nothing to undo if we never made it past the pre-relocation stub. */
	if (!dsi->base)
		return 0;

	/*
	 * Force the DSI host back to an idle command-mode state before Linux
	 * boots. Linux's mtk_dsi driver can in principle take over a host
	 * still actively driving video mode (mtk_dsi_host_transfer() checks
	 * DSI_MODE_CTRL and tries a graceful stop), but that path waits on a
	 * VM_DONE irq that never arrives because mtk_dsi_poweron() has
	 * already reset the engine by the time it runs, so it just times out
	 * and the panel fails to init. Leaving DSI_MODE_CTRL/DSI_START
	 * cleared avoids that broken handoff path, at the cost of blanking
	 * the splash right before Linux's own display driver probes.
	 *
	 * TODO: a real flicker-free handoff (kernel picking up the still-
	 * running video mode instead of us tearing it down here) would be
	 * nicer, but needs the above kernel-side issue fixed first.
	 *
	 * Note that DM powers the display domain off right after this, which
	 * is what actually resets the rest of the pipeline (OVL, RDMA, mutex
	 * and the SMI larb IOMMU bypass) for the kernel.
	 */
	writel(0, dsi->base + DSI_START);
	writel(CMD_MODE, dsi->base + DSI_MODE_CTRL);
	mtk_dsi_reset_engine(dsi);
	mtk_dsi_clk_hs_mode(dsi, false);
	mtk_dsi_mask(dsi, DSI_CON_CTRL, 0, DSI_EN);

	return 0;
}

static int mtk_dsi_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	const struct mtk_dsi_data *data =
		(const struct mtk_dsi_data *)dev_get_driver_data(dev);
	struct udevice *host_dev;
	int ret;

	plat->size = MTK_DSI_MAX_WIDTH * MTK_DSI_MAX_HEIGHT * VNBYTES(VIDEO_BPP32);

	/*
	 * The DSI host (UCLASS_DSI_HOST) shares this same DT node. DM only
	 * binds one driver per node, so bind the host driver by name onto our
	 * ofnode as a child device. It is retrieved later in probe via
	 * uclass_get_device(UCLASS_DSI_HOST, ...).
	 */
	ret = device_bind_driver_to_node(dev, "mtk-dsi-host", "mtk-dsi-host",
					 dev_ofnode(dev), &host_dev);
	if (ret)
		return ret;

	host_dev->driver_data = data->host_cmdq_off;

	return dm_scan_fdt_dev(dev);
}

static const struct udevice_id mtk_dsi_ids[] = {
	{ .compatible = "mediatek,mt8183-dsi", .data = (ulong)&mt8183_dsi_data },
	{ .compatible = "mediatek,mt8188-dsi", .data = (ulong)&mt8188_dsi_data },
	{ .compatible = "mediatek,mt8366-dsi", .data = (ulong)&mt8188_dsi_data },
	{}
};

U_BOOT_DRIVER(mtk_dsi) = {
	.name	   = "mtk-dsi",
	.id	   = UCLASS_VIDEO,
	.of_match  = mtk_dsi_ids,
	.probe	   = mtk_dsi_probe,
	.bind	   = mtk_dsi_bind,
	.remove	   = mtk_dsi_remove,
	.priv_auto = sizeof(struct mtk_dsi_priv),
	.flags	   = DM_FLAG_PRE_RELOC | DM_FLAG_OS_PREPARE,
};
