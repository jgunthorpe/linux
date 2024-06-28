/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 *
 * This header is included after the format. It contains definitions
 * that build on the format definitions to create the basic format API.
 *
 * The format API is listed here, with kdocs, in alphabetical order. The
 * functions without bodies are implemented in the format using the pattern:
 *     static inline FMTpt_XXX(..) {..}
 *     #define pt_XXX FMTpt_XXX
 *
 * The routines marked "@pts: Entry to query" operate on the entire contiguous
 * entry and can be called with a pts->index pointing to any sub item that makes
 * up that entry.
 *
 * The header order is:
 *  pt_defs.h
 *  fmt_XX.h
 *  pt_common.h
 */
#ifndef __GENERIC_PT_PT_COMMON_H
#define __GENERIC_PT_PT_COMMON_H

#include "pt_defs.h"
#include "pt_fmt_defaults.h"

/**
 * pt_attr_from_entry() - Convert the permission bits back to attrs
 * @pts: Entry to convert from
 * @attrs: Resulting attrs
 *
 * Fill in the attrs with the permission bits encoded in the current leaf entry.
 * The attrs should be usable with pt_install_leaf_entry() to reconstruct the
 * same entry.
 */
static inline void pt_attr_from_entry(const struct pt_state *pts,
				      struct pt_write_attrs *attrs);

/**
 * pt_can_have_leaf() - True if the current level can have an OA entry
 * @pts: The current level
 *
 * True if the current level can support pt_install_leaf_entry(). A leaf
 * entry produce an OA.
 */
static inline bool pt_can_have_leaf(const struct pt_state *pts);

/**
 * pt_can_have_table() - True if the current level can have a lower table
 * @pts: The current level
 *
 * Every level except 0 is allowed to have a lower table.
 */
static inline bool pt_can_have_table(const struct pt_state *pts)
{
	/* No further tables at level 0 */
	return pts->level > 0;
}

/**
 * pt_clear_entry() - Make entries empty (non-present)
 * @pts: Starting table index
 * @num_contig_lg2: Number of contiguous items to clear
 *
 * Clear a run of entries. A cleared entry will load back as PT_ENTRY_EMPTY
 * and does not have any effect on table walking. The starting index must be
 * aligned to num_contig_lg2.
 */
static inline void pt_clear_entry(struct pt_state *pts,
				  unsigned int num_contig_lg2);

/**
 * pt_entry_make_write_dirty() - Make an entry dirty
 * @pts: Table index to change
 *
 * Make pt_entry_write_is_dirty() return true for this entry. This can be called
 * asynchronously with any other table manipulation under a RCU lock and must
 * not corrupt the table.
 */
static inline bool pt_entry_make_write_dirty(struct pt_state *pts);

/**
 * pt_entry_num_contig_lg2() - Number of contiguous items for this leaf entry
 * @pts: Entry to query
 *
 * Returns the number of contiguous items this leaf entry spans. If the entry is
 * single item it returns ilog2(1).
 */
static inline unsigned int pt_entry_num_contig_lg2(const struct pt_state *pts);

/**
 * pt_entry_oa() - Output Address for this leaf entry
 * @pts: Entry to query
 *
 * Return the output address for the start of the entry. If the entry
 * is contigous this returns the same value for each sub-item. Ie:
 *    log2_mod(pt_entry_oa(), pt_entry_oa_lg2sz()) == 0
 *
 * See pt_item_oa(). The format should implement one of these two functions
 * depending on how it stores the OA's in the table.
 */
static inline pt_oaddr_t pt_entry_oa(const struct pt_state *pts);

/**
 * pt_entry_oa_lg2sz() - Return the size of a OA entry
 * @pts: Entry to query
 *
 * If the entry is not contigous this returns pt_table_item_lg2sz(), otherwise
 * it returns the total VA/OA size of the entire contiguous entry.
 */
static inline unsigned int pt_entry_oa_lg2sz(const struct pt_state *pts)
{
	return pt_entry_num_contig_lg2(pts) + pt_table_item_lg2sz(pts);
}

/**
 * pt_entry_oa_full() - Return the full OA for an entry
 * @pts: Entry to query
 *
 * During iteration the first entry could have a VA with an offset from the
 * natural start of the entry. Return the true full OA considering the pts's VA
 * offset.
 */
static inline pt_oaddr_t pt_entry_oa_full(const struct pt_state *pts)
{
	return _pt_entry_oa_fast(pts) |
	       log2_mod(pts->range->va, pt_entry_oa_lg2sz(pts));
}

/**
 * pt_entry_set_write_clean() - Make the entry write clean
 * @pts: Table index to change
 *
 * Modify the entry so that pt_entry_write_is_dirty() == false. The HW will
 * eventually be notified of this change via a TLB flush, which is the point
 * that the HW must become synchronized. Any "write dirty" prior to the TLB
 * flush can be lost, but once the TLB flush completes all writes must make
 * their entries write dirty.
 *
 * The format should alter the entry in a way that is compatible with any
 * concurrent update from HW. The entire contiguous entry is changed.
 */
static inline void pt_entry_set_write_clean(struct pt_state *pts);

/**
 * pt_entry_write_is_dirty() - True if the entry has been written to
 * @pts: Entry to query
 *
 * "write dirty" means that the HW has written to the OA translated
 * by this entry. If the entry is contiguous then the consolidated
 * "write dirty" for all the items must be returned.
 */
static inline bool pt_entry_write_is_dirty(const struct pt_state *pts);

/**
 * pt_full_va_prefix() - The top bits of the VA
 * @common: Page table to query
 *
 * This is usually 0, but some formats have their VA space going downward from
 * PT_VADDR_MAX, and will return that instead. This value must always be
 * adjusted by struct pt_common max_vasz_lg2.
 */
static inline pt_vaddr_t pt_full_va_prefix(const struct pt_common *common);

/**
 * pt_install_leaf_entry() - Write a leaf entry to the table
 * @pts: Table index to change
 * @oa: Output Address for this leaf
 * @oasz_lg2: Size in VA for this leaf
 *
 * A leaf OA entry will return PT_ENTRY_OA from pt_load_entry(). It translates
 * the VA indicated by pts to the given OA.
 *
 * For a single item non-contiguous entry oasz_lg2 is pt_table_item_lg2sz().
 * For contiguous it is pt_table_item_lg2sz() + num_contig_lg2.
 *
 * This must not be called if pt_can_have_leaf() == false. Contigous sizes
 * not indicated by pt_possible_sizes() must not be specified.
 */
static inline void pt_install_leaf_entry(struct pt_state *pts, pt_oaddr_t oa,
					 unsigned int oasz_lg2,
					 const struct pt_write_attrs *attrs);

/**
 * pt_install_table() - Write a table entry to the table
 * @pts: Table index to change
 * @table_pa: CPU physical address of the lower table's memory
 * @attrs: Attributes to modify the table index
 *
 * A table entry will return PT_ENTRY_TABLE from pt_load_entry(). The table_pa
 * is the table at pts->level - 1.
 *
 * This must not be called if pt_can_have_table() == false.
 */
static inline bool pt_install_table(struct pt_state *pts, pt_oaddr_t table_pa,
				    const struct pt_write_attrs *attrs);

/**
 * pt_item_oa() - Output Address for this leaf item
 * @pts: Item to query
 *
 * Return the output address for this item. If the item is part of a contiguous
 * entry it returns the value of the OA for this individual sub item.
 *
 * See pt_entry_oa(). The format should implement one of these two functions
 * depending on how it stores the OA's in the table.
 */
static inline pt_oaddr_t pt_item_oa(const struct pt_state *pts);

/**
 * pt_load_entry_raw() - Read from the location pts points at into the pts
 * @pts: Table index to load
 *
 * Return the type of entry that was loaded. pts->entry will be filled in with
 * the entry's content. See pt_load_entry()
 */
static inline enum pt_entry_type pt_load_entry_raw(struct pt_state *pts);

/**
 * pt_max_output_address_lg2() - Return the maximum OA the table format can hold
 * @common: Page table to query
 *
 * The value oalog2_to_max_int(pt_max_output_address_lg2()) is the MAX for the
 * OA. This is the absolute maximum address the table can hold. struct pt_common
 * max_oasz_lg2 sets a lower dynamic maximum based on HW capability.
 */
static inline unsigned int
pt_max_output_address_lg2(const struct pt_common *common);

/**
 * pt_num_items_lg2() - Return the number of items in this table level
 * @pts: The current level
 *
 * The number of items in a table level defines the number of bits this level
 * decodes from the VA. This function is not called for the top level,
 * so it does not need to compute a special value for the top case. The
 * result for the top is based on pt_common max_vasz_lg2.
 *
 * The value is used as part if determining the table indexes via the
 * equation:
 *   log2_mod(log2_div(VA, pt_table_item_lg2sz()), pt_num_items_lg2())
 */
static inline unsigned int pt_num_items_lg2(const struct pt_state *pts);

/**
 * pt_possible_sizes() - Return a bitmap of possible output sizes at this level
 * @pts: The current level
 *
 * Each level has a list of possible output sizes that can be installed as
 * leaf entries. If pt_can_have_leaf() is false returns zero.
 *
 * Otherwise the bit in position pt_table_item_lg2sz() should be set indicating
 * that a non-contigous singe item leaf entry is supported. The following
 * pt_num_items_lg2() number of bits can be set indicating contiguous entries
 * are supported. Bit pt_table_item_lg2sz() + pt_num_items_lg2() must not be
 * set, contiguous entries cannot span the entire table.
 *
 * The OR of pt_possible_sizes() of all levels is the typical bitmask of all
 * supported sizes in the entire table.
 */
static inline pt_vaddr_t pt_possible_sizes(const struct pt_state *pts);

/**
 * pt_table_item_lg2sz() - Size of a single item entry in this table level
 * @pts: The current level
 *
 * The size of the item specifies how much VA and OA a single item occupies.
 *
 * See pt_entry_oa_lg2sz() for the same value including the effect of contiguous
 * entries.
 */
static inline unsigned int pt_table_item_lg2sz(const struct pt_state *pts);

/**
 * pt_table_oa_lg2sz() - Return the VA/OA size of the entire table
 * @pts: The current level
 *
 * Return the size of VA decoded by the entire table level.
 */
static inline unsigned int pt_table_oa_lg2sz(const struct pt_state *pts)
{
	return min_t(unsigned int, pts->range->common->max_vasz_lg2,
		     pt_num_items_lg2(pts) + pt_table_item_lg2sz(pts));
}

/**
 * pt_table_pa() - Return the CPU physical address of the table entry
 * @pts: Entry to query
 *
 * This is only ever called on PT_ENTRY_TABLE entries. Must return the same
 * value passed to pt_install_table().
 */
static inline pt_oaddr_t pt_table_pa(const struct pt_state *pts);

/**
 * pt_table_ptr() - Return a CPU pointer for a table item
 * @pts: Entry to query
 *
 * Same as pt_table_pa() but returns a CPU pointer.
 */
static inline struct pt_table_p *pt_table_ptr(const struct pt_state *pts)
{
	return __va(pt_table_pa(pts));
}

/**
 * pt_load_entry() - Read from the location pts points at into the pts
 * @pts: Table index to load
 *
 * Set the type of entry that was loaded. pts->entry and pts->table_lower
 * will be filled in with the entry's content.
 */
static inline void pt_load_entry(struct pt_state *pts)
{
	pts->type = pt_load_entry_raw(pts);
	if (pts->type == PT_ENTRY_TABLE)
		pts->table_lower = pt_table_ptr(pts);
}
#endif
