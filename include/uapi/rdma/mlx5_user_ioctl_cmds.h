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

#if !defined(MLX5_USER_IOCTL_CMDS_H) || defined(UVERBS_HDR_MODE)
#define MLX5_USER_IOCTL_CMDS_H

#define UVERBS_DRIVER_NAME MLX5

#ifndef UVERBS_HDR_MODE
#include <rdma/ib_user_ioctl_cmds.h>

#include "ib_user_ioctl_decls.h"
#endif

START_UDRV_OBJECT(UVERBS_OBJECT_DM)
UVERBS_UDRV_OBJECT_OPTIONAL(UVERBS_METHOD_DM_ALLOC)

ADD_UDRV_ATTRIBUTES(UVERBS_METHOD_DM_ALLOC,
		    UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_ALLOC_DM_RESP_START_OFFSET,
					UVERBS_ATTR_TYPE(__u64),
					UA_FLAGS(UVERBS_ATTR_SPEC_F_MANDATORY)),
		    UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_ALLOC_DM_RESP_PAGE_INDEX,
					UVERBS_ATTR_TYPE(__u16),
					UA_FLAGS(UVERBS_ATTR_SPEC_F_MANDATORY)))

END_UDRV_OBJECT(UVERBS_OBJECT_DM)

START_UDRV_OBJECT(UVERBS_OBJECT_FLOW_ACTION)
UVERBS_UDRV_OBJECT_OPTIONAL(UVERBS_METHOD_FLOW_ACTION_ESP_CREATE)

ADD_UDRV_ATTRIBUTES(UVERBS_METHOD_FLOW_ACTION_ESP_CREATE,
		    UVERBS_ATTR_PTR_IN(MLX5_IB_ATTR_CREATE_FLOW_ACTION_FLAGS,
				       UVERBS_ATTR_TYPE(__u64),
				       UA_FLAGS(UVERBS_ATTR_SPEC_F_MANDATORY)))

END_UDRV_OBJECT(UVERBS_OBJECT_FLOW_ACTION)

#ifndef UVERBS_HDR_MODE
#include "ib_user_ioctl_decls.h"
#endif

#endif
