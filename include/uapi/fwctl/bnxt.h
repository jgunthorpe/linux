/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2024, Broadcom Corporation
 * Copyright (c) 2024, Intel Corporation
 *
 */
#ifndef _UAPI_FWCTL_BNXT_H_
#define _UAPI_FWCTL_BNXT_H_

#include <linux/types.h>

enum fwctl_bnxt_commands {
	FWCTL_BNXT_QUERY_COMMANDS = 0,
	FWCTL_BNXT_SEND_COMMAND,
};

/**
 * struct fwctl_info_bnxt - ioctl(FWCTL_INFO) out_device_data
 * @uctx_caps: The command capabilities driver accepts.
 *
 * Return basic information about the FW interface available.
 */
struct fwctl_info_bnxt {
	__u32 uid;
	__u32 uctx_caps;
};

#if 0
struct set_feature_input {
	__u8 uuid[16];
	__u32 flags;
	__u16 offset;
	__u8 version;
	__u8 reserved[9];
	__u8 data[];
} __packed;
#endif

/* FIXME: duplicate forward declarations  */

struct input {
	__le16  req_type;
	__le16  cmpl_ring;
	__le16  seq_id;
	__le16  target_id;
	__le64  resp_addr;
};

#define HWRM_VER_GET                              0x0UL

#include <linux/pci.h>
struct bnxt_en_dev {
        struct net_device *net;
        struct pci_dev *pdev;
};

/* 
 * Format expected by bnxt_send_msg.  Decoding this by the driver does
 * not matter much, it will take whatever is passed.  It is important to
 * validate the message before sending it.
 *
 * Typically user will create msg via this sequence:
 *
 * bnxt_re_init_hwrm_hdr(req, HWRM_OPCODE);
 * bnxt_re_fill_fw_msg(&fw_msg, &req, sizeof(req), &resp,
 * 		       sizeof(resp), DFLT_HWRM_CMD_TIMEOUT);
 * bnxt_send_msg(en_dev, &fw_msg);
 *
 */
struct bnxt_fw_msg {
        void    *msg;
        int     msg_len;
        void    *resp;
        int     resp_max_len;
        int     timeout;
};

int bnxt_send_msg(struct bnxt_en_dev *edev, struct bnxt_fw_msg *fw_msg);

#endif
