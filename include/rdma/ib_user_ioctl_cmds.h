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
#ifndef KERNEL_IB_USER_IOCTL_CMDS_H
#define KERNEL_IB_USER_IOCTL_CMDS_H

#define __UVERBS_PASTE(x, y) x##y
#define _UVERBS_PASTE(x, y) __UVERBS_PASTE(x, y)

/* These are the 'top level' naming macros. They create names that are
 * globally unique and can only be used to implement the first level of the
 * specs (eg DECLARE_*). Any extensions to existing objects (eg driver
 * add-ons) must not use these macros, however drivers can use them to create
 * their own driver 'top level' specs.
 */

/* The name of the C function that implements a method's code */
#define UVERBS_HANDLER(method_id) _uverbs_handler_##method_id
/* The name of the C function that implements an objects free function */
#define UVERBS_FREE_HANDLER(object_id) _uverbs_free_handler_##object_id
/* The name of the struct uverbs_attr_def array for the method */
#define UVERBS_METHOD_ATTRS(method_id) _uverbs_attrs_##method_id
/* The name of the struct uverbs_method_def for the method */
#define UVERBS_METHOD(method_id) _uverbs_method_def_##method_id
/* The name of the struct uverbs_method_def array for the object */
#define UVERBS_OBJECT_METHODS(object_id) _uverbs_methods_##object_id
/* The name of the struct uverbs_obj_*_type for the object */
#define UVERBS_OBJECT_TYPE(object_id) _uverbs_type_##object_id
/* The name of the struct file_operations for a FD object */
#define UVERBS_FD_FOPS(object_id) _uverbs_fops_##object_id
/* The name of the struct uverbs_object_def for the object */
#define UVERBS_OBJECT(object_id) _uverbs_object_def_##object_id

/* UDRV names exist only after including the driver's _user_ioctl_cmds.h and
 * are setup so that only one driver cmds.h can be included in a compilation
 * unit.
 */
#define _UDRV_NAME(_id, _name)                                                 \
	_UVERBS_PASTE(_UVERBS_PASTE(_name##_, UVERBS_MODULE_NAME), _##_id)

/* The name of the C function that implements UVERBS_UDRV_OBJECT_OPTIONAL */
#define UVERBS_UDRV_SUPPORTED(_id) _UDRV_NAME(_id, _udrv_supported)
/* The name of the struct uverbs_method_def array for the object */
#define UVERBS_UDRV_OBJECT_METHODS(object_id) _UDRV_NAME(object_id, _udrv_methods)
/* The name of the struct uverbs_object_def for the driver's ADD object */
#define UVERBS_UDRV_OBJECT(object_id) _UDRV_NAME(object_id, _udrv_object_def)

#include <uapi/rdma/ib_user_ioctl_cmds.h>
#define UVERBS_HDR <uapi/rdma/ib_user_ioctl_cmds.h>
#include <rdma/specs/create_prototypes.h>

#endif
