/*
 * Copyright (c) 2018, Mellanox Technologies inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "uverbs.h"
#include <rdma/ib_user_ioctl_cmds.h>

int uverbs_destroy_def_handler(struct ib_device *ib_dev,
			       struct ib_uverbs_file *file,
			       struct uverbs_attr_bundle *attrs)
{
	return 0;
}
EXPORT_SYMBOL(uverbs_destroy_def_handler);

DECLARE_IDR_OBJECT_TYPE(UVERBS_OBJECT_AH, struct ib_uobject, 0);
DECLARE_FD_OBJECT_TYPE(UVERBS_OBJECT_COMP_CHANNEL,
		       struct ib_uverbs_completion_event_file,
		       "[infinibandevent]", O_RDONLY, 0);
DECLARE_IDR_OBJECT_TYPE(UVERBS_OBJECT_CQ, struct ib_ucq_object, 0);
/* 1 is used in order to free the DM after MRs */
DECLARE_IDR_OBJECT_TYPE(UVERBS_OBJECT_DM, struct ib_uobject, 1);
DECLARE_IDR_OBJECT_TYPE(UVERBS_OBJECT_FLOW, struct ib_uflow_object, 0);
DECLARE_IDR_OBJECT_TYPE(UVERBS_OBJECT_FLOW_ACTION, struct ib_uobject, 0);
/* 1 is used in order to free the MR after all the MWs */
DECLARE_IDR_OBJECT_TYPE(UVERBS_OBJECT_MR, struct ib_uobject, 1);
DECLARE_IDR_OBJECT_TYPE(UVERBS_OBJECT_MW, struct ib_uobject, 0);
DECLARE_IDR_OBJECT_TYPE(UVERBS_OBJECT_PD, struct ib_uobject, 0);
DECLARE_IDR_OBJECT_TYPE(UVERBS_OBJECT_QP, struct ib_uqp_object, 0);
DECLARE_IDR_OBJECT_TYPE(UVERBS_OBJECT_RWQ_IND_TBL, struct ib_uobject, 0);
DECLARE_IDR_OBJECT_TYPE(UVERBS_OBJECT_SRQ, struct ib_usrq_object, 0);
DECLARE_IDR_OBJECT_TYPE(UVERBS_OBJECT_WQ, struct ib_uwq_object, 0);
DECLARE_IDR_OBJECT_TYPE(UVERBS_OBJECT_XRCD, struct ib_uxrcd_object, 0);

/* The multi-include header will create a bunch of static const data that
 * creates a struct uverbs_object_tree_def tree that contains everything in
 * the uapi header.
 */
#define UVERBS_TREE_NAME tree
#define UVERBS_HDR <uapi/rdma/ib_user_ioctl_cmds.h>
#include <rdma/specs/create_data.h>

const struct uverbs_object_tree_def *uverbs_default_get_objects(void)
{
	return &tree;
}
