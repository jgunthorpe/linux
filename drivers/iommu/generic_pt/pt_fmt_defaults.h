/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 *
 * Default definitions for formats that don't define these functions.
 */
#ifndef __GENERIC_PT_PT_FMT_DEFAULTS_H
#define __GENERIC_PT_PT_FMT_DEFAULTS_H

#include "pt_defs.h"
#include <linux/log2.h>

/* Header self-compile default defines */
#ifndef pt_load_entry_raw
#include "fmt/amdv1.h"
#endif

/* If not supplied by the format then contiguous pages are not supported */
#ifndef pt_entry_num_contig_lg2
static inline unsigned int pt_entry_num_contig_lg2(const struct pt_state *pts)
{
	return ilog2(1);
}

static inline unsigned short pt_contig_count_lg2(const struct pt_state *pts)
{
	return ilog2(1);
}
#endif

/* If not supplied by the format then dirty tracking is not supported */
#ifndef pt_entry_write_is_dirty
static inline bool pt_entry_write_is_dirty(const struct pt_state *pts)
{
	return false;
}

static inline void pt_entry_set_write_clean(struct pt_state *pts)
{
}
#endif
#ifndef pt_entry_make_write_dirty
static inline bool pt_entry_make_write_dirty(struct pt_state *pts)
{
	return false;
}
#endif

/*
 * Format supplies either:
 *   pt_entry_oa - OA is at the start of a contiguous entry
 * or
 *   pt_item_oa  - OA is correct for every item in a contiguous entry
 *
 * Build the missing one
 */
#ifdef pt_entry_oa
static inline pt_oaddr_t pt_item_oa(const struct pt_state *pts)
{
	return pt_entry_oa(pts) |
	       log2_mul(pts->index, pt_table_item_lg2sz(pts));
}
#define _pt_entry_oa_fast pt_entry_oa
#endif

#ifdef pt_item_oa
static inline pt_oaddr_t pt_entry_oa(const struct pt_state *pts)
{
	return log2_set_mod(pt_item_oa(pts), 0,
			    pt_entry_num_contig_lg2(pts) +
				    pt_table_item_lg2sz(pts));
}
#define _pt_entry_oa_fast pt_item_oa
#endif

/*
 * If not supplied by the format then use the constant
 * PT_MAX_OUTPUT_ADDRESS_LG2.
 */
#ifndef pt_max_output_address_lg2
static inline unsigned int
pt_max_output_address_lg2(const struct pt_common *common)
{
	return PT_MAX_OUTPUT_ADDRESS_LG2;
}
#endif

/*
 * If not supplied by the format then assume only one contiguous size determined
 * by pt_contig_count_lg2()
 */
#ifndef pt_possible_sizes
static inline unsigned short pt_contig_count_lg2(const struct pt_state *pts);

/* Return a bitmap of possible leaf page sizes at this level */
static inline pt_vaddr_t pt_possible_sizes(const struct pt_state *pts)
{
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);

	if (!pt_can_have_leaf(pts))
		return 0;
	return log2_to_int(isz_lg2) |
	       log2_to_int(pt_contig_count_lg2(pts) + isz_lg2);
}
#endif

/* If not supplied by the format then use 0. */
#ifndef pt_full_va_prefix
static inline pt_vaddr_t pt_full_va_prefix(const struct pt_common *common)
{
	return 0;
}
#endif

/* If not supplied by the format then zero fill using PT_ENTRY_WORD_SIZE */
#ifndef pt_clear_entry
static inline void pt_clear_entry64(struct pt_state *pts,
				    unsigned int num_contig_lg2)
{
	u64 *tablep = pt_cur_table(pts, u64) + pts->index;
	u64 *end = tablep + log2_to_int(num_contig_lg2);

	PT_WARN_ON(log2_mod(pts->index, num_contig_lg2));
	for (; tablep != end; tablep++)
		WRITE_ONCE(*tablep, 0);
}

static inline void pt_clear_entry32(struct pt_state *pts,
				    unsigned int num_contig_lg2)
{
	u32 *tablep = pt_cur_table(pts, u32) + pts->index;
	u32 *end = tablep + log2_to_int(num_contig_lg2);

	PT_WARN_ON(log2_mod(pts->index, num_contig_lg2));
	for (; tablep != end; tablep++)
		WRITE_ONCE(*tablep, 0);
}

static inline void pt_clear_entry(struct pt_state *pts,
				  unsigned int num_contig_lg2)
{
	if (PT_ENTRY_WORD_SIZE == sizeof(u32))
		pt_clear_entry32(pts, num_contig_lg2);
	else
		pt_clear_entry64(pts, num_contig_lg2);
}
#define pt_clear_entry pt_clear_entry
#endif

#endif
