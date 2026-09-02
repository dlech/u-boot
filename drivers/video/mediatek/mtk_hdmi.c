// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 BayLibre, SAS
 * Author: Julien Stephan <jstephan@baylibre.com>
 */

#include <asm/io.h>
#include <asm/system.h>
#include <asm/unaligned.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dm/ofnode.h>
#include <edid.h>
#include <errno.h>
#include <generic-phy.h>
#include <i2c.h>
#include <linux/delay.h>
#include <linux/hdmi.h>
#include <video.h>

#include "mtk_disp_comp.h"
#include "mtk_disp_merge.h"
#include "mtk_ethdr.h"
#include "mtk_disp_mutex.h"
#include "mtk_disp_padding.h"
#include "mtk_dpi.h"
#include "mtk_hdmi_regs_v2.h"
#include "mtk_mdp_rdma.h"

/* VDOSYS1 display routing (SEL_IN / SOUT mux) registers */
#define SUBDISP_DISP_DPI1_SEL_IN		0xF10
#define SUBDISP_DISP_MERGE4_MOUT_SEL		0xF18
#define SUBDISP_MIXER_IN3_SEL_IN		0xF2C
#define SUBDISP_MIXER_OUT_SOUT_SEL		0xF34
#define SUBDISP_MERGE2_ASYNC_SOUT_SEL		0xF48
#define SUBDISP_MERGE4_ASYNC_SEL_IN		0xF50
#define SUBDISP_MIXER_IN3_SOUT_SEL		0xF60
#define SUBDISP_MIXER_SOUT_SEL_IN		0xF68

#define OUTPUT_TO_MIXER_IN3_SEL			0x1
#define INPUT_FROM_MERGE2_ASYNC_SOUT		0x1
#define OUTPUT_TO_DISP_MIXER			0x0
#define INPUT_FROM_DISP_MIXER			0x0
#define OUTPUT_TO_HDR_VDO_BE0			0x0
#define INPUT_FROM_HDR_VDO_BE0			0x0
#define OUTPUT_TO_DPI1_SEL			0x4
#define INPUT_FROM_VPP_MERGE4_MOUT		0x0

#define MTK_HDMI_MAX_WIDTH			3840
#define MTK_HDMI_MAX_HEIGHT			2160

#define MTK_HDMI_HPD_TIMEOUT_MS			100

/* 4 byte header + 13 byte payload */
#define HDMI_AVI_INFOFRAME_SIZE			17
/* 4 byte header + 25 byte payload */
#define HDMI_SPD_INFOFRAME_SIZE			29

/*
 * HDMI 2.0 requires TMDS clock scrambling and a 1/40 bit clock ratio for
 * TMDS character rates above 340 MHz.
 */
#define HDMI_TMDS_CLOCK_SCRAMBLE_HZ		340000000

/* the driver only ever drives 8 bits per component (no deep color) */
#define RGB444_8bit				BIT(0)
#define YCBCR444_8bit				BIT(4)
#define YCBCR420_8bit				BIT(12)

struct mtk_hdmi {
	struct udevice *dev;
	struct udevice *ddc_bus;
	struct udevice *dpi1;
	struct udevice *merge3;
	struct udevice *merge5;
	struct udevice *ethdr;
	struct udevice *mutex;
	struct udevice *padding4;
	struct udevice *padding5;
	struct udevice *rdma4;
	struct udevice *rdma5;
	struct phy phy;
	fdt_addr_t regs;
	struct clk_bulk clk_bulk;
	u64 set_csp_depth;
	enum hdmi_colorspace csp;
	enum hdmi_colorimetry colorimetry;
	struct display_timing mode;
};

enum hdmi_hpd_state {
	HDMI_PLUG_OUT = 0,
	HDMI_PLUG_IN_AND_SINK_POWER_ON,
	HDMI_PLUG_IN_ONLY,
};

static u32 mtk_hdmi_read(struct mtk_hdmi *hdmi, u32 offset)
{
	return readl(hdmi->regs + offset);
}

static void mtk_hdmi_write(struct mtk_hdmi *hdmi, u32 offset, u32 val)
{
	writel(val, hdmi->regs + offset);
}

static void mtk_hdmi_update(struct mtk_hdmi *hdmi, u32 offset, u32 val, u32 mask)
{
	fdt_addr_t reg = hdmi->regs + offset;
	u32 tmp;

	tmp = readl(reg);
	tmp = (tmp & ~mask) | (val & mask);
	writel(tmp, reg);
}

static void mtk_hdmi_enable_hdmi_mode(struct mtk_hdmi *hdmi, bool enable)
{
	u32 value = enable ? HDMI_MODE_HDMI : 0;

	mtk_hdmi_update(hdmi, TOP_CFG00, value, HDMI_MODE_HDMI);
}

static void mtk_hdmi_hw_vid_black(struct mtk_hdmi *hdmi, bool black)
{
	if (black)
		mtk_hdmi_update(hdmi, TOP_VMUTE_CFG1, REG_VMUTE_EN, REG_VMUTE_EN);
	else
		mtk_hdmi_update(hdmi, TOP_VMUTE_CFG1, 0, REG_VMUTE_EN);
}

static void mtk_hdmi_hw_reset(struct mtk_hdmi *hdmi)
{
	mtk_hdmi_update(hdmi, HDMITX_CONFIG_MT8188, 0, HDMITX_SW_RSTB);
	udelay(5);
	mtk_hdmi_update(hdmi, HDMITX_CONFIG_MT8188, HDMITX_SW_RSTB, HDMITX_SW_RSTB);
}

static inline void mtk_hdmi_set_sw_hpd(struct mtk_hdmi *hdmi, bool high)
{
	u32 value = high ? HDMITX_SW_HPD : 0;

	mtk_hdmi_update(hdmi, HDMITX_CONFIG_MT8188, value, HDMITX_SW_HPD);
}

/*
 * The driver only ever outputs 8 bits per component, so force 8 bpp and
 * disable the deep-color GCP and packets (TMDS_PACK_MODE_8BPP is 0).
 */
static void mtk_hdmi_set_8bit_color(struct mtk_hdmi *hdmi)
{
	mtk_hdmi_update(hdmi, TOP_CFG00, 0, TMDS_PACK_MODE);
	mtk_hdmi_update(hdmi, TOP_CFG00, 0, DEEPCOLOR_PKT_EN);
	mtk_hdmi_update(hdmi, TOP_MISC_CTLR, 0, DEEP_COLOR_ADD);
}

static void mtk_hdmi_yuv420_downsample_enable(struct mtk_hdmi *hdmi)
{
	mtk_hdmi_update(hdmi, HDMITX_CONFIG_MT8188, HDMI_YUV420_MODE | HDMITX_SW_HPD,
			HDMI_YUV420_MODE | HDMITX_SW_HPD);
	mtk_hdmi_update(hdmi, VID_DOWNSAMPLE_CONFIG, C444_C422_CONFIG_ENABLE,
			C444_C422_CONFIG_ENABLE);
	mtk_hdmi_update(hdmi, VID_DOWNSAMPLE_CONFIG, C422_C420_CONFIG_ENABLE,
			C422_C420_CONFIG_ENABLE);
	mtk_hdmi_update(hdmi, VID_DOWNSAMPLE_CONFIG, 0, C422_C420_CONFIG_BYPASS);
	mtk_hdmi_update(hdmi, VID_DOWNSAMPLE_CONFIG,
			C422_C420_CONFIG_OUT_CB_OR_CR,
			C422_C420_CONFIG_OUT_CB_OR_CR);
	mtk_hdmi_update(hdmi, VID_OUT_FORMAT, OUTPUT_FORMAT_DEMUX_420_ENABLE,
			OUTPUT_FORMAT_DEMUX_420_ENABLE);
}

static void mtk_hdmi_yuv420_downsample_disable(struct mtk_hdmi *hdmi)
{
	mtk_hdmi_update(hdmi, HDMITX_CONFIG_MT8188, HDMITX_SW_HPD,
			HDMI_YUV420_MODE | HDMITX_SW_HPD);
	mtk_hdmi_update(hdmi, VID_DOWNSAMPLE_CONFIG, 0, C444_C422_CONFIG_ENABLE);
	mtk_hdmi_update(hdmi, VID_DOWNSAMPLE_CONFIG, 0, C422_C420_CONFIG_ENABLE);
	mtk_hdmi_update(hdmi, VID_DOWNSAMPLE_CONFIG, C422_C420_CONFIG_BYPASS,
			C422_C420_CONFIG_BYPASS);
	mtk_hdmi_update(hdmi, VID_DOWNSAMPLE_CONFIG, 0,
			C422_C420_CONFIG_OUT_CB_OR_CR);
	mtk_hdmi_update(hdmi, VID_OUT_FORMAT, 0, OUTPUT_FORMAT_DEMUX_420_ENABLE);
}

static bool mtk_hdmi_tmds_over_340M(struct mtk_hdmi *hdmi)
{
	/* 8 bpc only, so the TMDS character rate equals the pixel clock */
	unsigned long tmds_clk = hdmi->mode.pixelclock.typ;

	return tmds_clk >= HDMI_TMDS_CLOCK_SCRAMBLE_HZ &&
	       hdmi->csp != HDMI_COLORSPACE_YUV420;
}

static inline void mtk_hdmi_enable_scrambling(struct mtk_hdmi *hdmi,
					      bool enable)
{
	udelay(150);

	if (enable)
		mtk_hdmi_update(hdmi, TOP_CFG00, SCR_ON | HDMI2_ON,
				SCR_ON | HDMI2_ON);
	else
		mtk_hdmi_update(hdmi, TOP_CFG00, 0, SCR_ON | HDMI2_ON);
}

static void mtk_hdmi_disable_abist(struct mtk_hdmi *hdmi)
{
	mtk_hdmi_update(hdmi, TOP_CFG00, 0, HDMI_ABIST_ENABLE);
}

static void mtk_hdmi_change_video_resolution(struct mtk_hdmi *hdmi)
{
	const bool is_hdmi_sink = true;
	bool is_over_340M;

	mtk_hdmi_hw_reset(hdmi);
	mtk_hdmi_set_sw_hpd(hdmi, true);
	udelay(5);

	mtk_hdmi_write(hdmi, HDCP_TOP_CTRL, 0x0);

	mtk_hdmi_set_8bit_color(hdmi);
	mtk_hdmi_enable_hdmi_mode(hdmi, is_hdmi_sink);

	udelay(10);
	mtk_hdmi_hw_vid_black(hdmi, true);

	mtk_hdmi_update(hdmi, TOP_CFG01, NULL_PKT_VSYNC_HIGH_EN,
			NULL_PKT_VSYNC_HIGH_EN | NULL_PKT_EN);

	is_over_340M = mtk_hdmi_tmds_over_340M(hdmi);
	dev_dbg(hdmi->dev, "is_over_340M: %d\n", is_over_340M);

	mtk_hdmi_enable_scrambling(hdmi, is_over_340M);

	if (hdmi->csp == HDMI_COLORSPACE_YUV420)
		mtk_hdmi_yuv420_downsample_enable(hdmi);
	else
		mtk_hdmi_yuv420_downsample_disable(hdmi);
}

static void mtk_hdmi_output_set_display_mode(struct mtk_hdmi *hdmi,
					     struct display_timing *mode)
{
	unsigned long link_rate = mode->pixelclock.typ;
	int ret;

	ret = generic_phy_configure(&hdmi->phy, &link_rate);
	if (ret)
		dev_err(hdmi->dev, "Setting clock=%u failed: %d\n",
			mode->pixelclock.typ, ret);

	mtk_hdmi_change_video_resolution(hdmi);
}

static void mtk_hdmi_convert_colorspace(struct mtk_hdmi *hdmi)
{
	switch (hdmi->set_csp_depth) {
	case YCBCR444_8bit:
		hdmi->csp = HDMI_COLORSPACE_YUV444;
		break;
	case YCBCR420_8bit:
		hdmi->csp = HDMI_COLORSPACE_YUV420;
		break;
	case RGB444_8bit:
	default:
		hdmi->csp = HDMI_COLORSPACE_RGB;
		break;
	}

	dev_dbg(hdmi->dev, "color space: %d\n", hdmi->csp);
}

/*
 * Copy at most @dst_len bytes of @src into @dst and pad the remainder with
 * spaces. Unlike strlcpy() this does not NUL-terminate, so the full field
 * width can be used for the string.
 */
static void mtk_hdmi_pad_string(u8 *dst, size_t dst_len, const char *src)
{
	size_t src_len = strlen(src);

	if (src_len > dst_len)
		src_len = dst_len;

	memcpy(dst, src, src_len);
	memset(dst + src_len, ' ', dst_len - src_len);
}

static int mtk_hdmi_setup_spd_infoframe(struct mtk_hdmi *hdmi, u8 *buffer,
					size_t bufsz, const char *vendor,
					const char *product)
{
	u8 checksum;
	int i;

	if (bufsz < HDMI_SPD_INFOFRAME_SIZE)
		return -EINVAL;

	memset(buffer, 0, HDMI_SPD_INFOFRAME_SIZE);

	/* SPD InfoFrame header */
	buffer[0] = 0x83;  /* SPD InfoFrame type */
	buffer[1] = 0x01;  /* Version */
	buffer[2] = 0x19;  /* Length (25 bytes) */

	/* Vendor name (8 bytes) and product description (16 bytes) */
	mtk_hdmi_pad_string(&buffer[4], 8, vendor);
	mtk_hdmi_pad_string(&buffer[12], 16, product);

	/* checksum over the full frame, so that the total sums to zero */
	for (checksum = 0, i = 0; i < HDMI_SPD_INFOFRAME_SIZE; i++)
		checksum += buffer[i];
	buffer[3] = 0x100 - checksum;

	return 0;
}

static int mtk_hdmi_setup_avi_infoframe(struct mtk_hdmi *hdmi, u8 *buffer,
					size_t bufsz,
					struct display_timing *mode)
{
	u8 checksum;
	int i;

	if (bufsz < HDMI_AVI_INFOFRAME_SIZE)
		return -EINVAL;

	memset(buffer, 0, HDMI_AVI_INFOFRAME_SIZE);

	/* AVI InfoFrame header */
	buffer[0] = 0x82;  /* AVI InfoFrame type */
	buffer[1] = 0x02;  /* Version */
	buffer[2] = 0x0D;  /* Length (13 bytes) */

	/* Data byte 1: Scan info, bar info, active format info, RGB/YCC */
	switch (hdmi->csp) {
	case HDMI_COLORSPACE_YUV422:
		buffer[4] = 0x20;
		break;
	case HDMI_COLORSPACE_YUV444:
		buffer[4] = 0x40;
		break;
	case HDMI_COLORSPACE_YUV420:
		buffer[4] = 0x60;
		break;
	default:
		buffer[4] = 0x00;
		break;
	}

	/* Data byte 4: Video Identification Code (VIC) */
	if (mode->hactive.typ == 1920 && mode->vactive.typ == 1080)
		buffer[7] = 16;  /* 1920x1080@60Hz */
	else if (mode->hactive.typ == 1280 && mode->vactive.typ == 720)
		buffer[7] = 4;   /* 1280x720@60Hz */
	else if (mode->hactive.typ == 720 && mode->vactive.typ == 480)
		buffer[7] = 2;   /* 720x480@60Hz */
	else
		buffer[7] = 0;   /* Unknown/unsupported timing */

	/*
	 * Data byte 2: picture aspect ratio, active portion same as
	 * picture. VIC 2 is a 4:3 mode, the others are 16:9.
	 */
	buffer[5] = buffer[7] == 2 ? 0x18 : 0x28;

	/* Data byte 3: Colorimetry, picture scaling */
	switch (hdmi->colorimetry) {
	case HDMI_COLORIMETRY_ITU_709:
		buffer[6] = 0x80;
		break;
	case HDMI_COLORIMETRY_ITU_601:
		buffer[6] = 0x40;
		break;
	default:
		buffer[6] = 0x00;
		break;
	}

	/* Data byte 5: Pixel repetition */
	buffer[8] = 0x00;  /* No pixel repetition */

	/* checksum over the full frame, so that the total sums to zero */
	for (checksum = 0, i = 0; i < HDMI_AVI_INFOFRAME_SIZE; i++)
		checksum += buffer[i];
	buffer[3] = 0x100 - checksum;

	return 0;
}

static void mtk_hdmi_hw_avi_infoframe(struct mtk_hdmi *hdmi, u8 *buf)
{
	/* Disable AVI InfoFrame first */
	mtk_hdmi_update(hdmi, TOP_INFO_EN, 0, AVI_EN_WR | AVI_EN);
	mtk_hdmi_update(hdmi, TOP_INFO_RPT, 0, AVI_RPT_EN);

	/* Write AVI InfoFrame header */
	mtk_hdmi_write(hdmi, TOP_AVI_HEADER, get_unaligned_le24(&buf[0]));

	/* Write AVI InfoFrame data packets */
	mtk_hdmi_write(hdmi, TOP_AVI_PKT00, get_unaligned_le32(&buf[3]));
	mtk_hdmi_write(hdmi, TOP_AVI_PKT01, get_unaligned_le24(&buf[7]));
	mtk_hdmi_write(hdmi, TOP_AVI_PKT02, get_unaligned_le32(&buf[10]));
	mtk_hdmi_write(hdmi, TOP_AVI_PKT03, get_unaligned_le24(&buf[14]));

	/* Clear remaining packets */
	mtk_hdmi_write(hdmi, TOP_AVI_PKT04, 0);
	mtk_hdmi_write(hdmi, TOP_AVI_PKT05, 0);

	/* Enable AVI InfoFrame */
	mtk_hdmi_update(hdmi, TOP_INFO_RPT, AVI_RPT_EN, AVI_RPT_EN);
	mtk_hdmi_update(hdmi, TOP_INFO_EN, AVI_EN_WR | AVI_EN,
			AVI_EN_WR | AVI_EN);
}

static void mtk_hdmi_hw_spd_infoframe(struct mtk_hdmi *hdmi, u8 *buf)
{
	/* Disable SPD InfoFrame first */
	mtk_hdmi_update(hdmi, TOP_INFO_EN, 0, SPD_EN_WR | SPD_EN);
	mtk_hdmi_update(hdmi, TOP_INFO_RPT, 0, SPD_RPT_EN);

	/* Write SPD InfoFrame header */
	mtk_hdmi_write(hdmi, TOP_SPDIF_HEADER, get_unaligned_le24(&buf[0]));

	/* Write SPD InfoFrame data packets */
	mtk_hdmi_write(hdmi, TOP_SPDIF_PKT00, get_unaligned_le32(&buf[3]));
	mtk_hdmi_write(hdmi, TOP_SPDIF_PKT01, get_unaligned_le24(&buf[7]));
	mtk_hdmi_write(hdmi, TOP_SPDIF_PKT02, get_unaligned_le32(&buf[10]));
	mtk_hdmi_write(hdmi, TOP_SPDIF_PKT03, get_unaligned_le24(&buf[14]));
	mtk_hdmi_write(hdmi, TOP_SPDIF_PKT04, get_unaligned_le32(&buf[17]));
	mtk_hdmi_write(hdmi, TOP_SPDIF_PKT05, get_unaligned_le24(&buf[21]));
	mtk_hdmi_write(hdmi, TOP_SPDIF_PKT06, get_unaligned_le32(&buf[24]));
	mtk_hdmi_write(hdmi, TOP_SPDIF_PKT07, buf[28]);

	/* Enable SPD InfoFrame */
	mtk_hdmi_update(hdmi, TOP_INFO_RPT, SPD_RPT_EN, SPD_RPT_EN);
	mtk_hdmi_update(hdmi, TOP_INFO_EN, SPD_EN_WR | SPD_EN,
			SPD_EN_WR | SPD_EN);
}

static void mtk_hdmi_setup_infoframes(struct mtk_hdmi *hdmi,
				      struct display_timing *mode)
{
	u8 buffer_spd[HDMI_SPD_INFOFRAME_SIZE];
	u8 buffer_avi[HDMI_AVI_INFOFRAME_SIZE];
	int ret;

	ret = mtk_hdmi_setup_avi_infoframe(hdmi, buffer_avi,
					   sizeof(buffer_avi), mode);
	if (ret) {
		dev_warn(hdmi->dev, "Failed to setup AVI infoframe: %d\n", ret);
		return;
	}

	ret = mtk_hdmi_setup_spd_infoframe(hdmi, buffer_spd,
					   sizeof(buffer_spd),
					   "MediaTek", "On-chip HDMI");
	if (ret) {
		dev_warn(hdmi->dev, "Failed to setup SPD infoframe: %d\n", ret);
		return;
	}

	/* Send the infoframes to hardware */
	mtk_hdmi_hw_avi_infoframe(hdmi, buffer_avi);
	mtk_hdmi_hw_spd_infoframe(hdmi, buffer_spd);
}

static void mtk_hdmi_controller_pre_enable(struct mtk_hdmi *hdmi,
					   struct display_timing *mode)
{
	mtk_hdmi_convert_colorspace(hdmi);
	mtk_hdmi_output_set_display_mode(hdmi, mode);
	mtk_hdmi_setup_infoframes(hdmi, mode);
}

static void mtk_hdmi_controller_enable(struct mtk_hdmi *hdmi)
{
	generic_phy_power_on(&hdmi->phy);
	mtk_hdmi_hw_vid_black(hdmi, false);
}

static inline void mtk_hdmi_disable_all_int(struct mtk_hdmi *hdmi)
{
	/* disable all tx irq */
	mtk_hdmi_write(hdmi, TOP_INT_ENABLE00, 0);
	mtk_hdmi_write(hdmi, TOP_INT_ENABLE01, 0);
}

static int mtk_hdmi_read_edid(struct mtk_hdmi *hdmi, u8 *buf, int buf_size)
{
	struct udevice *chip;
	int ret;

	if (!hdmi->ddc_bus || buf_size < EDID_SIZE)
		return -EINVAL;

	ret = i2c_get_chip(hdmi->ddc_bus, EDID_ADDR, 1, &chip);
	if (ret)
		return ret;

	ret = dm_i2c_read(chip, 0, buf, EDID_SIZE);
	if (ret) {
		dev_err(hdmi->dev, "failed to read EDID: %d\n", ret);
		return ret;
	}

	/* read the extension block, if any */
	if (buf[0x7e] != 0 && buf_size >= EDID_EXT_SIZE) {
		ret = dm_i2c_read(chip, EDID_SIZE, buf + EDID_SIZE,
				  buf_size - EDID_SIZE);
		if (ret)
			dev_warn(hdmi->dev,
				 "error reading extended EDID block\n");
	}

	return 0;
}

static enum hdmi_hpd_state mtk_hdmi_hpd_pord_status(struct mtk_hdmi *hdmi)
{
	unsigned int hpd_status;

	hpd_status = mtk_hdmi_read(hdmi, HPD_DDC_STATUS);
	if ((hpd_status & (HPD_PIN_STA | PORD_PIN_STA)) ==
	    (HPD_PIN_STA | PORD_PIN_STA))
		return HDMI_PLUG_IN_AND_SINK_POWER_ON;
	else if ((hpd_status & (HPD_PIN_STA | PORD_PIN_STA)) == HPD_PIN_STA)
		return HDMI_PLUG_IN_ONLY;
	else
		return HDMI_PLUG_OUT;
}

static int mtk_hdmi_wait_for_hpd(struct mtk_hdmi *hdmi)
{
	ulong start = get_timer(0);

	do {
		if (mtk_hdmi_hpd_pord_status(hdmi) ==
		    HDMI_PLUG_IN_AND_SINK_POWER_ON)
			return 0;

		udelay(100);
	} while (get_timer(start) < MTK_HDMI_HPD_TIMEOUT_MS);

	return -ETIMEDOUT;
}

static bool mtk_hdmi_mode_valid(void *priv, const struct display_timing *timing)
{
	return timing->hactive.typ <= MTK_HDMI_MAX_WIDTH &&
	       timing->vactive.typ <= MTK_HDMI_MAX_HEIGHT;
}

static int mtk_hdmi_get_display_timing(struct mtk_hdmi *hdmi,
				       struct display_timing *timing)
{
	u8 edid[EDID_EXT_SIZE];
	int panel_bits_per_colour;
	int ret;

	ret = mtk_hdmi_read_edid(hdmi, edid, EDID_EXT_SIZE);
	if (ret)
		return ret;

	ret = edid_get_timing_validate(edid, EDID_EXT_SIZE,
				       timing, &panel_bits_per_colour,
				       mtk_hdmi_mode_valid, NULL);
	if (ret)
		return ret;

	debug("Display timing:\n clock %u Hz\n", timing->pixelclock.typ);
	debug(" hactive: %d,\thfront_p: %d,\thback_p: %d hsync:\t%d\n",
	      timing->hactive.typ, timing->hfront_porch.typ,
	      timing->hback_porch.typ, timing->hsync_len.typ);
	debug(" vactive: %d,\tvfront_p: %d,\tvback_p: %d vsync:\t%d\n",
	      timing->vactive.typ, timing->vfront_porch.typ,
	      timing->vback_porch.typ, timing->vsync_len.typ);
	debug(" flags: 0x%x\n", timing->flags);

	return 0;
}

static void mtk_hdmi_controller_initialize(struct mtk_hdmi *hdmi,
					   struct display_timing *mode)
{
	mtk_hdmi_disable_all_int(hdmi);

	mtk_hdmi_disable_abist(hdmi);

	mtk_hdmi_controller_pre_enable(hdmi, mode);

	/* let the new configuration settle before unmuting the output */
	mdelay(50);

	mtk_hdmi_controller_enable(hdmi);
}

static void mtk_hdmi_reset_colorspace_setting(struct mtk_hdmi *hdmi)
{
	hdmi->set_csp_depth = RGB444_8bit;
	hdmi->csp = HDMI_COLORSPACE_RGB;
	hdmi->colorimetry = HDMI_COLORIMETRY_NONE;
}

static int mtk_hdmi_get_comp_by_alias(struct udevice *dev, const char *alias,
				      struct udevice **compp)
{
	struct udevice *comp;
	ofnode node;
	int ret;

	node = ofnode_get_aliases_node(alias);
	if (!ofnode_valid(node)) {
		dev_err(dev, "cannot find alias %s\n", alias);
		return -ENODEV;
	}

	ret = uclass_get_device_by_ofnode(UCLASS_MISC, node, &comp);
	if (ret) {
		dev_err(dev, "cannot get %s: %d\n", alias, ret);
		return ret;
	}

	ret = mtk_disp_comp_enable(comp);
	if (ret)
		return ret;

	/* only publish the handle once its clocks are on, so the error */
	/* unwind can rely on a non-NULL pointer meaning "enabled" */
	*compp = comp;

	return 0;
}

/* disable the pipeline component clocks, in reverse acquisition order */
static void mtk_hdmi_disable_components(struct mtk_hdmi *hdmi)
{
	struct udevice *comps[] = {
		hdmi->padding5, hdmi->padding4, hdmi->mutex, hdmi->ethdr,
		hdmi->merge5, hdmi->merge3, hdmi->rdma5, hdmi->rdma4,
		hdmi->dpi1,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(comps); i++)
		if (comps[i])
			mtk_disp_comp_disable(comps[i]);
}

/* program the vdosys1 muxes to route the mixer output to the DPI1/HDMI path */
static void mtk_hdmi_configure_vdosys1(void __iomem *base)
{
	/* MERGE2_ASYNC_SOUT --> MIXER_IN3_SEL */
	writel(OUTPUT_TO_MIXER_IN3_SEL, base + SUBDISP_MERGE2_ASYNC_SOUT_SEL);
	writel(INPUT_FROM_MERGE2_ASYNC_SOUT, base + SUBDISP_MIXER_IN3_SEL_IN);

	/* MIXER_IN3_SOUT --> DISP_MIXER */
	writel(OUTPUT_TO_DISP_MIXER, base + SUBDISP_MIXER_IN3_SOUT_SEL);

	/* DISP_MIXER --> MIXER_SOUT_SEL */
	writel(INPUT_FROM_DISP_MIXER, base + SUBDISP_MIXER_SOUT_SEL_IN);

	/* MIXER_SOUT_SEL --> HDR_VDO_BE0 */
	writel(OUTPUT_TO_HDR_VDO_BE0, base + SUBDISP_MIXER_OUT_SOUT_SEL);

	/* HDR_VDO_BE0 --> MERGE4_ASYNC_SEL */
	writel(INPUT_FROM_HDR_VDO_BE0, base + SUBDISP_MERGE4_ASYNC_SEL_IN);

	/* MERGE4_MOUT_SEL --> DPI1_SEL */
	writel(OUTPUT_TO_DPI1_SEL, base + SUBDISP_DISP_MERGE4_MOUT_SEL);
	writel(INPUT_FROM_VPP_MERGE4_MOUT, base + SUBDISP_DISP_DPI1_SEL_IN);
}

static int mtk_hdmi_probe(struct udevice *dev)
{
	struct mtk_hdmi *hdmi = dev_get_priv(dev);
	struct video_priv *priv = dev_get_uclass_priv(dev);
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct ofnode_phandle_args args;
	struct display_timing timing;
	fdt_size_t fb_region_size;
	struct udevice *vdosys1;
	void __iomem *vdosys1_base;
	ofnode node;
	int ret;

	hdmi->dev = dev;

	hdmi->regs = dev_read_addr(dev);
	if (hdmi->regs == FDT_ADDR_T_NONE)
		return -EINVAL;

	ret = clk_get_bulk(dev, &hdmi->clk_bulk);
	if (ret) {
		dev_err(dev, "failed to get clocks: %d\n", ret);
		return ret;
	}

	ret = clk_enable_bulk(&hdmi->clk_bulk);
	if (ret) {
		dev_err(dev, "failed to enable clocks: %d\n", ret);
		return ret;
	}

	ret = generic_phy_get_by_name(dev, "hdmi", &hdmi->phy);
	if (ret) {
		dev_err(dev, "failed to get HDMI PHY: %d\n", ret);
		goto err_disable_clk;
	}

	ret = uclass_get_device_by_ofnode(UCLASS_I2C,
					  dev_read_subnode(dev, "i2c"),
					  &hdmi->ddc_bus);
	if (ret) {
		dev_err(dev, "failed to get DDC I2C bus: %d\n", ret);
		goto err_disable_clk;
	}

	mtk_hdmi_reset_colorspace_setting(hdmi);

	ret = mtk_hdmi_wait_for_hpd(hdmi);
	if (ret) {
		dev_err(dev, "display is not connected\n");
		goto err_disable_clk;
	}

	ret = mtk_hdmi_get_display_timing(hdmi, &timing);
	if (ret) {
		dev_err(dev, "failed to get display timing from EDID: %d\n",
			ret);
		goto err_disable_clk;
	}

	hdmi->mode = timing;
	/* CEA-861: SD modes use ITU-601 colorimetry, HD modes ITU-709 */
	hdmi->colorimetry = timing.vactive.typ >= 720 ?
			    HDMI_COLORIMETRY_ITU_709 : HDMI_COLORIMETRY_ITU_601;

	/* this powers on the HDMI PHY */
	mtk_hdmi_controller_initialize(hdmi, &timing);

	/* Set up video private data based on the selected display timing */
	priv->bpix = VIDEO_BPP32;
	priv->xsize = timing.hactive.typ;
	priv->ysize = timing.vactive.typ;
	priv->line_length = priv->xsize * VNBYTES(VIDEO_BPP32);

	plat->size = priv->ysize * priv->line_length;

	/* the framebuffer lives in a dedicated reserved-memory region */
	ret = dev_read_phandle_with_args(dev, "memory-region", NULL, 0, 0,
					 &args);
	if (ret) {
		dev_err(dev, "no framebuffer memory-region: %d\n", ret);
		goto err_power_off_phy;
	}

	plat->base = ofnode_get_addr_size(args.node, "reg", &fb_region_size);
	if (plat->base == FDT_ADDR_T_NONE) {
		dev_err(dev, "failed to decode framebuffer region\n");
		ret = -EINVAL;
		goto err_power_off_phy;
	}

	if (fb_region_size < plat->size) {
		dev_err(dev, "framebuffer region too small: %llu < %u\n",
			(unsigned long long)fb_region_size, plat->size);
		ret = -EINVAL;
		goto err_power_off_phy;
	}

	ret = mtk_hdmi_get_comp_by_alias(dev, "dpi1", &hdmi->dpi1);
	if (ret)
		goto err_disable_comps;

	mtk_dpi_hw_enable(hdmi->dpi1);
	mtk_dpi_config(hdmi->dpi1, &timing,
		       hdmi->csp == HDMI_COLORSPACE_RGB);

	ret = mtk_hdmi_get_comp_by_alias(dev, "vdo1-rdma4", &hdmi->rdma4);
	if (ret)
		goto err_disable_comps;

	ret = mtk_mdp_rdma_config(hdmi->rdma4, plat, &timing,
				  priv->line_length, false);
	if (ret)
		goto err_disable_comps;

	ret = mtk_hdmi_get_comp_by_alias(dev, "vdo1-rdma5", &hdmi->rdma5);
	if (ret)
		goto err_disable_comps;

	ret = mtk_mdp_rdma_config(hdmi->rdma5, plat, &timing,
				  priv->line_length, true);
	if (ret)
		goto err_disable_comps;

	/*
	 * The display routing muxes live in the vdosys1 syscon register block.
	 * It is a clock provider rather than a pipeline component, so resolve
	 * it by compatible and grab the register base directly.
	 */
	node = ofnode_by_compatible(ofnode_null(), "mediatek,mt8188-vdosys1");
	if (!ofnode_valid(node)) {
		dev_err(dev, "cannot find vdosys1 node\n");
		ret = -ENODEV;
		goto err_disable_comps;
	}

	ret = uclass_get_device_by_ofnode(UCLASS_CLK, node, &vdosys1);
	if (ret) {
		dev_err(dev, "cannot get vdosys1 syscon: %d\n", ret);
		goto err_disable_comps;
	}

	vdosys1_base = dev_remap_addr(vdosys1);
	if (!vdosys1_base) {
		ret = -EINVAL;
		goto err_disable_comps;
	}

	mtk_hdmi_configure_vdosys1(vdosys1_base);

	ret = mtk_hdmi_get_comp_by_alias(dev, "merge3", &hdmi->merge3);
	if (ret)
		goto err_disable_comps;

	mtk_disp_merge_config(hdmi->merge3, priv->xsize / 2, priv->ysize,
			      priv->xsize / 2, priv->ysize,
			      priv->xsize, priv->ysize);

	ret = mtk_hdmi_get_comp_by_alias(dev, "merge5", &hdmi->merge5);
	if (ret)
		goto err_disable_comps;

	mtk_disp_merge_config(hdmi->merge5, priv->xsize, priv->ysize, 0, 0,
			      priv->xsize, priv->ysize);

	ret = mtk_hdmi_get_comp_by_alias(dev, "ethdr0", &hdmi->ethdr);
	if (ret)
		goto err_disable_comps;

	mtk_ethdr_config(hdmi->ethdr, priv->xsize, priv->ysize);

	ret = mtk_hdmi_get_comp_by_alias(dev, "mutex1", &hdmi->mutex);
	if (ret)
		goto err_disable_comps;

	mtk_disp_mutex_config_hdmi(hdmi->mutex);

	ret = mtk_hdmi_get_comp_by_alias(dev, "padding4", &hdmi->padding4);
	if (ret)
		goto err_disable_comps;

	mtk_disp_padding_config(hdmi->padding4);

	ret = mtk_hdmi_get_comp_by_alias(dev, "padding5", &hdmi->padding5);
	if (ret)
		goto err_disable_comps;

	mtk_disp_padding_config(hdmi->padding5);

	video_set_flush_dcache(dev, true);

	return 0;

err_disable_comps:
	mtk_hdmi_disable_components(hdmi);
err_power_off_phy:
	generic_phy_power_off(&hdmi->phy);
err_disable_clk:
	clk_disable_bulk(&hdmi->clk_bulk);

	return ret;
}

static const struct udevice_id mtk_hdmi_ids[] = {
	{
		.compatible = "mediatek,mt8188-hdmi-tx",
	},
	{ }
};

U_BOOT_DRIVER(mtk_hdmi) = {
	.name		= "mtk_hdmi",
	.id		= UCLASS_VIDEO,
	.of_match	= mtk_hdmi_ids,
	.probe		= mtk_hdmi_probe,
	.bind		= dm_scan_fdt_dev,
	.priv_auto	= sizeof(struct mtk_hdmi),
};
