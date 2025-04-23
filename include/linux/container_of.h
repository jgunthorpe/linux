/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CONTAINER_OF_H
#define _LINUX_CONTAINER_OF_H

#include <linux/build_bug.h>
#include <linux/stddef.h>

#define typeof_member(T, m)	typeof(((T*)0)->m)

/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:	the pointer to the member.
 * @type:	the type of the container struct this is embedded in.
 * @member:	the name of the member within the struct.
 *
 * WARNING: any const qualifier of @ptr is lost.
 * Do not use container_of() in new code.
 */
#define container_of(ptr, type, member) ({				\
	void *__mptr = (void *)(ptr);					\
	static_assert(__same_type(*(ptr), ((type *)0)->member) ||	\
		      __same_type(*(ptr), void),			\
		      "pointer type mismatch in container_of()");	\
	((type *)(__mptr - offsetof(type, member))); })

/**
 * container_of_const - cast a member of a structure out to the containing
 *			structure and preserve the const-ness of the pointer
 * @ptr:		the pointer to the member
 * @type:		the type of the container struct this is embedded in.
 * @member:		the name of the member within the struct.
 *
 * Always prefer container_of_const() instead of container_of() in new code.
 */
#define container_of_const(ptr, type, member)				\
	_Generic(ptr,							\
		const typeof(*(ptr)) *: ((const type *)container_of(ptr, type, member)),\
		default: ((type *)container_of(ptr, type, member))	\
	)

/**
 * alloc_container_sz - allocate a container struct that emebeds another struct
 * @container_type: the type of the enclosing structure
 * @container_sz: size to allocate
 * @member: the structure member within container_type for embedded_type
 * @alloc_fn: Allocation function to call
 *
 * This helper is useful allocate and initialize derived structs. Typically a
 * subsystem will provide an allocate and initialize function for embedded_type.
 * A user of the subsystem will provide the derived type container_type which
 * encloses embedded_type and uses container_of() to move between the types.
 *
 * In this pattern the subsystem provides a function to both allocate and
 * initialize embedded_type so that the subsystem normal destroy function can be
 * used. This avoids exposing a failure path for allocated but uninitialized
 * memory that complicates error unwind and prevents trivially using cleanup.h.
 *
 * To use this API the user is required to place embedded_type at the front of
 * container_type.
 *
 * alloc_fn() accepts the size to allocate as the first argument and the VA_ARGS
 * as the remainder. size is guaranteed by the macro to be at least
 * sizeof(embedded_type).
 *
 * Returns a pointer of type container_type. Returns the same failure code as
 * alloc_fn, either ERR_PTR or NULL.
 */
#define alloc_container_sz(container_type, container_sz, member, alloc_fn, \
			   ...)                                            \
	({                                                                 \
		typeof_member(container_type, member) *res =               \
			alloc_fn(container_sz, ##__VA_ARGS__);             \
		static_assert(offsetof(container_type, member) == 0);      \
		(container_type *)res;                                     \
	})

/**
 * alloc_container - allocate a container struct that emebeds another struct
 * @container_type: the type of the enclosing structure
 * @member: the structure member within container_type for embedded_type
 * @alloc_fn: Allocation function to call
 *
 * Helper to call alloc_container_sz() with sizeof(container_type).
 */
#define alloc_container(container_type, member, alloc_fn, ...)             \
	alloc_container_sz(container_type, sizeof(container_type), member, \
			   alloc_fn, ##__VA_ARGS__)

#endif	/* _LINUX_CONTAINER_OF_H */
