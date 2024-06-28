/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 *
 * Iterators for Generic Page Table
 */
#ifndef __GENERIC_PT_PT_ITER_H
#define __GENERIC_PT_PT_ITER_H

#include "pt_common.h"

#include <linux/errno.h>

/*
 * Use to mangle symbols so that backtraces and the symbol table are
 * understandable. Any non-inlined function should get mangled like this.
 */
#define NS(fn) CONCATENATE(PTPFX, fn)

/*
 * With range->va being the start, set range->last_va and validate that the
 * range is within the allowed
 */
static inline int pt_check_range(struct pt_range *range)
{
	pt_vaddr_t prefix = pt_full_va_prefix(range->common);

	PT_WARN_ON(!range->max_vasz_lg2);

	if (!fvalog2_div_eq(range->va, prefix, range->max_vasz_lg2) ||
	    !fvalog2_div_eq(range->last_va, prefix, range->max_vasz_lg2))
		return -ERANGE;
	return 0;
}

/*
 * Adjust the va to match the current index.
 */
static inline void pt_index_to_va(struct pt_state *pts)
{
	unsigned int table_lg2sz = pt_table_oa_lg2sz(pts);
	pt_vaddr_t lower_va;

	lower_va = log2_mul(pts->index, pt_table_item_lg2sz(pts));
	pts->range->va = fvalog2_set_mod(pts->range->va, lower_va, table_lg2sz);
}

/*
 * Add index_count_lg2 number of entries to pts's VA and index. The va will be
 * adjusted to the end of the contiguous block if it is currently in the middle.
 */
static inline void _pt_advance(struct pt_state *pts,
			       unsigned int index_count_lg2)
{
	pts->index = log2_set_mod(pts->index + log2_to_int(index_count_lg2), 0,
				  index_count_lg2);
	pt_index_to_va(pts);
}

/* True if the current entry is fully enclosed by the range of va to last_va. */
static inline bool pt_entry_fully_covered(const struct pt_state *pts,
					  unsigned int oasz_lg2)
{
	struct pt_range *range = pts->range;

	/* Range begins at the start of the entry */
	if (log2_mod(pts->range->va, oasz_lg2))
		return false;

	/* Range ends past the end of the entry */
	if (!log2_div_eq(range->va, range->last_va, oasz_lg2))
		return true;

	/* Range ends at the end of the entry */
	return log2_mod_eq_max(range->last_va, oasz_lg2);
}

static inline unsigned int pt_range_to_index(struct pt_state *pts)
{
	unsigned int num_entries_lg2 = pt_num_items_lg2(pts);
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);

	if (pts->range->top_level == pts->level)
		return log2_div(fvalog2_mod(pts->range->va,
					    pts->range->max_vasz_lg2),
				isz_lg2);
	return log2_mod(log2_div(pts->range->va, isz_lg2), num_entries_lg2);
}

static inline unsigned int pt_range_to_last_index(struct pt_state *pts)
{
	unsigned int num_entries_lg2 = pt_num_items_lg2(pts);
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);

	if (pts->range->top_level == pts->level)
		return log2_div(fvalog2_mod(pts->range->last_va,
					    pts->range->max_vasz_lg2),
				isz_lg2);
	return log2_mod(log2_div(pts->range->last_va, isz_lg2),
			num_entries_lg2);
}

static inline void _pt_iter_first(struct pt_state *pts)
{
	unsigned int num_entries_lg2 = pt_num_items_lg2(pts);
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);
	struct pt_range *range = pts->range;

	pts->index = pt_range_to_index(pts);
	if (range->va == range->last_va) {
		pts->end_index = pts->index + 1;
		return;
	}

	/* last_va falls within this table */
	if (pts->range->top_level == pts->level ||
	    log2_div_eq(range->va, range->last_va, num_entries_lg2 + isz_lg2))
		pts->end_index = pt_range_to_last_index(pts) + 1;
	else
		pts->end_index = log2_to_int(num_entries_lg2);

	PT_WARN_ON(pts->index > pts->end_index);
}

static inline bool _pt_iter_load(struct pt_state *pts)
{
	if (pts->index == pts->end_index)
		return false;
	pt_load_entry(pts);
	return true;
}

/* Update pts to go to the next index at this level */
static inline void pt_next_entry(struct pt_state *pts)
{
	if (pts->type == PT_ENTRY_OA)
		_pt_advance(pts, pt_entry_num_contig_lg2(pts));
	else
		_pt_advance(pts, ilog2(1));
}

#define for_each_pt_level_item(pts) \
	for (_pt_iter_first(pts); _pt_iter_load(pts); pt_next_entry(pts))

/* Version of pt_load_entry() usable within a walker */
static inline enum pt_entry_type pt_load_single_entry(struct pt_state *pts)
{
	pts->index = pt_range_to_index(pts);
	pt_load_entry(pts);
	return pts->type;
}

static __always_inline struct pt_range _pt_top_range(struct pt_common *common,
						     uintptr_t top_of_table)
{
	struct pt_range range = {
		.common = common,
		.top_table =
			(struct pt_table_p *)(top_of_table &
					      ~(uintptr_t)PT_TOP_LEVEL_MASK),
#ifdef PT_FIXED_TOP_LEVEL
		.top_level = PT_FIXED_TOP_LEVEL,
#else
		.top_level = top_of_table % (1 << PT_TOP_LEVEL_BITS),
#endif
	};
	struct pt_state pts = { .range = &range, .level = range.top_level };

	range.max_vasz_lg2 =
		min_t(unsigned int, common->max_vasz_lg2,
		      pt_num_items_lg2(&pts) + pt_table_item_lg2sz(&pts));
	range.va = fvalog2_set_mod(pt_full_va_prefix(common), 0,
				   range.max_vasz_lg2);
	range.last_va = fvalog2_set_mod_max(pt_full_va_prefix(common),
					    range.max_vasz_lg2);
	return range;
}

/* Span the whole table */
static __always_inline struct pt_range pt_top_range(struct pt_common *common)
{
	/*
	 * The top pointer can change without locking. We capture the value and
	 * it's level here and are safe to walk it so long as both values are
	 * captured without tearing.
	 */
	return _pt_top_range(common, READ_ONCE(common->top_of_table));
}

/* Span a slice of the table starting at the top */
static __always_inline struct pt_range
pt_make_range(struct pt_common *common, pt_vaddr_t va, pt_vaddr_t last_va)
{
	struct pt_range range =
		_pt_top_range(common, READ_ONCE(common->top_of_table));

	range.va = va;
	range.last_va = last_va;
	return range;
}

/*
 * Span a slice of the table starting at a lower table level from an active
 * walk.
 */
static __always_inline struct pt_range
pt_make_child_range(const struct pt_range *parent, pt_vaddr_t va,
		    pt_vaddr_t last_va)
{
	struct pt_range range = *parent;

	range.va = va;
	range.last_va = last_va;

	PT_WARN_ON(last_va < va);
	PT_WARN_ON(pt_check_range(&range));

	return range;
}

static __always_inline struct pt_state
pt_init(struct pt_range *range, unsigned int level, struct pt_table_p *table)
{
	struct pt_state pts = {
		.range = range,
		.table = table,
		.level = level,
	};
	return pts;
}

static __always_inline struct pt_state pt_init_top(struct pt_range *range)
{
	return pt_init(range, range->top_level, range->top_table);
}

typedef int (*pt_level_fn_t)(struct pt_range *range, void *arg,
			     unsigned int level, struct pt_table_p *table);

static __always_inline int pt_descend(struct pt_state *pts, void *arg,
				      pt_level_fn_t fn)
{
	int ret;

	if (PT_WARN_ON(!pts->table_lower))
		return -EINVAL;

	ret = (*fn)(pts->range, arg, pts->level - 1, pts->table_lower);
	return ret;
}

/*
 * Walk over an IOVA range. The caller should have done a validity check, at
 * least calling pt_check_range(), when building range.
 */
static __always_inline int pt_walk_range(struct pt_range *range,
					 pt_level_fn_t fn, void *arg)
{
	return fn(range, arg, range->top_level, range->top_table);
}

/*
 * With parent_pts pointing at a table this will prepare to walk over a slice of
 * the child table of the current entry.
 */
static __always_inline int
pt_walk_child_range(const struct pt_state *parent_pts, pt_vaddr_t va,
		    pt_vaddr_t last_va, pt_level_fn_t fn, void *arg)
{
	struct pt_range range =
		pt_make_child_range(parent_pts->range, va, last_va);

	if (PT_WARN_ON(!pt_can_have_table(parent_pts)) ||
	    PT_WARN_ON(!parent_pts->table_lower))
		return -EINVAL;

	return fn(&range, arg, parent_pts->level - 1, parent_pts->table_lower);
}

/*
 * With parent_pts pointing at a table this will prepare to walk over the entire
 * the child table
 */
static __always_inline int pt_walk_child_all(const struct pt_state *parent_pts,
					     pt_level_fn_t fn, void *arg)
{
	unsigned int isz_lg2 = pt_table_item_lg2sz(parent_pts);

	return pt_walk_child_range(
		parent_pts, log2_set_mod(parent_pts->range->va, 0, isz_lg2),
		log2_set_mod_max(parent_pts->range->va, isz_lg2), fn, arg);
}

/* Create a range than spans an index range of the current pt_state */
static inline struct pt_range pt_range_slice(const struct pt_state *pts,
					     unsigned int start_index,
					     unsigned int end_index)
{
	unsigned int table_lg2sz = pt_table_oa_lg2sz(pts);
	pt_vaddr_t last_va;
	pt_vaddr_t va;

	va = fvalog2_set_mod(pts->range->va,
			     log2_mul(start_index, pt_table_item_lg2sz(pts)),
			     table_lg2sz);
	last_va = fvalog2_set_mod(
		pts->range->va,
		log2_mul(end_index, pt_table_item_lg2sz(pts)) - 1, table_lg2sz);
	return pt_make_child_range(pts->range, va, last_va);
}

/*
 * Compute the size of the top table. For PT_FEAT_DYNAMIC_TOP this will compute
 * the top size assuming the table will grow.
 */
static inline unsigned int pt_top_memsize_lg2(struct pt_common *common,
					      uintptr_t top_of_table)
{
	struct pt_range range = _pt_top_range(common, top_of_table);
	struct pt_state pts = pt_init_top(&range);
	unsigned int num_items_lg2;

	num_items_lg2 = common->max_vasz_lg2 - pt_table_item_lg2sz(&pts);
	if (range.top_level != PT_MAX_TOP_LEVEL &&
	    pt_feature(common, PT_FEAT_DYNAMIC_TOP))
		num_items_lg2 = min(num_items_lg2, pt_num_items_lg2(&pts));

	return num_items_lg2 + ilog2(PT_ENTRY_WORD_SIZE);
}

static inline unsigned int __pt_compute_best_pgsize(pt_vaddr_t pgsz_bitmap,
						    pt_vaddr_t va,
						    pt_vaddr_t last_va,
						    pt_oaddr_t oa)
{
	unsigned int best_pgsz_lg2;
	unsigned int pgsz_lg2;
	pt_vaddr_t len = last_va - va + 1;
	pt_vaddr_t mask;

	if (PT_WARN_ON(va >= last_va))
		return 0;

	/*
	 * Given a VA/OA pair the best page size is the largest page side
	 * where:
	 *
	 * 1) VA and OA start at the page. Bitwise this is the count of least
	 *    significant 0 bits.
	 *    This also implies that last_va/oa has the same prefix as va/oa.
	 */
	mask = va | oa;

	/*
	 * 2) The page size is not larger than the last_va (length). Since page
	 *    sizes are always power of two this can't be larger than the
	 *    largest power of two factor of the length.
	 */
	mask |= log2_to_int(log2_fls(len) - 1);

	best_pgsz_lg2 = log2_ffs(mask);

	/* Choose the higest bit <= best_pgsz_lg2 */
	if (best_pgsz_lg2 < PT_VADDR_MAX_LG2 - 1)
		pgsz_bitmap = log2_mod(pgsz_bitmap, best_pgsz_lg2 + 1);

	pgsz_lg2 = log2_fls(pgsz_bitmap);
	if (!pgsz_lg2)
		return 0;

	pgsz_lg2--;

	PT_WARN_ON(log2_mod(va, pgsz_lg2) != 0);
	PT_WARN_ON(oalog2_mod(oa, pgsz_lg2) != 0);
	PT_WARN_ON(va + log2_to_int(pgsz_lg2) - 1 > last_va);
	PT_WARN_ON(!log2_div_eq(va, va + log2_to_int(pgsz_lg2) - 1, pgsz_lg2));
	PT_WARN_ON(
		!oalog2_div_eq(oa, oa + log2_to_int(pgsz_lg2) - 1, pgsz_lg2));
	return pgsz_lg2;
}

/*
 * Compute the largest page size for va, last_va, and pa together and return it
 * in lg2.
 */
static inline unsigned int pt_compute_best_pgsize(struct pt_state *pts,
						  pt_oaddr_t oa)
{
	return __pt_compute_best_pgsize(pt_possible_sizes(pts), pts->range->va,
					pts->range->last_va, oa);
}

#define _PT_MAKE_CALL_LEVEL2(fn)                                             \
	static __always_inline int fn(struct pt_range *range, void *arg,     \
				      unsigned int level,                    \
				      struct pt_table_p *table)              \
	{                                                                    \
		switch (level) {                                             \
		case 0:                                                      \
			return CONCATENATE(fn, 0)(range, arg, level, table); \
		case 1:                                                      \
			if (1 > PT_MAX_TOP_LEVEL)                            \
				break;                                       \
			return CONCATENATE(fn, 1)(range, arg, level, table); \
		case 2:                                                      \
			if (2 > PT_MAX_TOP_LEVEL)                            \
				break;                                       \
			return CONCATENATE(fn, 2)(range, arg, level, table); \
		case 3:                                                      \
			if (3 > PT_MAX_TOP_LEVEL)                            \
				break;                                       \
			return CONCATENATE(fn, 3)(range, arg, level, table); \
		case 4:                                                      \
			if (4 > PT_MAX_TOP_LEVEL)                            \
				break;                                       \
			return CONCATENATE(fn, 4)(range, arg, level, table); \
		default:                                                     \
			break;                                               \
		}                                                            \
		return -EPROTOTYPE;                                          \
	}

#define _PT_MAKE_CALL_LEVEL(fn)                                          \
	static __always_inline int fn(struct pt_range *range, void *arg, \
				      unsigned int level,                \
				      struct pt_table_p *table)          \
	{                                                                \
		static_assert(PT_MAX_TOP_LEVEL <= 5);                    \
		if (level == 0)                                          \
			return CONCATENATE(fn, 0)(range, arg, 0, table); \
		if (level == 1 || PT_MAX_TOP_LEVEL == 1)                 \
			return CONCATENATE(fn, 1)(range, arg, 1, table); \
		if (level == 2 || PT_MAX_TOP_LEVEL == 2)                 \
			return CONCATENATE(fn, 2)(range, arg, 2, table); \
		if (level == 3 || PT_MAX_TOP_LEVEL == 3)                 \
			return CONCATENATE(fn, 3)(range, arg, 3, table); \
		if (level == 4 || PT_MAX_TOP_LEVEL == 4)                 \
			return CONCATENATE(fn, 4)(range, arg, 4, table); \
		return CONCATENATE(fn, 5)(range, arg, 5, table);         \
	}

static inline int __pt_make_level_fn_err(struct pt_range *range, void *arg,
					 unsigned int unused_level,
					 struct pt_table_p *table)
{
	static_assert(PT_MAX_TOP_LEVEL <= 5);
	return -EPROTOTYPE;
}

#define __PT_MAKE_LEVEL_FN(fn, level, descend_fn, do_fn)            \
	static inline int fn(struct pt_range *range, void *arg,     \
			     unsigned int unused_level,             \
			     struct pt_table_p *table)              \
	{                                                           \
		return do_fn(range, arg, level, table, descend_fn); \
	}

/*
 * This builds a function call tree that can be fully inlined,
 * The caller must provide a function body in an __always_inline function:
 * static __always_inline int do(struct pt_range *range, void *arg,
 *         unsigned int level, struct pt_table_p *table,
 *         pt_level_fn_t descend_fn)
 *
 * An inline function will be created for each table level that calls do with a
 * compile time constant for level and a pointer to the next lower function.
 * This generates an optimally inlined walk where each of the functions sees a
 * constant level and can codegen the exact constants/etc for that level.
 *
 * Note this can produce a lot of code!
 */
#define PT_MAKE_LEVELS(fn, do_fn)                                             \
	__PT_MAKE_LEVEL_FN(CONCATENATE(fn, 0), 0, __pt_make_level_fn_err,     \
			   do_fn);                                            \
	__PT_MAKE_LEVEL_FN(CONCATENATE(fn, 1), 1, CONCATENATE(fn, 0), do_fn); \
	__PT_MAKE_LEVEL_FN(CONCATENATE(fn, 2), 2, CONCATENATE(fn, 1), do_fn); \
	__PT_MAKE_LEVEL_FN(CONCATENATE(fn, 3), 3, CONCATENATE(fn, 2), do_fn); \
	__PT_MAKE_LEVEL_FN(CONCATENATE(fn, 4), 4, CONCATENATE(fn, 3), do_fn); \
	__PT_MAKE_LEVEL_FN(CONCATENATE(fn, 5), 5, CONCATENATE(fn, 4), do_fn); \
	_PT_MAKE_CALL_LEVEL(fn)

#endif
