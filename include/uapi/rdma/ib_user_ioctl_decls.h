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

/* This header should only ever be included by ib_user_ioctl_cmds.h. This
 * macro set creates the enum values for the methods and method attributes
 * that user space expects to see.
 */
#ifndef IB_USER_IOCTL_DECLS_H
#define IB_USER_IOCTL_DECLS_H

/* Occasionally we need some globally unique names */
#ifdef UVERBS_DRIVER_NAME
#define __DUMMY3(unique, drv, ln) _dummy##ln##_##drv##_##unique
#define _DUMMY3(unique, drv, ln) __DUMMY3(unique, drv, ln)
#define _DUMMY(unique) _DUMMY3(unique, UVERBS_DRIVER_NAME, __LINE__)
#else
#define __DUMMY2(unique, ln) _dummy##ln##_##unique
#define _DUMMY2(unique, ln) __DUMMY2(unique, ln)
#define _DUMMY(unique) _DUMMY2(unique, __LINE__)
#endif

/* A basic method for an object. The ... is a list of UVERBS_ATTR_* */
#define DECLARE_UVERBS_METHOD(_method_id, _id_num, ...)                        \
	enum { _method_id = _id_num };                                         \
	enum { __VA_ARGS__ };

/* Special versions of DECLARE_UVERBS_METHOD. DESTROY indicates this is a
 * simple object destroy using the default destroy handler and a single
 * attribute.  LEGACY indicates this is part of the legacy compat API and is
 * not enabled unless CONFIG_INFINIBAND_EXP_LEGACY_VERBS_NEW_UAPI
 */
#define DECLARE_UVERBS_METHOD_DESTROY DECLARE_UVERBS_METHOD
#define DECLARE_UVERBS_LEGACY_METHOD DECLARE_UVERBS_METHOD

/* Create an enumeration for use with UVERBS_ATTR_ENUM_IN */
#define DECLARE_ENUM(_enum_arr, ...)

/* Define attributes that extend an existing core method */
#define ADD_UDRV_ATTRIBUTES(_method_id, ...)                                   \
	enum { _DUMMY(_method_id) = (1U << UVERBS_ID_NS_SHIFT) - 1,            \
	       __VA_ARGS__ };

#define START_UVERBS_OBJECT(_object_id, _id_num) enum { _object_id = _id_num };
#define END_UVERBS_OBJECT(_object_id)
#define EMPTY_UVERBS_OBJECT START_UVERBS_OBJECT
#define EMPTY_UVERBS_FD_OBJECT START_UVERBS_OBJECT
#define EMPTY_UVERBS_NULL_OBJECT START_UVERBS_OBJECT

#define UVERBS_ATTR_TYPE(_type)
#define UVERBS_ATTR_STRUCT(_type, _last)
#define UVERBS_ATTR_SIZE(_min_len, _len)
#define UA_FLAGS(_flags)

#define UVERBS_ATTR_IDR(_attr_id, _idr_type, _access, ...) _attr_id
#define UVERBS_ATTR_FD(_attr_id, _fd_type, _access, ...) _attr_id
#define UVERBS_ATTR_PTR_IN(_attr_id, _type, ...) _attr_id
#define UVERBS_ATTR_PTR_OUT(_attr_id, _type, ...) _attr_id
#define UVERBS_ATTR_ENUM_IN(_attr_id, _enum_arr, ...) _attr_id

#define UVERBS_ENUM_PTR_IN(_enum_id, _type, ...)

#define UVERBS_UDRV_OBJECT_OPTIONAL(_object_id)
#define START_UDRV_OBJECT(_object_id)
#define END_UDRV_OBJECT(_object_id)

/* UHW is alway last so it doesn't need the _dummy */
#define UVERBS_ATTR_UHW()
#define UVERBS_ATTR_COPY(_src_attr_id, _new_mandatory) _DUMMY(_src_attr_id)

#else

/* Exiting the uapi header, drop the macros */

#undef DECLARE_UVERBS_METHOD
#undef DECLARE_UVERBS_METHOD_DESTROY
#undef DECLARE_UVERBS_LEGACY_METHOD
#undef DECLARE_ENUM

#undef UVERBS_UDRV_OBJECT_OPTIONAL

#undef ADD_UDRV_ATTRIBUTES

#undef START_UVERBS_OBJECT
#undef END_UVERBS_OBJECT

#undef START_UDRV_OBJECT
#undef END_UDRV_OBJECT

#undef EMPTY_UVERBS_OBJECT
#undef EMPTY_UVERBS_FD_OBJECT
#undef EMPTY_UVERBS_NULL_OBJECT

#undef UVERBS_ATTR_TYPE
#undef UVERBS_ATTR_STRUCT
#undef UVERBS_ATTR_SIZE
#undef UA_FLAGS

#undef UVERBS_ATTR_IDR
#undef UVERBS_ATTR_FD
#undef UVERBS_ATTR_PTR_IN
#undef UVERBS_ATTR_PTR_OUT
#undef UVERBS_ATTR_ENUM_IN
#undef UVERBS_ATTR_UHW
#undef UVERBS_ATTR_COPY

#undef UVERBS_ENUM_PTR_IN

#undef IB_USER_IOCTL_DECLS_H

#undef __DUMMY3
#undef _DUMMY3
#undef __DUMMY2
#undef _DUMMY2
#undef _DUMMY

#undef UVERBS_HDR_MODE

#endif
