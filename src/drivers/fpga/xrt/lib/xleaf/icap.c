// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA ICAP Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 *      Sonal Santan <sonals@xilinx.com>
 *      Max Zhen <maxz@xilinx.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include "metadata.h"
#include "xleaf.h"
#include "xleaf/icap.h"
#include "xclbin-helper.h"

#define XRT_ICAP "xrt_icap"

#define ICAP_ERR(icap, fmt, arg...)	\
	xrt_err((icap)->pdev, fmt "\n", ##arg)
#define ICAP_WARN(icap, fmt, arg...)	\
	xrt_warn((icap)->pdev, fmt "\n", ##arg)
#define ICAP_INFO(icap, fmt, arg...)	\
	xrt_info((icap)->pdev, fmt "\n", ##arg)
#define ICAP_DBG(icap, fmt, arg...)	\
	xrt_dbg((icap)->pdev, fmt "\n", ##arg)

/*
 * AXI-HWICAP IP register layout
 */
#define ICAP_REG_GIER(base)	((base) + 0x1C)
#define ICAP_REG_ISR(base)	((base) + 0x20)
#define ICAP_REG_IER(base)	((base) + 0x28)
#define ICAP_REG_WF(base)	((base) + 0x100)
#define ICAP_REG_RF(base)	((base) + 0x104)
#define ICAP_REG_SZ(base)	((base) + 0x108)
#define ICAP_REG_CR(base)	((base) + 0x10C)
#define ICAP_REG_SR(base)	((base) + 0x110)
#define ICAP_REG_WFV(base)	((base) + 0x114)
#define ICAP_REG_RFO(base)	((base) + 0x118)
#define ICAP_REG_ASR(base)	((base) + 0x11C)

struct icap {
	struct platform_device	*pdev;
	void __iomem		*reg_base;
	struct mutex		icap_lock; /* icap dev lock */

	unsigned int		idcode;
};

static inline u32 reg_rd(void __iomem *reg)
{
	if (!reg)
		return -1;

	return readl(reg);
}

static inline void reg_wr(void __iomem *reg, u32 val)
{
	if (!reg)
		return;

	writel(val, reg);
}

static int wait_for_done(struct icap *icap)
{
	int	i = 0;
	u32	w;

	WARN_ON(!mutex_is_locked(&icap->icap_lock));
	for (i = 0; i < 10; i++) {
		udelay(5);
		w = reg_rd(ICAP_REG_SR(icap->reg_base));
		ICAP_INFO(icap, "XHWICAP_SR: %x", w);
		if (w & 0x5)
			return 0;
	}

	ICAP_ERR(icap, "bitstream download timeout");
	return -ETIMEDOUT;
}

static int icap_write(struct icap *icap, const u32 *word_buf, int size)
{
	u32 value = 0;
	int i;

	for (i = 0; i < size; i++) {
		value = be32_to_cpu(word_buf[i]);
		reg_wr(ICAP_REG_WF(icap->reg_base), value);
	}

	reg_wr(ICAP_REG_CR(icap->reg_base), 0x1);

	for (i = 0; i < 20; i++) {
		value = reg_rd(ICAP_REG_CR(icap->reg_base));
		if ((value & 0x1) == 0)
			return 0;
		ndelay(50);
	}

	ICAP_ERR(icap, "writing %d dwords timeout", size);
	return -EIO;
}

static int bitstream_helper(struct icap *icap, const u32 *word_buffer,
			    u32 word_count)
{
	int wr_fifo_vacancy = 0;
	u32 word_written = 0;
	u32 remain_word;
	int err = 0;

	WARN_ON(!mutex_is_locked(&icap->icap_lock));
	for (remain_word = word_count; remain_word > 0;
		remain_word -= word_written, word_buffer += word_written) {
		wr_fifo_vacancy = reg_rd(ICAP_REG_WFV(icap->reg_base));
		if (wr_fifo_vacancy <= 0) {
			ICAP_ERR(icap, "no vacancy: %d", wr_fifo_vacancy);
			err = -EIO;
			break;
		}
		word_written = (wr_fifo_vacancy < remain_word) ?
			wr_fifo_vacancy : remain_word;
		if (icap_write(icap, word_buffer, word_written) != 0) {
			ICAP_ERR(icap, "write failed remain %d, written %d",
				 remain_word, word_written);
			err = -EIO;
			break;
		}
	}

	return err;
}

static int icap_download(struct icap *icap, const char *buffer,
			 unsigned long length)
{
	u32	num_chars_read = XCLBIN_HWICAP_BITFILE_BUF_SZ;
	u32	byte_read;
	int	err = 0;

	mutex_lock(&icap->icap_lock);
	for (byte_read = 0; byte_read < length; byte_read += num_chars_read) {
		num_chars_read = length - byte_read;
		if (num_chars_read > XCLBIN_HWICAP_BITFILE_BUF_SZ)
			num_chars_read = XCLBIN_HWICAP_BITFILE_BUF_SZ;

		err = bitstream_helper(icap, (u32 *)buffer, num_chars_read / sizeof(u32));
		if (err)
			goto failed;
		buffer += num_chars_read;
	}

	err = wait_for_done(icap);

failed:
	mutex_unlock(&icap->icap_lock);

	return err;
}

/*
 * Run the following sequence of canned commands to obtain IDCODE of the FPGA
 */
static void icap_probe_chip(struct icap *icap)
{
	reg_rd(ICAP_REG_SR(icap->reg_base));
	reg_rd(ICAP_REG_SR(icap->reg_base));
	reg_wr(ICAP_REG_GIER(icap->reg_base), 0x0);
	reg_rd(ICAP_REG_WFV(icap->reg_base));
	reg_wr(ICAP_REG_WF(icap->reg_base), 0xffffffff);
	reg_wr(ICAP_REG_WF(icap->reg_base), 0xaa995566);
	reg_wr(ICAP_REG_WF(icap->reg_base), 0x20000000);
	reg_wr(ICAP_REG_WF(icap->reg_base), 0x20000000);
	reg_wr(ICAP_REG_WF(icap->reg_base), 0x28018001);
	reg_wr(ICAP_REG_WF(icap->reg_base), 0x20000000);
	reg_wr(ICAP_REG_WF(icap->reg_base), 0x20000000);
	reg_rd(ICAP_REG_CR(icap->reg_base));
	reg_wr(ICAP_REG_CR(icap->reg_base), 0x1);
	reg_rd(ICAP_REG_CR(icap->reg_base));
	reg_rd(ICAP_REG_CR(icap->reg_base));
	reg_rd(ICAP_REG_SR(icap->reg_base));
	reg_rd(ICAP_REG_CR(icap->reg_base));
	reg_rd(ICAP_REG_SR(icap->reg_base));
	reg_wr(ICAP_REG_SZ(icap->reg_base), 0x1);
	reg_rd(ICAP_REG_CR(icap->reg_base));
	reg_wr(ICAP_REG_CR(icap->reg_base), 0x2);
	reg_rd(ICAP_REG_RFO(icap->reg_base));
	icap->idcode = reg_rd(ICAP_REG_RF(icap->reg_base));
	reg_rd(ICAP_REG_CR(icap->reg_base));
}

static int
xrt_icap_leaf_call(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct xrt_icap_wr *wr_arg = arg;
	struct icap *icap;
	int ret = 0;

	icap = platform_get_drvdata(pdev);

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		/* Does not handle any event. */
		break;
	case XRT_ICAP_WRITE:
		ret = icap_download(icap, wr_arg->xiiw_bit_data,
				    wr_arg->xiiw_data_len);
		break;
	case XRT_ICAP_IDCODE:
		*(u64 *)arg = icap->idcode;
		break;
	default:
		ICAP_ERR(icap, "unknown command %d", cmd);
		return -EINVAL;
	}

	return ret;
}

static int xrt_icap_remove(struct platform_device *pdev)
{
	struct icap *icap;

	icap = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, icap);

	return 0;
}

static int xrt_icap_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct icap *icap;
	int ret = 0;

	icap = devm_kzalloc(&pdev->dev, sizeof(*icap), GFP_KERNEL);
	if (!icap)
		return -ENOMEM;

	icap->pdev = pdev;
	platform_set_drvdata(pdev, icap);
	mutex_init(&icap->icap_lock);

	xrt_info(pdev, "probing");
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res) {
		icap->reg_base = ioremap(res->start, res->end - res->start + 1);
		if (!icap->reg_base) {
			xrt_err(pdev, "map base failed %pR", res);
			ret = -EIO;
			goto failed;
		}
	}

	icap_probe_chip(icap);
failed:
	return ret;
}

static struct xrt_subdev_endpoints xrt_icap_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names[]) {
			{ .ep_name = XRT_MD_NODE_FPGA_CONFIG },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_subdev_drvdata xrt_icap_data = {
	.xsd_dev_ops = {
		.xsd_leaf_call = xrt_icap_leaf_call,
	},
};

static const struct platform_device_id xrt_icap_table[] = {
	{ XRT_ICAP, (kernel_ulong_t)&xrt_icap_data },
	{ },
};

static struct platform_driver xrt_icap_driver = {
	.driver = {
		.name = XRT_ICAP,
	},
	.probe = xrt_icap_probe,
	.remove = xrt_icap_remove,
	.id_table = xrt_icap_table,
};

void icap_leaf_init_fini(bool init)
{
	if (init)
		xleaf_register_driver(XRT_SUBDEV_ICAP, &xrt_icap_driver, xrt_icap_endpoints);
	else
		xleaf_unregister_driver(XRT_SUBDEV_ICAP);
}
