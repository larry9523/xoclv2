// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA QSPI flash controller Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/delay.h>
#include <linux/uaccess.h>
#include "metadata.h"
#include "xleaf.h"
#include "xleaf/flash.h"

#define XRT_QSPI "xrt_qspi"

/* Status write command */
#define QSPI_CMD_STATUSREG_WRITE		0x01
/* Page Program command */
#define QSPI_CMD_PAGE_PROGRAM			0x02
/* Random read command */
#define QSPI_CMD_RANDOM_READ			0x03
/* Status read command */
#define QSPI_CMD_STATUSREG_READ			0x05
/* Enable flash write */
#define QSPI_CMD_WRITE_ENABLE			0x06
/* 4KB Subsector Erase command */
#define QSPI_CMD_4KB_SUBSECTOR_ERASE		0x20
/* Quad Input Fast Program */
#define QSPI_CMD_QUAD_WRITE			0x32
/* Extended quad input fast program */
#define QSPI_CMD_EXT_QUAD_WRITE			0x38
/* Dual Output Fast Read */
#define QSPI_CMD_DUAL_READ			0x3B
/* Clear flag register */
#define QSPI_CMD_CLEAR_FLAG_REGISTER		0x50
/* 32KB Subsector Erase command */
#define QSPI_CMD_32KB_SUBSECTOR_ERASE		0x52
/* Enhanced volatile configuration register write command */
#define QSPI_CMD_ENH_VOLATILE_CFGREG_WRITE	0x61
/* Enhanced volatile configuration register read command */
#define QSPI_CMD_ENH_VOLATILE_CFGREG_READ	0x65
/* Quad Output Fast Read */
#define QSPI_CMD_QUAD_READ			0x6B
/* Status flag read command */
#define QSPI_CMD_FLAG_STATUSREG_READ		0x70
/* Volatile configuration register write command */
#define QSPI_CMD_VOLATILE_CFGREG_WRITE		0x81
/* Volatile configuration register read command */
#define QSPI_CMD_VOLATILE_CFGREG_READ		0x85
/* Read ID Code */
#define QSPI_CMD_IDCODE_READ			0x9F
/* Non volatile configuration register write command */
#define QSPI_CMD_NON_VOLATILE_CFGREG_WRITE	0xB1
/* Non volatile configuration register read command */
#define QSPI_CMD_NON_VOLATILE_CFGREG_READ	0xB5
/* Dual IO Fast Read */
#define QSPI_CMD_DUAL_IO_READ			0xBB
/* Enhanced volatile configuration register write command */
#define QSPI_CMD_EXTENDED_ADDRESS_REG_WRITE	0xC5
/* Bulk Erase command */
#define QSPI_CMD_BULK_ERASE			0xC7
/* Enhanced volatile configuration register read command */
#define QSPI_CMD_EXTENDED_ADDRESS_REG_READ	0xC8
/* Sector Erase command */
#define QSPI_CMD_SECTOR_ERASE			0xD8
/* Quad IO Fast Read */
#define QSPI_CMD_QUAD_IO_READ			0xEB

#define QSPI_ERR(flash, fmt, arg...)	xrt_err((flash)->pdev, fmt, ##arg)
#define QSPI_WARN(flash, fmt, arg...)	xrt_warn((flash)->pdev, fmt, ##arg)
#define QSPI_INFO(flash, fmt, arg...)	xrt_info((flash)->pdev, fmt, ##arg)
#define QSPI_DBG(flash, fmt, arg...)	xrt_dbg((flash)->pdev, fmt, ##arg)

/*
 * QSPI control reg bits.
 */
#define QSPI_CR_LOOPBACK		BIT(0)
#define QSPI_CR_ENABLED			BIT(1)
#define QSPI_CR_MASTER_MODE		BIT(2)
#define QSPI_CR_CLK_POLARITY		BIT(3)
#define QSPI_CR_CLK_PHASE		BIT(4)
#define QSPI_CR_TXFIFO_RESET		BIT(5)
#define QSPI_CR_RXFIFO_RESET		BIT(6)
#define QSPI_CR_MANUAL_SLAVE_SEL	BIT(7)
#define QSPI_CR_TRANS_INHIBIT		BIT(8)
#define QSPI_CR_LSB_FIRST		BIT(9)
#define QSPI_CR_INIT_STATE		(QSPI_CR_TRANS_INHIBIT		| \
					QSPI_CR_MANUAL_SLAVE_SEL	| \
					QSPI_CR_RXFIFO_RESET		| \
					QSPI_CR_TXFIFO_RESET		| \
					QSPI_CR_ENABLED			| \
					QSPI_CR_MASTER_MODE)

/*
 * QSPI status reg bits.
 */
#define QSPI_SR_RX_EMPTY		BIT(0)
#define QSPI_SR_RX_FULL			BIT(1)
#define QSPI_SR_TX_EMPTY		BIT(2)
#define QSPI_SR_TX_FULL			BIT(3)
#define QSPI_SR_MODE_ERR		BIT(4)
#define QSPI_SR_SLAVE_MODE		BIT(5)
#define QSPI_SR_CPOL_CPHA_ERR		BIT(6)
#define QSPI_SR_SLAVE_MODE_ERR		BIT(7)
#define QSPI_SR_MSB_ERR			BIT(8)
#define QSPI_SR_LOOPBACK_ERR		BIT(9)
#define QSPI_SR_CMD_ERR			BIT(10)
#define QSPI_SR_ERRS			(QSPI_SR_CMD_ERR	|	\
					QSPI_SR_LOOPBACK_ERR	|	\
					QSPI_SR_MSB_ERR		|	\
					QSPI_SR_SLAVE_MODE_ERR	|	\
					QSPI_SR_CPOL_CPHA_ERR	|	\
					QSPI_SR_MODE_ERR)

#define MAX_NUM_OF_SLAVES	2
#define SLAVE_NONE		(-1)
#define SLAVE_SELECT_NONE	(BIT(MAX_NUM_OF_SLAVES) - 1)

/*
 * We support erasing flash memory at three page unit. Page read-modify-write
 * is done at smallest page unit.
 */
#define QSPI_LARGE_PAGE_SIZE	(32UL * 1024)
#define QSPI_HUGE_PAGE_SIZE	(64UL * 1024)
#define QSPI_PAGE_SIZE		(4UL * 1024)
#define QSPI_PAGE_MASK		(QSPI_PAGE_SIZE - 1)
#define QSPI_PAGE_ALIGN(off)	((off) & ~QSPI_PAGE_MASK)
#define QSPI_PAGE_OFFSET(off)	((off) & QSPI_PAGE_MASK)
static inline size_t QSPI_PAGE_ROUNDUP(loff_t offset)
{
	if (QSPI_PAGE_OFFSET(offset))
		return round_up(offset, QSPI_PAGE_SIZE);
	return offset + QSPI_PAGE_SIZE;
}

/*
 * Wait for condition to be true for at most 1 second.
 * Return true, if time'd out, false otherwise.
 */
#define QSPI_BUSY_WAIT(condition)					\
({									\
	const int interval = 5; /* in microsec */			\
	int retry = 1000 * 1000 / interval; /* wait for 1 second */	\
	while (retry && !(condition)) {					\
		udelay(interval);					\
		retry--;						\
	}								\
	(retry == 0);							\
})

static size_t micron_code2sectors(u8 code)
{
	size_t max_sectors = 0;

	switch (code) {
	case 0x17:
		max_sectors = 1;
		break;
	case 0x18:
		max_sectors = 1;
		break;
	case 0x19:
		max_sectors = 2;
		break;
	case 0x20:
		max_sectors = 4;
		break;
	case 0x21:
		max_sectors = 8;
		break;
	case 0x22:
		max_sectors = 16;
		break;
	default:
		break;
	}
	return max_sectors;
}

static size_t macronix_code2sectors(u8 code)
{
	if (code < 0x38 || code > 0x3c)
		return 0;
	return BIT((code - 0x38));
}

static u8 macronix_write_cmd(void)
{
	return QSPI_CMD_PAGE_PROGRAM;
}

static u8 micron_write_cmd(void)
{
	return QSPI_CMD_QUAD_WRITE;
}

/*
 * Flash memory vendor specific operations.
 */
static struct qspi_flash_vendor {
	u8 vendor_id;
	const char *vendor_name;
	size_t (*code2sectors)(u8 code);
	u8 (*write_cmd)(void);
} vendors[] = {
	{ 0x20, "micron", micron_code2sectors, micron_write_cmd },
	{ 0xc2, "macronix", macronix_code2sectors, macronix_write_cmd },
};

struct qspi_flash_addr {
	u8 slave;
	u8 sector;
	u8 addr_lo;
	u8 addr_mid;
	u8 addr_hi;
};

/*
 * QSPI flash controller IP register layout
 */
struct qspi_reg {
	u32	qspi_padding1[16];
	u32	qspi_reset;
	u32	qspi_padding2[7];
	u32	qspi_ctrl;
	u32	qspi_status;
	u32	qspi_tx;
	u32	qspi_rx;
	u32	qspi_slave;
	u32	qspi_tx_fifo;
	u32	qspi_rx_fifo;
} __packed;

struct xrt_qspi {
	struct platform_device	*pdev;
	struct resource *res;
	struct mutex io_lock;	/* qspi lock */
	size_t flash_size;
	u8 *io_buf;
	struct qspi_reg *qspi_regs;
	size_t qspi_fifo_depth;
	u8 qspi_curr_sector;
	struct qspi_flash_vendor *vendor;
	int qspi_curr_slave;
};

static inline const char *reg2name(struct xrt_qspi *flash, u32 *reg)
{
	static const char * const reg_names[] = {
		"qspi_ctrl",
		"qspi_status",
		"qspi_tx",
		"qspi_rx",
		"qspi_slave",
		"qspi_tx_fifo",
		"qspi_rx_fifo",
	};
	size_t off = (uintptr_t)reg - (uintptr_t)flash->qspi_regs;

	if (off == offsetof(struct qspi_reg, qspi_reset))
		return "qspi_reset";
	if (off < offsetof(struct qspi_reg, qspi_ctrl))
		return "padding";
	off -= offsetof(struct qspi_reg, qspi_ctrl);
	return reg_names[off / sizeof(u32)];
}

static inline u32 qspi_reg_rd(struct xrt_qspi *flash, u32 *reg)
{
	u32 val = ioread32(reg);

	QSPI_DBG(flash, "REG_RD(%s)=0x%x", reg2name(flash, reg), val);
	return val;
}

static inline void qspi_reg_wr(struct xrt_qspi *flash, u32 *reg, u32 val)
{
	QSPI_DBG(flash, "REG_WR(%s,0x%x)", reg2name(flash, reg), val);
	iowrite32(val, reg);
}

static inline u32 qspi_get_status(struct xrt_qspi *flash)
{
	return qspi_reg_rd(flash, &flash->qspi_regs->qspi_status);
}

static inline u32 qspi_get_ctrl(struct xrt_qspi *flash)
{
	return qspi_reg_rd(flash, &flash->qspi_regs->qspi_ctrl);
}

static inline void qspi_set_ctrl(struct xrt_qspi *flash, u32 ctrl)
{
	qspi_reg_wr(flash, &flash->qspi_regs->qspi_ctrl, ctrl);
}

static inline void qspi_activate_slave(struct xrt_qspi *flash, int index)
{
	u32 slave_reg;

	if (index == SLAVE_NONE)
		slave_reg = SLAVE_SELECT_NONE;
	else
		slave_reg = ~BIT(index);
	qspi_reg_wr(flash, &flash->qspi_regs->qspi_slave, slave_reg);
}

/*
 * Pull one byte from flash RX fifo.
 * So far, only 8-bit data width is supported.
 */
static inline u8 qspi_read8(struct xrt_qspi *flash)
{
	return (u8)qspi_reg_rd(flash, &flash->qspi_regs->qspi_rx);
}

/*
 * Push one byte to flash TX fifo.
 * So far, only 8-bit data width is supported.
 */
static inline void qspi_send8(struct xrt_qspi *flash, u8 val)
{
	qspi_reg_wr(flash, &flash->qspi_regs->qspi_tx, val);
}

static inline bool qspi_has_err(struct xrt_qspi *flash)
{
	u32 status = qspi_get_status(flash);

	if (!(status & QSPI_SR_ERRS))
		return false;

	QSPI_ERR(flash, "QSPI error status: 0x%x", status);
	return true;
}

/*
 * Caller should make sure the flash controller has exactly
 * len bytes in the fifo. It's an error if we pull out less.
 */
static int qspi_rx(struct xrt_qspi *flash, u8 *buf, size_t len)
{
	size_t cnt;
	u8 c;

	for (cnt = 0; cnt < len; cnt++) {
		if ((qspi_get_status(flash) & QSPI_SR_RX_EMPTY) != 0)
			return -EINVAL;

		c = qspi_read8(flash);

		if (buf)
			buf[cnt] = c;
	}

	if ((qspi_get_status(flash) & QSPI_SR_RX_EMPTY) == 0) {
		QSPI_ERR(flash, "failed to drain RX fifo");
		return -EINVAL;
	}

	if (qspi_has_err(flash))
		return -EINVAL;

	return 0;
}

/*
 * Caller should make sure the fifo is large enough to host len bytes.
 */
static int qspi_tx(struct xrt_qspi *flash, u8 *buf, size_t len)
{
	u32 ctrl = qspi_get_ctrl(flash);
	int i;

	WARN_ON(len > flash->qspi_fifo_depth);

	/* Stop transferring to the flash. */
	qspi_set_ctrl(flash, ctrl | QSPI_CR_TRANS_INHIBIT);

	/* Fill out the FIFO. */
	for (i = 0; i < len; i++)
		qspi_send8(flash, buf[i]);

	/* Start transferring to the flash. */
	qspi_set_ctrl(flash, ctrl & ~QSPI_CR_TRANS_INHIBIT);

	/* Waiting for FIFO to become empty again. */
	if (QSPI_BUSY_WAIT(qspi_get_status(flash) &
		(QSPI_SR_TX_EMPTY | QSPI_SR_ERRS))) {
		if (qspi_has_err(flash))
			QSPI_ERR(flash, "QSPI write failed");
		else
			QSPI_ERR(flash, "QSPI write timeout, status: 0x%x", qspi_get_status(flash));
		return -ETIMEDOUT;
	}

	/* Always stop transferring to the flash after we finish. */
	qspi_set_ctrl(flash, ctrl | QSPI_CR_TRANS_INHIBIT);

	if (qspi_has_err(flash))
		return -EINVAL;

	return 0;
}

/*
 * Reset both RX and TX FIFO.
 */
static int qspi_reset_fifo(struct xrt_qspi *flash)
{
	const u32 status_fifo_mask = QSPI_SR_TX_FULL | QSPI_SR_RX_FULL |
		QSPI_SR_TX_EMPTY | QSPI_SR_RX_EMPTY;
	u32 fifo_status = qspi_get_status(flash) & status_fifo_mask;

	if (fifo_status == (QSPI_SR_TX_EMPTY | QSPI_SR_RX_EMPTY))
		return 0;

	qspi_set_ctrl(flash, qspi_get_ctrl(flash) | QSPI_CR_TXFIFO_RESET |
		QSPI_CR_RXFIFO_RESET);

	if (QSPI_BUSY_WAIT((qspi_get_status(flash) & status_fifo_mask) ==
		(QSPI_SR_TX_EMPTY | QSPI_SR_RX_EMPTY))) {
		QSPI_ERR(flash, "failed to reset FIFO, status: 0x%x", qspi_get_status(flash));
		return -ETIMEDOUT;
	}
	return 0;
}

static int qspi_transaction(struct xrt_qspi *flash, u8 *buf, size_t len, bool need_output)
{
	int ret = 0;

	/* Reset both the TX and RX fifo before starting transaction. */
	ret = qspi_reset_fifo(flash);
	if (ret)
		return ret;

	/* The slave index should be within range. */
	if (flash->qspi_curr_slave >= MAX_NUM_OF_SLAVES)
		return -EINVAL;
	qspi_activate_slave(flash, flash->qspi_curr_slave);

	ret = qspi_tx(flash, buf, len);
	if (ret)
		return ret;

	if (need_output) {
		ret = qspi_rx(flash, buf, len);
	} else {
		/* Needs to drain the FIFO even when the data is not wanted. */
		qspi_rx(flash, NULL, len);
	}

	/* Always need to reset slave select register after each transaction */
	qspi_activate_slave(flash, SLAVE_NONE);

	return ret;
}

static size_t qspi_get_fifo_depth(struct xrt_qspi *flash)
{
	size_t depth = 0;
	u32 ctrl;

	/* Reset TX fifo. */
	if (qspi_reset_fifo(flash))
		return depth;

	/* Stop transferring to flash. */
	ctrl = qspi_get_ctrl(flash);
	qspi_set_ctrl(flash, ctrl | QSPI_CR_TRANS_INHIBIT);

	/*
	 * Find out fifo depth by keep pushing data to QSPI until
	 * the fifo is full. We can choose to send any data. But
	 * sending 0 seems to cause error, so pick a non-zero one.
	 */
	while (!(qspi_get_status(flash) & (QSPI_SR_TX_FULL | QSPI_SR_ERRS))) {
		qspi_send8(flash, 1);
		depth++;
	}

	/* Make sure flash is still in good shape. */
	if (qspi_has_err(flash))
		return 0;

	/* Reset RX/TX fifo and restore ctrl since we just touched them. */
	qspi_set_ctrl(flash, ctrl);
	qspi_reset_fifo(flash);

	return depth;
}

/*
 * Exec flash IO command on specified slave.
 */
static inline int qspi_exec_io_cmd(struct xrt_qspi *flash, size_t len, bool output_needed)
{
	char *buf = flash->io_buf;

	return qspi_transaction(flash, buf, len, output_needed);
}

/* Test if flash memory is ready. */
static bool qspi_is_ready(struct xrt_qspi *flash)
{
	/*
	 * Reading flash device status input needs a dummy byte
	 * after cmd byte. The output is in the 2nd byte.
	 */
	u8 cmd[2] = { QSPI_CMD_STATUSREG_READ, };
	int ret = qspi_transaction(flash, cmd, sizeof(cmd), true);

	if (ret || (cmd[1] & 0x1)) // flash device is busy
		return false;

	return true;
}

static int qspi_enable_write(struct xrt_qspi *flash)
{
	u8 cmd = QSPI_CMD_WRITE_ENABLE;
	int ret = qspi_transaction(flash, &cmd, 1, false);

	if (ret)
		QSPI_ERR(flash, "Failed to enable flash write: %d", ret);
	return ret;
}

static int qspi_set_sector(struct xrt_qspi *flash, u8 sector)
{
	int ret = 0;
	u8 cmd[] = { QSPI_CMD_EXTENDED_ADDRESS_REG_WRITE, sector };

	if (sector == flash->qspi_curr_sector)
		return 0;

	QSPI_DBG(flash, "setting sector to %d", sector);

	ret = qspi_enable_write(flash);
	if (ret)
		return ret;

	ret = qspi_transaction(flash, cmd, sizeof(cmd), false);
	if (ret) {
		QSPI_ERR(flash, "Failed to set sector %d: %d", sector, ret);
		return ret;
	}

	flash->qspi_curr_sector = sector;
	return ret;
}

/* For 24 bit addressing. */
static inline void qspi_offset2faddr(loff_t addr, struct qspi_flash_addr *faddr)
{
	faddr->slave = (u8)(addr >> 56);
	faddr->sector = (u8)(addr >> 24);
	faddr->addr_lo = (u8)(addr);
	faddr->addr_mid = (u8)(addr >> 8);
	faddr->addr_hi = (u8)(addr >> 16);
}

static inline loff_t qspi_faddr2offset(struct qspi_flash_addr *faddr)
{
	loff_t off = 0;

	off |= faddr->sector;
	off <<= 8;
	off |= faddr->addr_hi;
	off <<= 8;
	off |= faddr->addr_mid;
	off <<= 8;
	off |= faddr->addr_lo;
	off |= ((u64)faddr->slave) << 56;
	return off;
}

/* IO cmd starts with op code followed by address. */
static inline int qspi_setup_io_cmd_header(struct xrt_qspi *flash,
					   u8 op, struct qspi_flash_addr *faddr, size_t *header_len)
{
	int ret = 0;

	/* Set sector (the high byte of a 32-bit address), if needed. */
	ret = qspi_set_sector(flash, faddr->sector);
	if (ret == 0) {
		/* The rest of address bytes are in cmd. */
		flash->io_buf[0] = op;
		flash->io_buf[1] = faddr->addr_hi;
		flash->io_buf[2] = faddr->addr_mid;
		flash->io_buf[3] = faddr->addr_lo;
		*header_len = 4;
	}
	return ret;
}

static bool qspi_wait_until_ready(struct xrt_qspi *flash)
{
	if (QSPI_BUSY_WAIT(qspi_is_ready(flash))) {
		QSPI_ERR(flash, "QSPI flash device is not ready");
		return false;
	}
	return true;
}

/*
 * Do one FIFO read from flash.
 * @cnt contains bytes actually read on successful return.
 */
static int qspi_fifo_rd(struct xrt_qspi *flash, loff_t off, u8 *buf, size_t *cnt)
{
	/* For read cmd, we need to exclude a few more dummy bytes in FIFO. */
	const size_t read_dummy_len = 4;

	int ret;
	struct qspi_flash_addr faddr;
	size_t header_len, total_len, payload_len;

	/* Should not cross page bundary. */
	WARN_ON(off + *cnt > QSPI_PAGE_ROUNDUP(off));
	qspi_offset2faddr(off, &faddr);

	ret = qspi_setup_io_cmd_header(flash, QSPI_CMD_QUAD_READ, &faddr, &header_len);
	if (ret)
		return ret;

	/* Figure out length of IO for this read. */

	/*
	 * One read should not be more than one fifo depth, so that we don't
	 * overrun flash->io_buf.
	 * The first header_len + read_dummy_len bytes in output buffer are
	 * always garbage, need to make room for them. What a wonderful memory
	 * controller!!
	 */
	payload_len = min(*cnt, flash->qspi_fifo_depth - header_len - read_dummy_len);
	total_len = payload_len + header_len + read_dummy_len;

	QSPI_DBG(flash, "reading %zu bytes @0x%llx", payload_len, off);

	/* Now do the read. */

	/*
	 * You tell the memory controller how many bytes you want to read
	 * by writing that many bytes to it. How hard would it be to just
	 * add one more integer to specify the length in the input cmd?!
	 */
	ret = qspi_exec_io_cmd(flash, total_len, true);
	if (ret)
		return ret;

	/* Copy out the output. Skip the garbage part. */
	memcpy(buf, &flash->io_buf[header_len + read_dummy_len], payload_len);
	*cnt = payload_len;
	return 0;
}

/*
 * Do one FIFO write to flash. Assuming erase is already done.
 * @cnt contains bytes actually written on successful return.
 */
static int qspi_fifo_wr(struct xrt_qspi *flash, loff_t off, u8 *buf, size_t *cnt)
{
	/*
	 * For write cmd, we can't write more than write_max_len bytes in one
	 * IO request even though we have larger fifo. Otherwise, writes will
	 * randomly fail.
	 */
	const size_t write_max_len = 128UL;

	int ret;
	struct qspi_flash_addr faddr;
	size_t header_len, total_len, payload_len;

	qspi_offset2faddr(off, &faddr);

	ret = qspi_setup_io_cmd_header(flash, flash->vendor->write_cmd(), &faddr, &header_len);
	if (ret)
		return ret;

	/* Figure out length of IO for this write. */

	/*
	 * One IO should not be more than one fifo depth, so that we don't
	 * overrun flash->io_buf. And we don't go beyond the write_max_len;
	 */
	payload_len = min(*cnt, flash->qspi_fifo_depth - header_len);
	payload_len = min(payload_len, write_max_len);
	total_len = payload_len + header_len;

	QSPI_DBG(flash, "writing %zu bytes @0x%llx", payload_len, off);

	/* Copy in payload after header. */
	memcpy(&flash->io_buf[header_len], buf, payload_len);

	/* Now do the write. */

	ret = qspi_enable_write(flash);
	if (ret)
		return ret;
	ret = qspi_exec_io_cmd(flash, total_len, false);
	if (ret)
		return ret;
	if (!qspi_wait_until_ready(flash))
		return -EINVAL;

	*cnt = payload_len;
	return 0;
}

/*
 * Load/store the whole buf of data from/to flash memory.
 */
static int qspi_buf_rdwr(struct xrt_qspi *flash, u8 *buf, loff_t off, size_t len, bool write)
{
	int ret = 0;
	size_t n, curlen;

	for (n = 0; ret == 0 && n < len; n += curlen) {
		curlen = len - n;
		if (write)
			ret = qspi_fifo_wr(flash, off + n, &buf[n], &curlen);
		else
			ret = qspi_fifo_rd(flash, off + n, &buf[n], &curlen);
	}

	/*
	 * Yield CPU after every buf IO so that Linux does not complain
	 * about CPU soft lockup.
	 */
	schedule();
	return ret;
}

static u8 qspi_erase_cmd(size_t pagesz)
{
	u8 cmd = 0;
	const size_t onek = 1024;

	WARN_ON(!IS_ALIGNED(pagesz, onek));
	switch (pagesz / onek) {
	case 4:
		cmd = QSPI_CMD_4KB_SUBSECTOR_ERASE;
		break;
	case 32:
		cmd = QSPI_CMD_32KB_SUBSECTOR_ERASE;
		break;
	case 64:
		cmd = QSPI_CMD_SECTOR_ERASE;
		break;
	default:
		WARN_ON(1);
		break;
	}
	return cmd;
}

/*
 * Erase one flash page.
 */
static int qspi_page_erase(struct xrt_qspi *flash, loff_t off, size_t pagesz)
{
	int ret = 0;
	struct qspi_flash_addr faddr;
	size_t cmdlen;
	u8 cmd = qspi_erase_cmd(pagesz);

	QSPI_DBG(flash, "Erasing 0x%lx bytes @0x%llx with cmd=0x%x", pagesz, off, (u32)cmd);
	WARN_ON(!IS_ALIGNED(off, pagesz));
	qspi_offset2faddr(off, &faddr);

	if (!qspi_wait_until_ready(flash))
		return -EINVAL;

	ret = qspi_setup_io_cmd_header(flash, cmd, &faddr, &cmdlen);
	if (ret)
		return ret;

	ret = qspi_enable_write(flash);
	if (ret)
		return ret;

	ret = qspi_exec_io_cmd(flash, cmdlen, false);
	if (ret) {
		QSPI_ERR(flash, "Failed to erase 0x%lx bytes @0x%llx", pagesz, off);
		return ret;
	}

	if (!qspi_wait_until_ready(flash))
		return -EINVAL;

	return 0;
}

static bool is_valid_offset(struct xrt_qspi *flash, loff_t off)
{
	struct qspi_flash_addr faddr;

	qspi_offset2faddr(off, &faddr);
	/*
	 * Assuming all flash are of the same size, we use
	 * offset into flash 0 to perform boundary check.
	 */
	faddr.slave = 0;
	return qspi_faddr2offset(&faddr) < flash->flash_size;
}

static int
qspi_do_read(struct xrt_qspi *flash, char *kbuf, size_t n, loff_t off)
{
	u8 *page = NULL;
	size_t cnt = 0;
	struct qspi_flash_addr faddr;
	int ret = 0;

	page = vmalloc(QSPI_PAGE_SIZE);
	if (!page)
		return -ENOMEM;

	mutex_lock(&flash->io_lock);

	qspi_offset2faddr(off, &faddr);
	flash->qspi_curr_slave = faddr.slave;

	if (!qspi_wait_until_ready(flash))
		ret = -EINVAL;

	while (ret == 0 && cnt < n) {
		loff_t thisoff = off + cnt;
		size_t thislen = min(n - cnt, QSPI_PAGE_ROUNDUP(thisoff) - (size_t)thisoff);
		char *thisbuf = &page[QSPI_PAGE_OFFSET(thisoff)];

		ret = qspi_buf_rdwr(flash, thisbuf, thisoff, thislen, false);
		if (ret)
			break;

		memcpy(&kbuf[cnt], thisbuf, thislen);
		cnt += thislen;
	}

	mutex_unlock(&flash->io_lock);
	vfree(page);
	return ret;
}

/*
 * Read flash memory page by page into user buf.
 */
static ssize_t
qspi_read(struct file *file, char __user *ubuf, size_t n, loff_t *off)
{
	struct xrt_qspi *flash = file->private_data;
	char *kbuf = NULL;
	int ret = 0;

	QSPI_INFO(flash, "reading %zu bytes @0x%llx", n, *off);

	if (n == 0 || !is_valid_offset(flash, *off)) {
		QSPI_ERR(flash, "Can't read: out of boundary");
		return 0;
	}
	n = min(n, flash->flash_size - (size_t)*off);
	kbuf = vmalloc(n);
	if (!kbuf)
		return -ENOMEM;

	ret = qspi_do_read(flash, kbuf, n, *off);
	if (ret == 0) {
		if (copy_to_user(ubuf, kbuf, n) != 0)
			ret = -EFAULT;
	}
	vfree(kbuf);

	if (ret)
		return ret;

	*off += n;
	return n;
}

/* Read request from other parts of driver. */
static int qspi_kernel_read(struct platform_device *pdev, char *buf, size_t n, loff_t off)
{
	struct xrt_qspi *flash = platform_get_drvdata(pdev);

	QSPI_INFO(flash, "kernel reading %zu bytes @0x%llx", n, off);
	return qspi_do_read(flash, buf, n, off);
}

/*
 * Write a page. Perform read-modify-write as needed.
 * @cnt contains actual bytes copied from user on successful return.
 */
static int qspi_page_rmw(struct xrt_qspi *flash,
			 const char __user *ubuf, u8 *kbuf, loff_t off, size_t *cnt)
{
	loff_t thisoff = QSPI_PAGE_ALIGN(off);
	size_t front = QSPI_PAGE_OFFSET(off);
	size_t mid = min(*cnt, QSPI_PAGE_SIZE - front);
	size_t last = QSPI_PAGE_SIZE - front - mid;
	u8 *thiskbuf = kbuf;
	int ret;

	if (front) {
		ret = qspi_buf_rdwr(flash, thiskbuf, thisoff, front, false);
		if (ret)
			return ret;
	}
	thisoff += front;
	thiskbuf += front;
	if (copy_from_user(thiskbuf, ubuf, mid) != 0)
		return -EFAULT;
	*cnt = mid;
	thisoff += mid;
	thiskbuf += mid;
	if (last) {
		ret = qspi_buf_rdwr(flash, thiskbuf, thisoff, last, false);
		if (ret)
			return ret;
	}

	ret = qspi_page_erase(flash, QSPI_PAGE_ALIGN(off), QSPI_PAGE_SIZE);
	if (ret == 0)
		ret = qspi_buf_rdwr(flash, kbuf, QSPI_PAGE_ALIGN(off), QSPI_PAGE_SIZE, true);
	return ret;
}

static inline size_t qspi_get_page_io_size(loff_t off, size_t sz)
{
	if (IS_ALIGNED(off, QSPI_HUGE_PAGE_SIZE) && sz >= QSPI_HUGE_PAGE_SIZE)
		return QSPI_HUGE_PAGE_SIZE;
	if (IS_ALIGNED(off, QSPI_LARGE_PAGE_SIZE) && sz >= QSPI_LARGE_PAGE_SIZE)
		return QSPI_LARGE_PAGE_SIZE;
	if (IS_ALIGNED(off, QSPI_PAGE_SIZE) && sz >= QSPI_PAGE_SIZE)
		return QSPI_PAGE_SIZE;

	return 0; // can't do full page IO
}

/*
 * Try to erase and write full (large/huge) page.
 * @cnt contains actual bytes copied from user on successful return.
 * Needs to fallback to RMW, if not possible.
 */
static int qspi_page_wr(struct xrt_qspi *flash,
			const char __user *ubuf, u8 *kbuf, loff_t off, size_t *cnt)
{
	int ret;
	size_t thislen = qspi_get_page_io_size(off, *cnt);

	if (thislen == 0)
		return -EOPNOTSUPP;

	*cnt = thislen;

	if (copy_from_user(kbuf, ubuf, thislen) != 0)
		return -EFAULT;

	ret = qspi_page_erase(flash, off, thislen);
	if (ret == 0)
		ret = qspi_buf_rdwr(flash, kbuf, off, thislen, true);
	return ret;
}

/*
 * Write to flash memory page by page from user buf.
 */
static ssize_t qspi_write(struct file *file, const char __user *buf, size_t n, loff_t *off)
{
	struct xrt_qspi *flash = file->private_data;
	u8 *page = NULL;
	size_t cnt = 0;
	int ret = 0;
	struct qspi_flash_addr faddr;

	QSPI_INFO(flash, "writing %zu bytes @0x%llx", n, *off);

	if (n == 0 || !is_valid_offset(flash, *off)) {
		QSPI_ERR(flash, "Can't write: out of boundary");
		return -ENOSPC;
	}
	n = min(n, flash->flash_size - (size_t)*off);

	page = vmalloc(QSPI_HUGE_PAGE_SIZE);
	if (!page)
		return -ENOMEM;

	mutex_lock(&flash->io_lock);

	qspi_offset2faddr(*off, &faddr);
	flash->qspi_curr_slave = faddr.slave;

	if (!qspi_wait_until_ready(flash))
		ret = -EINVAL;
	while (ret == 0 && cnt < n) {
		loff_t thisoff = *off + cnt;
		const char *thisbuf = buf + cnt;
		size_t thislen = n - cnt;

		/* Try write full page. */
		ret = qspi_page_wr(flash, thisbuf, page, thisoff, &thislen);
		if (ret) {
			/* Fallback to RMW. */
			if (ret == -EOPNOTSUPP)
				ret = qspi_page_rmw(flash, thisbuf, page, thisoff, &thislen);
			if (ret)
				break;
		}
		cnt += thislen;
	}
	mutex_unlock(&flash->io_lock);

	vfree(page);
	if (ret)
		return ret;

	*off += n;
	return n;
}

static loff_t qspi_llseek(struct file *filp, loff_t off, int whence)
{
	loff_t npos;

	switch (whence) {
	case 0: /* SEEK_SET */
		npos = off;
		break;
	case 1: /* SEEK_CUR */
		npos = filp->f_pos + off;
		break;
	case 2: /* SEEK_END: no need to support */
		return -EINVAL;
	default: /* should not happen */
		return -EINVAL;
	}
	if (npos < 0)
		return -EINVAL;

	filp->f_pos = npos;
	return npos;
}

/*
 * Only allow one client at a time.
 */
static int qspi_open(struct inode *inode, struct file *file)
{
	struct xrt_qspi *flash;
	struct platform_device *pdev = xleaf_devnode_open_excl(inode);

	if (!pdev)
		return -EBUSY;

	flash = platform_get_drvdata(pdev);
	file->private_data = flash;
	return 0;
}

static int qspi_close(struct inode *inode, struct file *file)
{
	struct xrt_qspi *flash = file->private_data;

	if (!flash)
		return -EINVAL;

	file->private_data = NULL;
	xleaf_devnode_close(inode);
	return 0;
}

static ssize_t flash_type_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	/* We support only QSPI flash controller. */
	return sprintf(buf, "spi\n");
}
static DEVICE_ATTR_RO(flash_type);

static ssize_t size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct xrt_qspi *flash = dev_get_drvdata(dev);

	return sprintf(buf, "%zu\n", flash->flash_size);
}
static DEVICE_ATTR_RO(size);

static struct attribute *qspi_attrs[] = {
	&dev_attr_flash_type.attr,
	&dev_attr_size.attr,
	NULL,
};

static struct attribute_group qspi_attr_group = {
	.attrs = qspi_attrs,
};

static int qspi_remove(struct platform_device *pdev)
{
	struct xrt_qspi *flash = platform_get_drvdata(pdev);

	if (!flash)
		return -EINVAL;
	platform_set_drvdata(pdev, NULL);

	sysfs_remove_group(&DEV(flash->pdev)->kobj, &qspi_attr_group);

	if (flash->io_buf)
		vfree(flash->io_buf);

	if (flash->qspi_regs)
		iounmap(flash->qspi_regs);

	mutex_destroy(&flash->io_lock);
	return 0;
}

static int qspi_get_ID(struct xrt_qspi *flash)
{
	u8 cmd[5] = { QSPI_CMD_IDCODE_READ, };
	int ret = qspi_transaction(flash, cmd, sizeof(cmd), true);
	struct qspi_flash_vendor *vendor = NULL;
	/*
	 * Reading flash device vendor ID. Vendor ID is in cmd[1], max vector
	 * number is in cmd[3] from output.
	 */
	int i;

	if (ret) {
		QSPI_ERR(flash, "Can't get flash memory ID, err: %d", ret);
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(vendors); i++) {
		if (cmd[1] == vendors[i].vendor_id) {
			vendor = &vendors[i];
			break;
		}
	}

	/* Find out flash vendor and size. */
	if (!vendor) {
		QSPI_ERR(flash, "Unknown flash vendor: %d", cmd[1]);
		return -EINVAL;
	}
	flash->vendor = vendor;

	flash->flash_size = vendor->code2sectors(cmd[3]) * (16 * 1024 * 1024);
	if (flash->flash_size == 0) {
		QSPI_ERR(flash, "Unknown flash memory size code: %d", cmd[3]);
		return -EINVAL;
	}
	QSPI_INFO(flash, "Flash vendor: %s, size: %zu MB",
		  vendor->vendor_name, flash->flash_size / 1024 / 1024);

	return 0;
}

static int qspi_controller_probe(struct xrt_qspi *flash)
{
	int ret;

	/* Probing on first flash only. */
	flash->qspi_curr_slave = 0;

	qspi_set_ctrl(flash, QSPI_CR_INIT_STATE);

	/* Find out fifo depth before any read/write operations. */
	flash->qspi_fifo_depth = qspi_get_fifo_depth(flash);
	if (flash->qspi_fifo_depth == 0)
		return -EINVAL;
	QSPI_DBG(flash, "QSPI FIFO depth is: %zu", flash->qspi_fifo_depth);

	if (!qspi_wait_until_ready(flash))
		return -EINVAL;

	/* Update flash vendor. */
	ret = qspi_get_ID(flash);
	if (ret)
		return ret;

	flash->qspi_curr_sector = 0xff;

	return 0;
}

static int qspi_probe(struct platform_device *pdev)
{
	struct xrt_qspi *flash;
	int ret;

	flash = devm_kzalloc(DEV(pdev), sizeof(*flash), GFP_KERNEL);
	if (!flash)
		return -ENOMEM;

	platform_set_drvdata(pdev, flash);
	flash->pdev = pdev;

	mutex_init(&flash->io_lock);

	flash->res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!flash->res) {
		ret = -EINVAL;
		QSPI_ERR(flash, "empty resource");
		goto error;
	}

	flash->qspi_regs = ioremap(flash->res->start, flash->res->end - flash->res->start + 1);
	if (!flash->qspi_regs) {
		ret = -ENOMEM;
		QSPI_ERR(flash, "failed to map resource");
		goto error;
	}

	ret = qspi_controller_probe(flash);
	if (ret)
		goto error;

	flash->io_buf = vmalloc(flash->qspi_fifo_depth);
	if (!flash->io_buf) {
		ret = -ENOMEM;
		goto error;
	}

	ret  = sysfs_create_group(&DEV(pdev)->kobj, &qspi_attr_group);
	if (ret)
		QSPI_ERR(flash, "failed to create sysfs nodes");

	return 0;

error:
	QSPI_ERR(flash, "probing failed");
	qspi_remove(pdev);
	return ret;
}

static size_t qspi_get_size(struct platform_device *pdev)
{
	struct xrt_qspi *flash = platform_get_drvdata(pdev);

	return flash->flash_size;
}

static int
qspi_leaf_call(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct xrt_qspi *flash = platform_get_drvdata(pdev);
	int ret = 0;

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		/* Does not handle any event. */
		break;
	case XRT_FLASH_GET_SIZE: {
		size_t *sz = (size_t *)arg;
		*sz = qspi_get_size(pdev);
		break;
	}
	case XRT_FLASH_READ: {
		struct xrt_flash_read *rd = (struct xrt_flash_read *)arg;

		ret = qspi_kernel_read(pdev, rd->xfir_buf, rd->xfir_size, rd->xfir_offset);
		break;
	}
	default:
		QSPI_ERR(flash, "unknown flash IOCTL cmd: %d", cmd);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static struct xrt_subdev_endpoints xrt_qspi_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names []){
			{
				.ep_name = XRT_MD_NODE_FLASH_VSEC,
			},
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_subdev_drvdata qspi_data = {
	.xsd_dev_ops = {
		.xsd_leaf_call = qspi_leaf_call,
	},
	.xsd_file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = qspi_open,
			.release = qspi_close,
			.read = qspi_read,
			.write = qspi_write,
			.llseek = qspi_llseek,
		},
		.xsf_dev_name = "flash",
	},
};

static const struct platform_device_id qspi_id_table[] = {
	{ XRT_QSPI, (kernel_ulong_t)&qspi_data },
	{ },
};

static struct platform_driver xrt_qspi_driver = {
	.driver	= {
		.name    = XRT_QSPI,
	},
	.probe   = qspi_probe,
	.remove  = qspi_remove,
	.id_table = qspi_id_table,
};

void qspi_leaf_init_fini(bool init)
{
	if (init)
		xleaf_register_driver(XRT_SUBDEV_QSPI, &xrt_qspi_driver, xrt_qspi_endpoints);
	else
		xleaf_unregister_driver(XRT_SUBDEV_QSPI);
}
