// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek Video DSI host support
 *
 * Copyright (c) 2023 BayLibre, SAS.
 * Author: Julien Masson <jmasson@baylibre.com>
 */

#include <asm/io.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dsi_host.h>
#include <linux/iopoll.h>

#define DSI_START		0x00

#define DSI_INTSTA		0x0c
#define LPRX_RD_RDY_INT_FLAG	      BIT(0)
#define CMD_DONE_INT_FLAG	      BIT(1)
#define VM_DONE_INT_FLAG	      BIT(3)
#define DSI_BUSY		      BIT(31)

#define DSI_MODE_CTRL		0x14
#define MODE_CON		      GENMASK(0, 1)

#define DSI_CMDQ_SIZE		0x60
#define CMDQ_SIZE		      0x3f

#define DSI_RX_DATA0		0x74

#define DSI_RACK		0x84
#define RACK			      BIT(0)

#define CONFIG			      (0xff << 0)
#define SHORT_PACKET		      0
#define LONG_PACKET		      2
#define BTA			      BIT(2)
#define HSTX			      BIT(3)
#define DATA_ID			      (0xff << 8)
#define DATA_0			      (0xff << 16)
#define DATA_1			      (0xff << 24)

#define MTK_DSI_HOST_IS_READ(type)				\
	((type == MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM) ||	\
	 (type == MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM) ||	\
	 (type == MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM) ||	\
	 (type == MIPI_DSI_DCS_READ))

struct mtk_dsi_host_priv {
	void __iomem *base;
	struct udevice *dev;
	struct mipi_dsi_host dsi_host;
	u32 cmdq_off;
};

static inline struct mtk_dsi_host_priv *host_to_dsi(struct mipi_dsi_host *host)
{
	return container_of(host, struct mtk_dsi_host_priv, dsi_host);
}

static void mtk_dsi_host_mask(struct mtk_dsi_host_priv *dsi, u32 offset, u32 val,
			      u32 mask)
{
	void __iomem *addr = dsi->base + offset;
	u32 tmp = readl(addr) & ~mask;

	tmp |= (val & mask);
	writel(tmp, addr);
}

static void mtk_dsi_host_start(struct mtk_dsi_host_priv *dsi)
{
	writel(0, dsi->base + DSI_START);
	writel(1, dsi->base + DSI_START);
}

static int mtk_dsi_host_wait_for_idle(struct mtk_dsi_host_priv *dsi)
{
	u32 val;

	return readl_poll_timeout(dsi->base + DSI_INTSTA, val, !(val & DSI_BUSY),
				  2000000);
}

static int mtk_dsi_host_wait_for_done(struct mtk_dsi_host_priv *dsi)
{
	u32 flag = LPRX_RD_RDY_INT_FLAG | CMD_DONE_INT_FLAG | VM_DONE_INT_FLAG;
	u64 delay = 100;
	u64 timeout_us = 50 * delay;
	u32 status, tmp;

	do {
		status = readl(dsi->base + DSI_INTSTA) & flag;
		if (status) {
			do {
				mtk_dsi_host_mask(dsi, DSI_RACK, RACK, RACK);
				tmp = readl(dsi->base + DSI_INTSTA);
			} while (tmp & DSI_BUSY);

			mtk_dsi_host_mask(dsi, DSI_INTSTA, 0, status);
			break;
		}

		udelay(delay);
		timeout_us -= delay;
	} while (timeout_us);

	if (!timeout_us)
		dev_err(dsi->dev, "wait_for_done timeout: INTSTA=0x%08x\n",
			readl(dsi->base + DSI_INTSTA));

	return timeout_us ? 0 : -ETIME;
}

static void mtk_dsi_host_cmdq(struct mtk_dsi_host_priv *dsi,
			      const struct mipi_dsi_msg *msg)
{
	const char *tx_buf = msg->tx_buf;
	u8 config, cmdq_size, cmdq_off, type = msg->type;
	u32 reg_val, cmdq_mask, i;
	u32 reg_cmdq_off = dsi->cmdq_off;

	if (MTK_DSI_HOST_IS_READ(type))
		config = BTA;
	else
		config = (msg->tx_len > 2) ? LONG_PACKET : SHORT_PACKET;

	if (!(msg->flags & MIPI_DSI_MSG_USE_LPM))
		config |= HSTX;

	if (msg->tx_len > 2) {
		cmdq_size = 1 + (msg->tx_len + 3) / 4;
		cmdq_off = 4;
		cmdq_mask = CONFIG | DATA_ID | DATA_0 | DATA_1;
		reg_val = (msg->tx_len << 16) | (type << 8) | config;
	} else {
		cmdq_size = 1;
		cmdq_off = 2;
		cmdq_mask = CONFIG | DATA_ID;
		reg_val = (type << 8) | config;
	}

	for (i = 0; i < msg->tx_len; i++)
		mtk_dsi_host_mask(dsi, (reg_cmdq_off + cmdq_off + i) & (~0x3U),
				  tx_buf[i] << (((i + cmdq_off) & 3U) * 8U),
				  (0xffUL << (((i + cmdq_off) & 3U) * 8U)));

	mtk_dsi_host_mask(dsi, reg_cmdq_off, reg_val, cmdq_mask);
	mtk_dsi_host_mask(dsi, DSI_CMDQ_SIZE, cmdq_size, CMDQ_SIZE);
}

static u32 mtk_dsi_host_recv_cnt(struct mtk_dsi_host_priv *dsi, u8 type, u8 *read_data)
{
	switch (type) {
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_1BYTE:
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_1BYTE:
		return 1;
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_2BYTE:
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_2BYTE:
		return 2;
	case MIPI_DSI_RX_GENERIC_LONG_READ_RESPONSE:
	case MIPI_DSI_RX_DCS_LONG_READ_RESPONSE:
		return read_data[1] + read_data[2] * 16;
	case MIPI_DSI_RX_ACKNOWLEDGE_AND_ERROR_REPORT:
		dev_info(dsi->dev, "type is 0x02, try again\n");
		break;
	default:
		dev_warn(dsi->dev, "type(0x%x) not recognized\n", type);
		break;
	}

	return 0;
}

static ssize_t mtk_dsi_host_send_cmd(struct mtk_dsi_host_priv *dsi,
				     const struct mipi_dsi_msg *msg)
{
	int ret;

	ret = mtk_dsi_host_wait_for_idle(dsi);
	if (ret) {
		dev_err(dsi->dev, "polling dsi wait not busy timeout!\n");
		return ret;
	}

	mtk_dsi_host_cmdq(dsi, msg);
	mtk_dsi_host_start(dsi);

	return mtk_dsi_host_wait_for_done(dsi);
}

static ssize_t mtk_mipi_dsi_host_transfer(struct mipi_dsi_host *host,
					  const struct mipi_dsi_msg *msg)
{
	struct mtk_dsi_host_priv *dsi = host_to_dsi(host);
	ssize_t recv_cnt;
	u8 i, read_data[16];
	void *src_addr;

	if (readl(dsi->base + DSI_MODE_CTRL) & MODE_CON) {
		dev_err(dsi->dev, "dsi engine is not command mode\n");
		return -EINVAL;
	}

	if (mtk_dsi_host_send_cmd(dsi, msg) < 0)
		return -ETIME;

	if (!MTK_DSI_HOST_IS_READ(msg->type))
		return 0;

	if (!msg->rx_buf) {
		dev_err(dsi->dev, "dsi receive buffer size may be NULL\n");
		return -EINVAL;
	}

	for (i = 0; i < 16; i++)
		*(read_data + i) = readb(dsi->base + DSI_RX_DATA0 + i);

	recv_cnt = mtk_dsi_host_recv_cnt(dsi, read_data[0], read_data);

	if (recv_cnt > 2)
		src_addr = &read_data[4];
	else
		src_addr = &read_data[1];

	if (recv_cnt > 10)
		recv_cnt = 10;

	if (recv_cnt > msg->rx_len)
		recv_cnt = msg->rx_len;

	if (recv_cnt)
		memcpy(msg->rx_buf, src_addr, recv_cnt);

	dev_dbg(dsi->dev, "dsi get %ld byte data from the panel address(0x%x)\n",
		recv_cnt, *((u8 *)(msg->tx_buf)));

	return recv_cnt;
}

static int mtk_dsi_host_probe(struct udevice *dev)
{
	struct mtk_dsi_host_priv *dsi = dev_get_priv(dev);

	dsi->dev = dev;

	dsi->base = dev_remap_addr(dev);
	if (IS_ERR(dsi->base))
		return PTR_ERR(dsi->base);

	dsi->cmdq_off = dev_get_driver_data(dev);

	return 0;
}

static int mtk_mipi_dsi_host_attach(struct mipi_dsi_host *host,
				    struct mipi_dsi_device *device)
{
	return 0;
}

struct mipi_dsi_host_ops mtk_mipi_dsi_host_ops = {
	.attach	  = mtk_mipi_dsi_host_attach,
	.transfer = mtk_mipi_dsi_host_transfer,
};

static int mtk_dsi_host_ops_init(struct udevice *dev,
				 struct mipi_dsi_device *device,
				 struct display_timing *timings,
				 unsigned int max_data_lanes,
				 const struct mipi_dsi_phy_ops *phy_ops)
{
	struct mtk_dsi_host_priv *dsi = dev_get_priv(dev);

	dsi->dsi_host.dev = (struct device *)dev;
	dsi->dsi_host.ops = &mtk_mipi_dsi_host_ops;
	device->host = &dsi->dsi_host;

	return 0;
}

struct dsi_host_ops mtk_dsi_host_ops = {
	.init = mtk_dsi_host_ops_init,
};

/*
 * No .of_match here on purpose: this driver shares its DT node with the
 * mtk-dsi video driver (both describe dsi@... ). Since DM binds a single
 * driver per node, the mtk-dsi driver binds us by name onto the same
 * ofnode from its .bind hook (see mtk_dsi_bind()).
 */
U_BOOT_DRIVER(mtk_dsi_host) = {
	.name	   = "mtk-dsi-host",
	.id	   = UCLASS_DSI_HOST,
	.ops	   = &mtk_dsi_host_ops,
	.probe	   = mtk_dsi_host_probe,
	.priv_auto = sizeof(struct mtk_dsi_host_priv),
};
