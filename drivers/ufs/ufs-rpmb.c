// SPDX-License-Identifier: GPL-2.0+
#include <dm.h>
#include <log.h>
#include <malloc.h>
#include <scsi.h>
#include <ufs.h>
#include <asm/cache.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "ufs.h"

#define RPMB_REQ_KEY		1
#define RPMB_REQ_WCOUNTER	2
#define RPMB_REQ_WRITE_DATA	3
#define RPMB_REQ_READ_DATA	4
#define RPMB_REQ_STATUS		5

#define RPMB_FRAME_SIZE		512
#define RPMB_FRAME_REQ_OFFSET	510

#define UFS_RPMB_UA_RETRIES	3

#define SEC_PROTOCOL_UFS		0xEC
#define UFS_RPMB_SEC_PROTOCOL_ID	0x01

#define GEOMETRY_DESC_RPMB_RW_SIZE	0x17

static u16 rpmb_frame_request(const void *frame)
{
	const u8 *p = frame;

	return ((u16)p[RPMB_FRAME_REQ_OFFSET] << 8) |
		p[RPMB_FRAME_REQ_OFFSET + 1];
}

static int ufs_rpmb_secprot(struct udevice *scsi_dev, unsigned int region,
			    u8 opcode, void *buf, unsigned int nframes,
			    enum dma_data_direction dir)
{
	u16 spsp = (region << 8) | UFS_RPMB_SEC_PROTOCOL_ID;
	u32 len = nframes * RPMB_FRAME_SIZE;
	struct scsi_cmd pccb;
	void *dma_buf = buf;
	void *bounce = NULL;
	int retries;
	int ret = 0;

	if (!IS_ALIGNED((uintptr_t)buf, ARCH_DMA_MINALIGN)) {
		bounce = memalign(ARCH_DMA_MINALIGN,
				  ALIGN(len, ARCH_DMA_MINALIGN));
		if (!bounce)
			return -ENOMEM;
		dma_buf = bounce;
		if (dir == DMA_TO_DEVICE)
			memcpy(bounce, buf, len);
	}

	memset(&pccb, 0, sizeof(pccb));
	pccb.lun = UFS_UPIU_RPMB_WLUN;
	pccb.cmd[0] = opcode;
	pccb.cmd[1] = SEC_PROTOCOL_UFS;
	pccb.cmd[2] = (spsp >> 8) & 0xff;
	pccb.cmd[3] = spsp & 0xff;
	pccb.cmd[4] = 0;
	pccb.cmd[5] = 0;
	pccb.cmd[6] = (len >> 24) & 0xff;
	pccb.cmd[7] = (len >> 16) & 0xff;
	pccb.cmd[8] = (len >> 8) & 0xff;
	pccb.cmd[9] = len & 0xff;
	pccb.cmd[10] = 0;
	pccb.cmd[11] = 0;
	pccb.cmdlen = 12;
	pccb.pdata = dma_buf;
	pccb.datalen = len;
	pccb.dma_dir = dir;

	for (retries = UFS_RPMB_UA_RETRIES; retries > 0; retries--) {
		ret = scsi_exec(scsi_dev, &pccb);
		if (!ret)
			break;
	}

	if (bounce) {
		if (!ret && dir == DMA_FROM_DEVICE)
			memcpy(buf, bounce, len);

		free(bounce);
	}

	return ret;
}

static int ufs_rpmb_send(struct udevice *scsi_dev, unsigned int region,
			 void *frames, unsigned int nframes)
{
	return ufs_rpmb_secprot(scsi_dev, region, SCSI_SECURITY_PROTOCOL_OUT,
				frames, nframes, DMA_TO_DEVICE);
}

static int ufs_rpmb_recv(struct udevice *scsi_dev, unsigned int region,
			 void *frames, unsigned int nframes)
{
	return ufs_rpmb_secprot(scsi_dev, region, SCSI_SECURITY_PROTOCOL_IN,
				frames, nframes, DMA_FROM_DEVICE);
}

int ufs_rpmb_route_frames(struct udevice *scsi_dev, unsigned int region,
			  void *req, unsigned long reqlen, void *rsp,
			  unsigned long rsplen)
{
	unsigned int n_req = reqlen / RPMB_FRAME_SIZE;
	unsigned int n_rsp = rsplen / RPMB_FRAME_SIZE;
	u16 request;
	int ret;

	if (!scsi_dev || reqlen % RPMB_FRAME_SIZE ||
	    rsplen % RPMB_FRAME_SIZE || !n_req)
		return -EINVAL;

	request = rpmb_frame_request(req);

	switch (request) {
	case RPMB_REQ_KEY:
	case RPMB_REQ_WRITE_DATA: {
		u8 status_frame[RPMB_FRAME_SIZE];

		ret = ufs_rpmb_send(scsi_dev, region, req, n_req);
		if (ret)
			return ret;

		memset(status_frame, 0, sizeof(status_frame));
		status_frame[RPMB_FRAME_REQ_OFFSET] = RPMB_REQ_STATUS >> 8;
		status_frame[RPMB_FRAME_REQ_OFFSET + 1] = RPMB_REQ_STATUS & 0xff;
		ret = ufs_rpmb_send(scsi_dev, region, status_frame, 1);
		if (ret)
			return ret;

		if (n_rsp < 1)
			return -EINVAL;

		return ufs_rpmb_recv(scsi_dev, region, rsp, 1);
	}
	case RPMB_REQ_WCOUNTER:
	case RPMB_REQ_READ_DATA:
		ret = ufs_rpmb_send(scsi_dev, region, req, 1);
		if (ret)
			return ret;

		if (n_rsp < 1)
			return -EINVAL;

		return ufs_rpmb_recv(scsi_dev, region, rsp, n_rsp);
	default:
		debug("ufs-rpmb: unsupported request 0x%x\n", request);
		return -EINVAL;
	}
}

static int ufs_rpmb_read_geometry(struct udevice *scsi_dev, u8 *rpmb_rw_size)
{
	struct ufs_hba *hba = dev_get_uclass_priv(scsi_dev->parent);
	u8 desc[QUERY_DESC_GEOMETRY_DEF_SIZE];
	int ret;

	ret = ufshcd_read_desc_param(hba, QUERY_DESC_IDN_GEOMETRY, 0, 0,
				     desc, sizeof(desc));
	if (ret)
		return ret;

	*rpmb_rw_size = desc[GEOMETRY_DESC_RPMB_RW_SIZE];

	return 0;
}
