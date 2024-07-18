/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2024, Intel Corporation
 *
 * These are definitions for the mailbox command interface of CXL subsystem.
 */
#ifndef _UAPI_FWCTL_CXL_H_
#define _UAPI_FWCTL_CXL_H_

#include <linux/types.h>

enum fwctl_cxl_commands {
	FWCTL_CXL_QUERY_COMMANDS = 0,
	FWCTL_CXL_SEND_COMMAND,
};

/**
 * struct fwctl_info_cxl - ioctl(FWCTL_INFO) out_device_data
 * @uctx_caps: The command capabilities driver accepts.
 *
 * Return basic information about the FW interface available.
 */
struct fwctl_info_cxl {
	__u32 uctx_caps;
};

/*
 * CXL spec r3.1 Table 8-101 Set Feature Input Payload
 */
struct set_feature_input {
	__u8 uuid[16];
	__u32 flags;
	__u16 offset;
	__u8 version;
	__u8 reserved[9];
	__u8 data[];
} __packed;

/**
 * struct cxl_send_command - Send a command to a memory device.
 * @id: The command to send to the memory device. This must be one of the
 *	commands returned by the query command.
 * @flags: Flags for the command (input).
 * @raw: Special fields for raw commands
 * @raw.opcode: Opcode passed to hardware when using the RAW command.
 * @raw.rsvd: Must be zero.
 * @rsvd: Must be zero.
 * @retval: Return value from the memory device (output).
 * @in: Parameters associated with input payload.
 * @in.size: Size of the payload to provide to the device (input).
 * @in.rsvd: Must be zero.
 * @in.payload: Pointer to memory for payload input, payload is little endian.
 *
 * Output payload is defined with 'struct fwctl_rpc' and is the hardware output
 */
struct fwctl_cxl_command {
	__u32 id;
	__u32 flags;
	union {
		struct {
			__u16 opcode;
			__u16 rsvd;
		} raw;
		__u32 rsvd;
	};

	struct {
		__u32 size;
		__u32 rsvd;
		__u64 payload;
	} in;
};

/**
 * struct fwctl_rpc_cxl - ioctl(FWCTL_RPC) input
 */
struct fwctl_rpc_cxl {
	__u32 rpc_cmd;
	__u32 payload_size;
	__u32 version;
	__u32 rsvd;
	union {
		struct cxl_mem_query_commands query;
		struct fwctl_cxl_command send_cmd;
	};
};

struct fwctl_rpc_cxl_out {
	__u32 retval;
	__u32 rsvd;
	__u8 payload[];
};

#endif
