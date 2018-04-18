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

#include <linux/types.h>

enum rdma_remove_reason;
struct ib_device;
struct ib_uobject;
struct ib_uobject_file;
struct ib_uverbs_file;
struct uverbs_attr_bundle;

/* This header should not be included directly, instead it is included by a
 * support header that defines UVERBS_HDR. It includes the specs header from
 * uapi/ with a different macro set to create the kernel prototypes.
 */

#define DECLARE_UVERBS_METHOD(_method_id, _id_num, ...)                        \
	int UVERBS_HANDLER(_method_id)(struct ib_device *ib_dev,               \
				       struct ib_uverbs_file *file,            \
				       struct uverbs_attr_bundle *attrs);

#define DECLARE_UVERBS_METHOD_DESTROY DECLARE_UVERBS_METHOD
#define DECLARE_UVERBS_LEGACY_METHOD DECLARE_UVERBS_METHOD

/* UVERBS_FREE_HANDLER is struct uverbs_obj_idr_type destroy_object */
#define START_UVERBS_OBJECT(_object_id, _id_num)                               \
	extern const struct uverbs_object_def UVERBS_OBJECT(_object_id);       \
	int UVERBS_FREE_HANDLER(_object_id)(struct ib_uobject *uobject,        \
					    enum rdma_remove_reason why);
#define EMPTY_UVERBS_OBJECT START_UVERBS_OBJECT

/* UVERBS_FREE_HANDLER is struct uverbs_obj_fd_type context_closed */
#define EMPTY_UVERBS_FD_OBJECT(_object_id, _id_num)                            \
	extern const struct uverbs_object_def UVERBS_OBJECT(_object_id);       \
	extern const struct file_operations UVERBS_FD_FOPS(_object_id);        \
	int UVERBS_FREE_HANDLER(_object_id)(struct ib_uobject_file *uobj_file, \
					    enum rdma_remove_reason why);

/* NULL object have no kernel uobject */
#define EMPTY_UVERBS_NULL_OBJECT(_object_id, _id_num)                          \
	extern const struct uverbs_object_def UVERBS_OBJECT(_object_id);

/* UVERBS_UDRV_SUPPORTED is struct uverbs_object_def object_supported */
#define UVERBS_UDRV_OBJECT_OPTIONAL(_object_id)                                \
	bool UVERBS_UDRV_SUPPORTED(_object_id)(struct ib_device *ib_dev);

#define DECLARE_ENUM(_enum_arr, ...)
#define END_UVERBS_OBJECT(_object_id)
#define ADD_UDRV_ATTRIBUTES(_method_id, ...)
#define END_UDRV_OBJECT(_object_id)
#define START_UDRV_OBJECT(_object_id)

#define UVERBS_HDR_MODE 1
#include UVERBS_HDR

/* Clear out all the spec macros before exiting */
#define IB_USER_IOCTL_DECLS_H
#include <uapi/rdma/ib_user_ioctl_decls.h>

#undef UVERBS_HDR
