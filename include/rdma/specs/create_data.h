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

#include <rdma/uverbs_ioctl.h>

/* This header should not be included directly, instead it is included by a
 * _data.c file that defines UVERBS_HDR. It includes the specs header from
 * uapi/ with a different macro set to create the kernel rodata for the ioctl
 * parser/validator. The output is a:
 *   const struct uverbs_object_def UVERBS_TREE_NAME
 */

/* ----------------------------------------------------------------------
 * Create struct uverbs_method_def's containing all the attributes for
 * each declared method
 */

/* Use in the _type parameter for attribute specifications */
#define UVERBS_ATTR_TYPE(_type)					\
	.min_len = sizeof(_type), .len = sizeof(_type)
#define UVERBS_ATTR_STRUCT(_type, _last)			\
	.min_len = ((uintptr_t)(&((_type *)0)->_last + 1)), .len = sizeof(_type)
#define UVERBS_ATTR_SIZE(_min_len, _len)			\
	.min_len = _min_len, .len = _len

/* Use in the '...' of any UVERBS_ATTR */
#define UA_FLAGS(_flags) .flags = _flags

#define UVERBS_ATTR_IDR(_attr_id, _idr_type, _access, ...)                     \
	(&(const struct uverbs_attr_def){                                      \
		.id = _attr_id,                                                \
		.attr = {                                                      \
			.obj = { .type = UVERBS_ATTR_TYPE_IDR,                 \
				 .obj_type = _idr_type,                        \
				 .access = _access,                            \
				 __VA_ARGS__ },                                \
		} })
#define UVERBS_ATTR_FD(_attr_id, _fd_type, _access, ...)                       \
	(&(const struct uverbs_attr_def){                                      \
		.id = (_attr_id) +                                             \
		      BUILD_BUG_ON_ZERO((_access) != UVERBS_ACCESS_NEW &&      \
					(_access) != UVERBS_ACCESS_READ),      \
		.attr = {                                                      \
			.obj = { .type = UVERBS_ATTR_TYPE_FD,                  \
				 .obj_type = _fd_type,                         \
				 .access = _access,                            \
				 __VA_ARGS__ },                                \
		} })

#define UVERBS_ATTR_PTR_IN(_attr_id, _type, ...)                               \
	(&(const struct uverbs_attr_def){                                      \
		.id = _attr_id,                                                \
		.attr = {                                                      \
			.ptr = { .type = UVERBS_ATTR_TYPE_PTR_IN,              \
				 _type,                                        \
				 __VA_ARGS__ },                                \
		} })

#define UVERBS_ENUM_PTR_IN(_enum_id, _type, ...)                                \
	[_enum_id] = {                                                          \
		.ptr = { .type = UVERBS_ATTR_TYPE_PTR_IN, _type, __VA_ARGS__ }, \
	}

#define UVERBS_ATTR_PTR_OUT(_attr_id, _type, ...)                              \
	(&(const struct uverbs_attr_def){                                      \
		.id = _attr_id,                                                \
		.attr = {                                                      \
			.ptr = { .type = UVERBS_ATTR_TYPE_PTR_OUT,             \
				 _type,                                        \
				 __VA_ARGS__ },                                \
		} })

/* _enum_arry should be a 'static const union uverbs_attr_spec[]' */
#define UVERBS_ATTR_ENUM_IN(_attr_id, _enum_arr, ...)                          \
	(&(const struct uverbs_attr_def){                                      \
		.id = _attr_id,                                                \
		.attr = {                                                      \
			.enum_def = { .type = UVERBS_ATTR_TYPE_ENUM_IN,        \
				      .ids = _enum_arr,                        \
				      .num_elems = ARRAY_SIZE(_enum_arr),      \
				      __VA_ARGS__ },                           \
		} })

/* This spec is used in order to pass information to the hardware driver in a
 * legacy way. Every verb that could get driver specific data should get this
 * spec as the list element in the attributes list.
 */
#define UVERBS_ATTR_UHW()                                                      \
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_UHW_IN,                                 \
			   UVERBS_ATTR_SIZE(0, USHRT_MAX),		       \
			   UA_FLAGS(UVERBS_ATTR_SPEC_F_MIN_SZ_OR_ZERO)),       \
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_UHW_OUT,                               \
			    UVERBS_ATTR_SIZE(0, USHRT_MAX),		       \
			    UA_FLAGS(UVERBS_ATTR_SPEC_F_MIN_SZ_OR_ZERO))

#define UVERBS_ATTR_COPY(_src_attr_id, _new_mandatory) NULL

#define DECLARE_UVERBS_METHOD(_method_id, _id_num, ...)                        \
	static const struct uverbs_attr_def *const UVERBS_METHOD_ATTRS(        \
		_method_id)[] = { __VA_ARGS__ };                               \
	static const struct uverbs_method_def UVERBS_METHOD(_method_id) = {    \
		.id = _method_id,                                              \
		.handler = UVERBS_HANDLER(_method_id),                         \
		.num_attrs = ARRAY_SIZE(UVERBS_METHOD_ATTRS(_method_id)),      \
		.attrs = &UVERBS_METHOD_ATTRS(_method_id),                     \
	};

#if IS_ENABLED(CONFIG_INFINIBAND_EXP_LEGACY_VERBS_NEW_UAPI)
/* LEGACY methods are the same as normal methods, except they carry the UHW
 * and are controlled by a config.
 */
#define DECLARE_UVERBS_LEGACY_METHOD(_method_id, _id_num, ...)                 \
	static const struct uverbs_attr_def *const UVERBS_METHOD_ATTRS(        \
		_method_id)[] = { __VA_ARGS__, UVERBS_ATTR_UHW() };            \
	static const struct uverbs_method_def UVERBS_METHOD(_method_id) = {    \
		.id = _method_id,                                              \
		.handler = UVERBS_HANDLER(_method_id),                         \
		.num_attrs = ARRAY_SIZE(UVERBS_METHOD_ATTRS(_method_id)),      \
		.attrs = &UVERBS_METHOD_ATTRS(_method_id),                     \
	};
#else
#define DECLARE_UVERBS_LEGACY_METHOD(_method_id, _id_num, ...)
#endif

#define ADD_UDRV_ATTRIBUTES(_method_id, ...)                                   \
	static const struct uverbs_attr_def *const UVERBS_METHOD_ATTRS(        \
		_method_id)[] = { __VA_ARGS__ };                               \
	static const struct uverbs_method_def UVERBS_METHOD(_method_id) = {    \
		.id = _method_id,                                              \
		.num_attrs = ARRAY_SIZE(UVERBS_METHOD_ATTRS(_method_id)),      \
		.attrs = &UVERBS_METHOD_ATTRS(_method_id),                     \
	};

/* Create a standard destroy method using the default handler. The handle_attr
 * argument must be the attribute specifying the handle to destroy, the
 * default handler does not support any other attributes.
 */
int uverbs_destroy_def_handler(struct ib_device *ib_dev,
			       struct ib_uverbs_file *file,
			       struct uverbs_attr_bundle *attrs);
#define DECLARE_UVERBS_METHOD_DESTROY(_method_id, _id_num, _handle_attr)       \
	static const struct uverbs_attr_def *const UVERBS_METHOD_ATTRS(        \
		_method_id)[] = { _handle_attr };                              \
	static const struct uverbs_method_def UVERBS_METHOD(_method_id) = {    \
		.id = _method_id,                                              \
		.handler = uverbs_destroy_def_handler,                         \
		.num_attrs = ARRAY_SIZE(UVERBS_METHOD_ATTRS(_method_id)),      \
		.attrs = &UVERBS_METHOD_ATTRS(_method_id),                     \
	};

/* Define what ENUM values are allowed and their types. */
#define DECLARE_ENUM(_enum_arr, ...)                                           \
	static const union uverbs_attr_spec _enum_arr[] = { __VA_ARGS__ };

#define EMPTY_UVERBS_FD_OBJECT(_object_id, _id_num)
#define EMPTY_UVERBS_NULL_OBJECT(_object_id, _id_num)
#define EMPTY_UVERBS_OBJECT(_object_id, _id_num)
#define END_UDRV_OBJECT(_object_id)
#define END_UVERBS_OBJECT(_object_id)
#define START_UDRV_OBJECT(_object_id)
#define START_UVERBS_OBJECT(_object_id, _id_num)
#define UVERBS_UDRV_OBJECT_OPTIONAL(_object_id)

/* Include the user header with this macro set, then clear the macros */
#define UVERBS_HDR_MODE 1
#include UVERBS_HDR
#define IB_USER_IOCTL_DECLS_H
#include <uapi/rdma/ib_user_ioctl_decls.h>

/* ----------------------------------------------------------------------
 * Create struct uverbs_object_def's arrays containing all the declared
 * methods for each declared object.
 */
#define START_UVERBS_OBJECT(_object_id, _id_num)                               \
	static const struct uverbs_method_def *const UVERBS_OBJECT_METHODS(    \
		_object_id)[] = {

#define DECLARE_UVERBS_METHOD(_method_id, _id_num, ...)	\
	(&UVERBS_METHOD(_method_id)),
#if IS_ENABLED(CONFIG_INFINIBAND_EXP_LEGACY_VERBS_NEW_UAPI)
#define DECLARE_UVERBS_LEGACY_METHOD DECLARE_UVERBS_METHOD
#else
#define DECLARE_UVERBS_LEGACY_METHOD(_method_id, _id_num, ...)
#endif
#define DECLARE_UVERBS_METHOD_DESTROY DECLARE_UVERBS_METHOD

#define END_UVERBS_OBJECT(_object_id)                                          \
	};								       \

/* This pass also creates the uverbs_method_def array for UDRV objects*/
#define START_UDRV_OBJECT(_object_id)                                          \
	static const struct uverbs_method_def *const                           \
		UVERBS_UDRV_OBJECT_METHODS(_object_id)[] = {

#define ADD_UDRV_ATTRIBUTES(_method_id, _id_num, ...)                          \
	(&UVERBS_METHOD(_method_id)),

#define END_UDRV_OBJECT(_object_id)                                            \
	};								       \

#define DECLARE_ENUM(_enum_arr, ...)
#define EMPTY_UVERBS_FD_OBJECT(_object_id, _id_num)
#define EMPTY_UVERBS_NULL_OBJECT(_object_id, _id_num)
#define EMPTY_UVERBS_OBJECT(_object_id, _id_num)
#define UVERBS_UDRV_OBJECT_OPTIONAL(_object_id)

/* Include the user header with this macro set, then clear the macros */
#define UVERBS_HDR_MODE 1
#include UVERBS_HDR
#define IB_USER_IOCTL_DECLS_H
#include <uapi/rdma/ib_user_ioctl_decls.h>

/* ----------------------------------------------------------------------
 * Create struct uverbs_object_def's containing all the declared methods
 * and meta data for every object
 */
#define EMPTY_UVERBS_OBJECT(_object_id, _id_num)                               \
	const struct uverbs_object_def UVERBS_OBJECT(_object_id) = {           \
		.id = _object_id,                                              \
		.type_attrs = &UVERBS_OBJECT_TYPE(_object_id).type,            \
	};
#define EMPTY_UVERBS_FD_OBJECT(_object_id, _id_num)                            \
	const struct uverbs_object_def UVERBS_OBJECT(_object_id) = {           \
		.id = _object_id,                                              \
		.type_attrs = &UVERBS_OBJECT_TYPE(_object_id).type,            \
	};
#define EMPTY_UVERBS_NULL_OBJECT(_object_id, _id_num)                          \
	const struct uverbs_object_def UVERBS_OBJECT(_object_id) = {           \
		.id = _object_id,                                              \
	};

#define START_UVERBS_OBJECT(_object_id, _id_num)                               \
	const struct uverbs_object_def UVERBS_OBJECT(_object_id) = {           \
		.id = _object_id,                                              \
		.type_attrs = &UVERBS_OBJECT_TYPE(_object_id).type,            \
		.num_methods = ARRAY_SIZE(UVERBS_OBJECT_METHODS(_object_id)),  \
		.methods = &UVERBS_OBJECT_METHODS(_object_id),                 \
	};
#define END_UVERBS_OBJECT(_object_id)

#define START_UDRV_OBJECT(_object_id)                                          \
	const struct uverbs_object_def UVERBS_UDRV_OBJECT(_object_id) = {      \
		.id = _object_id,                                              \
		.num_methods =                                                 \
			ARRAY_SIZE(UVERBS_UDRV_OBJECT_METHODS(_object_id)),    \
		.methods = &UVERBS_UDRV_OBJECT_METHODS(_object_id),

#define UVERBS_UDRV_OBJECT_OPTIONAL(_object_id)                                \
	.object_supported = UVERBS_UDRV_SUPPORTED(_object_id),

#define END_UDRV_OBJECT(_object_id) \
	};

#define ADD_UDRV_ATTRIBUTES(_method_id, _id_num, ...)
#define DECLARE_ENUM(_enum_arr, ...)
#define DECLARE_UVERBS_LEGACY_METHOD DECLARE_UVERBS_METHOD
#define DECLARE_UVERBS_METHOD(_object_id, _id_num, ...)
#define DECLARE_UVERBS_METHOD_DESTROY DECLARE_UVERBS_METHOD

/* Include the user header with this macro set, then clear the macros */
#define UVERBS_HDR_MODE 1
#include UVERBS_HDR
#define IB_USER_IOCTL_DECLS_H
#include <uapi/rdma/ib_user_ioctl_decls.h>

/* ----------------------------------------------------------------------
 * Create a uverbs_object_tree containing all the objects
 */

#define EMPTY_UVERBS_OBJECT(_object_id, _id_num) (&UVERBS_OBJECT(_object_id)),
#define EMPTY_UVERBS_FD_OBJECT(_object_id, _id_num) (&UVERBS_OBJECT(_object_id)),
#define EMPTY_UVERBS_NULL_OBJECT(_object_id, _id_num) (&UVERBS_OBJECT(_object_id)),
#define START_UVERBS_OBJECT(_object_id, _id_num) (&UVERBS_OBJECT(_object_id)),
#define START_UDRV_OBJECT(_object_id) (&UVERBS_UDRV_OBJECT(_object_id)),

#define ADD_UDRV_ATTRIBUTES(_method_id, _id_num, ...)
#define DECLARE_ENUM(_enum_arr, ...)
#define DECLARE_UVERBS_LEGACY_METHOD DECLARE_UVERBS_METHOD
#define DECLARE_UVERBS_METHOD(_object_id, _id_num, ...)
#define DECLARE_UVERBS_METHOD_DESTROY DECLARE_UVERBS_METHOD
#define END_UDRV_OBJECT(_object_id)
#define END_UVERBS_OBJECT(_object_id)
#define UVERBS_UDRV_OBJECT_OPTIONAL(_object_id)

#define _UVERBS_OBJECTS(name) _UVERBS_PASTE(_uverbs_objects_, name)

static const struct uverbs_object_def *const
	_UVERBS_OBJECTS(UVERBS_TREE_NAME)[] = {
#define UVERBS_HDR_MODE 1
#include UVERBS_HDR
	};
static const struct uverbs_object_tree_def UVERBS_TREE_NAME = {
	.num_objects = ARRAY_SIZE(_UVERBS_OBJECTS(UVERBS_TREE_NAME)),
	.objects = &_UVERBS_OBJECTS(UVERBS_TREE_NAME),
};

/* Clear out all the spec macros before exiting */
#define IB_USER_IOCTL_DECLS_H
#include <uapi/rdma/ib_user_ioctl_decls.h>
