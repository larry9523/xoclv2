/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XRT_FLASH_H_
#define _XRT_FLASH_H_

#include "xleaf.h"

/*
 * Flash controller driver leaf calls.
 */
enum xrt_flash_leaf_cmd {
	XRT_FLASH_GET_SIZE = XRT_XLEAF_CUSTOM_BASE, /* See comments in xleaf.h */
	XRT_FLASH_READ,
};

struct xrt_flash_read {
	char *xfir_buf;
	size_t xfir_size;
	loff_t xfir_offset;
};

#endif	/* _XRT_FLASH_H_ */
