/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _CXL_TYPE2_H
#define _CXL_TYPE2_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define CXL_ACCEL_OP_COMPRESS		1
#define CXL_ACCEL_OP_DECOMPRESS		2

struct cxl_accel_cmd {
	__u32 opcode;
	__u32 reserved;
	__u64 src_offset;
	__u64 src_length;
	__u64 dst_offset;
	__u64 dst_length;
	__u64 result_len;
};

#define CXL_ACCEL_IOCTL_SUBMIT	_IOWR(0xCE, 0x01, struct cxl_accel_cmd)

#endif /* _CXL_TYPE2_H */
