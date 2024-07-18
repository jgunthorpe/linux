// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, Intel Corporation
 */
#include <linux/fwctl.h>
#include <linux/auxiliary_bus.h>
#include <linux/rcuwait.h>
#include <linux/cxl/mailbox.h>
#include <linux/auxiliary_bus.h>
#include <linux/slab.h>
#include <uapi/fwctl/cxl.h>

struct cxlctl_uctx {
	struct fwctl_uctx uctx;
	u32 uctx_caps;
	u32 uctx_uid;
};

struct cxlctl_dev {
	struct fwctl_device fwctl;
	struct cxl_mailbox *mbox;
};

DEFINE_FREE(cxlctl, struct cxlctl_dev *, if (_T) fwctl_put(&_T->fwctl))

static int cxlctl_open_uctx(struct fwctl_uctx *uctx)
{
	struct cxlctl_uctx *cxlctl_uctx =
		container_of(uctx, struct cxlctl_uctx, uctx);

	cxlctl_uctx->uctx_caps = BIT(FWCTL_CXL_QUERY_COMMANDS) |
				 BIT(FWCTL_CXL_SEND_COMMAND);

	return 0;
}

static void cxlctl_close_uctx(struct fwctl_uctx *uctx)
{
}

static void *cxlctl_info(struct fwctl_uctx *uctx, size_t *length)
{
	struct cxlctl_uctx *cxlctl_uctx =
		container_of(uctx, struct cxlctl_uctx, uctx);
	struct fwctl_info_cxl *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	info->uctx_caps = cxlctl_uctx->uctx_caps;

	return info;
}

static bool cxlctl_validate_set_features(struct cxl_mailbox *cxl_mbox,
					 const struct fwctl_cxl_command *send_cmd,
					 enum fwctl_rpc_scope scope)
{
	struct cxl_feat_entry *feat;
	bool found = false;
	uuid_t uuid;
	u16 mask;

	if (send_cmd->in.size < sizeof(struct set_feature_input))
		return false;

	if (copy_from_user(&uuid, u64_to_user_ptr(send_cmd->in.payload),
			   sizeof(uuid)))
		return false;

	for (int i = 0; i < cxl_mbox->num_features; i++) {
		feat = &cxl_mbox->entries[i];
		if (uuid_equal(&uuid, &feat->uuid)) {
			found = true;
			break;
		}
	}

	if (!found)
		return false;

	/* Currently no user background command support */
	if (feat->effects & CXL_CMD_BACKGROUND)
		return false;

	mask = CXL_CMD_CONFIG_CHANGE_IMMEDIATE |
	       CXL_CMD_DATA_CHANGE_IMMEDIATE |
	       CXL_CMD_POLICY_CHANGE_IMMEDIATE |
	       CXL_CMD_LOG_CHANGE_IMMEDIATE;
	if (feat->effects & mask && scope >= FWCTL_RPC_DEBUG_WRITE)
		return true;

	/* These effects supported for all scope */
	if ((feat->effects & CXL_CMD_CONFIG_CHANGE_COLD_RESET ||
	     feat->effects & CXL_CMD_CONFIG_CHANGE_CONV_RESET) &&
	    scope >= FWCTL_RPC_DEBUG_READ_ONLY)
		return true;

	return false;
}

static bool cxlctl_validate_hw_cmds(struct cxl_mailbox *cxl_mbox,
				    const struct fwctl_cxl_command *send_cmd,
				    enum fwctl_rpc_scope scope)
{
	struct cxl_mem_command *cmd;

	/*
	 * Only supporting feature commands.
	 */
	if (!cxl_mbox->num_features)
		return false;

	cmd = cxl_get_mem_command(send_cmd->id);
	if (!cmd)
		return false;

	if (test_bit(cmd->info.id, cxl_mbox->enabled_cmds))
		return false;

	if (test_bit(cmd->info.id, cxl_mbox->exclusive_cmds))
		return false;

	switch (cmd->opcode) {
	case CXL_MBOX_OP_GET_SUPPORTED_FEATURES:
	case CXL_MBOX_OP_GET_FEATURE:
		if (scope >= FWCTL_RPC_DEBUG_READ_ONLY)
			return true;
		break;
	case CXL_MBOX_OP_SET_FEATURE:
		return cxlctl_validate_set_features(cxl_mbox, send_cmd, scope);
	default:
		return false;
	};

	return false;
}

static bool cxlctl_validate_query_commands(struct fwctl_rpc_cxl *rpc_in)
{
	int cmds;

	if (rpc_in->payload_size < sizeof(rpc_in->query))
		return false;

	cmds = rpc_in->query.n_commands;
	if (cmds) {
		int cmds_size = rpc_in->payload_size - sizeof(rpc_in->query);

		if (cmds != cmds_size / sizeof(struct cxl_command_info))
			return false;
	}

	return true;
}

static bool cxlctl_validate_rpc(struct fwctl_uctx *uctx,
				struct fwctl_rpc_cxl *rpc_in,
				enum fwctl_rpc_scope scope)
{
	struct cxlctl_dev *cxlctl =
		container_of(uctx->fwctl, struct cxlctl_dev, fwctl);

	switch (rpc_in->rpc_cmd) {
	case FWCTL_CXL_QUERY_COMMANDS:
		return cxlctl_validate_query_commands(rpc_in);

	case FWCTL_CXL_SEND_COMMAND:
		return cxlctl_validate_hw_cmds(cxlctl->mbox,
					       &rpc_in->send_cmd, scope);

	default:
		return false;
	}
}

static void *send_cxl_command(struct cxl_mailbox *cxl_mbox,
			      struct fwctl_cxl_command *send_cmd,
			      size_t *out_len)
{
	struct cxl_mbox_cmd mbox_cmd;
	int rc;

	rc = cxl_fwctl_send_cmd(cxl_mbox, send_cmd, &mbox_cmd, out_len);
	if (rc)
		return ERR_PTR(rc);

	*out_len = mbox_cmd.size_out;

	return mbox_cmd.payload_out;
}

static void *cxlctl_fw_rpc(struct fwctl_uctx *uctx, enum fwctl_rpc_scope scope,
			   void *in, size_t in_len, size_t *out_len)
{
	struct cxlctl_dev *cxlctl =
		container_of(uctx->fwctl, struct cxlctl_dev, fwctl);
	struct cxl_mailbox *cxl_mbox = cxlctl->mbox;
	struct fwctl_rpc_cxl *rpc_in = in;

	if (!cxlctl_validate_rpc(uctx, rpc_in, scope))
		return ERR_PTR(-EPERM);

	switch (rpc_in->rpc_cmd) {
	case FWCTL_CXL_QUERY_COMMANDS:
		return cxl_query_cmd_from_fwctl(cxl_mbox, &rpc_in->query,
						out_len);

	case FWCTL_CXL_SEND_COMMAND:
		return send_cxl_command(cxl_mbox, &rpc_in->send_cmd, out_len);

	default:
		return ERR_PTR(-EOPNOTSUPP);
	}
}

static const struct fwctl_ops cxlctl_ops = {
	.device_type = FWCTL_DEVICE_TYPE_CXL,
	.uctx_size = sizeof(struct cxlctl_uctx),
	.open_uctx = cxlctl_open_uctx,
	.close_uctx = cxlctl_close_uctx,
	.info = cxlctl_info,
	.fw_rpc = cxlctl_fw_rpc,
};

static int cxlctl_probe(struct auxiliary_device *adev,
			const struct auxiliary_device_id *id)
{
	struct cxl_mailbox *mbox = container_of(adev, struct cxl_mailbox, adev);
	struct cxlctl_dev *cxlctl __free(cxlctl) =
		fwctl_alloc_device(mbox->host, &cxlctl_ops,
				   struct cxlctl_dev, fwctl);
	int rc;

	if (!cxlctl)
		return -ENOMEM;

	cxlctl->mbox = mbox;

	rc = fwctl_register(&cxlctl->fwctl);
	if (rc)
		return rc;

	auxiliary_set_drvdata(adev, no_free_ptr(cxlctl));

	return 0;
}

static void cxlctl_remove(struct auxiliary_device *adev)
{
	struct cxlctl_dev *ctldev __free(cxlctl) = auxiliary_get_drvdata(adev);

	fwctl_unregister(&ctldev->fwctl);
}

static const struct auxiliary_device_id cxlctl_id_table[] = {
	{ .name = "CXL.fwctl", },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, cxlctl_id_table);

static struct auxiliary_driver cxlctl_driver = {
	.name = "cxl_fwctl",
	.probe = cxlctl_probe,
	.remove = cxlctl_remove,
	.id_table = cxlctl_id_table,
};

module_auxiliary_driver(cxlctl_driver);

MODULE_IMPORT_NS(CXL);
MODULE_IMPORT_NS(FWCTL);
MODULE_DESCRIPTION("CXL fwctl driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL");
