/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES
 *
 * Simple IOTLB emulation for verifying struct iommu_iotlb_gather correctness.
 *
 * The emulated IOTLB is organized into a walk/paging structure cache and a
 * leaf cache. Each cache is then further organized into silos by page size.
 * This allows the invalidation logic to target specific IOTLB entries which
 * is similar to the invalidations systems of ARM and RISC-V.
 *
 * The emulated IOTLB is activated by calling kunit_iotlb_start() which sets
 * priv->iotlb non-NULL. Once active:
 *  - iotlb_sync callback verifies the gather covers all page table changes
 *    then re-marks entries for the next cycle
 *  - iotlb_sync_map callback validates that existing entries are unchanged,
 *    adds new entries from map, and detects entries removed without invalidation
 */
#ifndef __GENERIC_PT_KUNIT_IOMMU_IOTLB_H
#define __GENERIC_PT_KUNIT_IOMMU_IOTLB_H

#include <kunit/test.h>
#include <linux/xarray.h>
#include "../iommu-pages.h"
#include "pt_iter.h"
#include "kunit_iommu.h"

/*
 * Mark set on every xarray entry during the read phase. Cleared during the
 * compare walk when an entry is matched against the current page table.
 * Any entries still marked after the walk are iotlb extras that have no
 * corresponding page table entry.
 */
#define INV_XA_MARK_UNSEEN XA_MARK_0

enum kunit_iommu_inv_model {
	KUNIT_IOMMU_MODEL_GENERIC,
	KUNIT_IOMMU_MODEL_AMD,
};

struct kunit_iommu_inv_iotlb {
	struct xarray leaf_entries[PT_VADDR_MAX_LG2];
	struct xarray table_entries[PT_VADDR_MAX_LG2];
	enum kunit_iommu_inv_model model;
};

/*
 * Encode a PTE for xarray storage. xa_mk_value() shifts left by 1, so mask off
 * bit 63 to avoid overflow. For testing invalidation bit 63 is not important.
 */
static void *inv_pte_to_xa(u64 entry)
{
	return xa_mk_value((unsigned long)(entry & ~BIT_ULL(63)));
}

static u64 xa_to_inv_pte(void *entry)
{
	return xa_to_value(entry);
}

static void kunit_iotlb_cmp_pte(struct kunit *test, void *iotlb_pte, u64 pt_pte,
				u64 va, unsigned int lg2sz)
{
	KUNIT_ASSERT_EQ_MSG(test, xa_to_inv_pte(iotlb_pte),
			    xa_to_inv_pte(inv_pte_to_xa(pt_pte)),
			    KUNIT_SUBSUBTEST_INDENT
			    "entry mismatch at VA %llx lg2sz %u",
			    va, lg2sz);
}

static void kunit_iotlb_compare_entry(struct kunit *test, struct xa_state *xas,
				      pt_vaddr_t va, unsigned int lg2sz,
				      u64 entry)
{
	void *iotlb_pte;

	xas_set(xas, log2_div(va, lg2sz));
	xas_lock(xas);
	iotlb_pte = xas_load(xas);
	xas_clear_mark(xas, INV_XA_MARK_UNSEEN);
	xas_unlock(xas);
	kunit_iotlb_cmp_pte(test, iotlb_pte, entry, va, lg2sz);
}

static void kunit_iotlb_update_entry(struct kunit *test, struct xa_state *xas,
				     pt_vaddr_t va, unsigned int lg2sz,
				     u64 entry)
{
	pt_vaddr_t xa_idx = log2_div(va, lg2sz);
	void *old;

	xas_set(xas, xa_idx);
	do {
		xas_lock(xas);
		old = xas_load(xas);
		if (old) {
			xas_clear_mark(xas, INV_XA_MARK_UNSEEN);
			xas_unlock(xas);

			kunit_iotlb_cmp_pte(test, old, entry, va, lg2sz);
			return;
		}
		xas_store(xas, inv_pte_to_xa(entry));
		xas_unlock(xas);
	} while (xas_nomem(xas, GFP_KERNEL));

	KUNIT_ASSERT_EQ(test, xas_error(xas), 0);
}

struct kunit_iommu_iotlb_walk_arg {
	struct kunit *test;
	struct kunit_iommu_inv_iotlb *iotlb;
	bool table_only;
};

/*
 * If the range starts in the middle of a contiguous entry, rewind to the base
 * of the contiguous block and reload. This ensures the iotlb always
 * stores/compares the canonical (first-item) PTE value.
 */
static void kunit_iotlb_normalize_contig(struct pt_state *pts)
{
	unsigned int num_contig_lg2 = pt_entry_num_contig_lg2(pts);

	if (num_contig_lg2 && log2_mod(pts->index, num_contig_lg2)) {
		pts->index = log2_set_mod(pts->index, 0, num_contig_lg2);
		pt_index_to_va(pts);
		pt_load_entry(pts);
	}
}

/*
 * Ensure the iotlb matches the table. After unmap the gather is applied
 * and the iotlb should not be changed.
 */
static int __iotlb_compare_pt(struct pt_range *range, void *arg,
			      unsigned int level, struct pt_table_p *table)
{
	struct pt_state pts = pt_init(range, level, table);
	struct kunit_iommu_iotlb_walk_arg *cmp = arg;
	struct kunit *test = cmp->test;

	for_each_pt_level_entry(&pts) {
		if (pts.type == PT_ENTRY_TABLE) {
			unsigned int lg2sz = pt_table_item_lg2sz(&pts);
			XA_STATE(xas, &cmp->iotlb->table_entries[lg2sz], 0);

			kunit_iotlb_compare_entry(test, &xas, range->va, lg2sz,
						  pts.entry);
			pt_descend(&pts, arg, __iotlb_compare_pt);
			continue;
		}
		if (pts.type == PT_ENTRY_OA) {
			unsigned int lg2sz = pt_entry_oa_lg2sz(&pts);
			XA_STATE(xas, &cmp->iotlb->leaf_entries[lg2sz], 0);

			kunit_iotlb_normalize_contig(&pts);

			kunit_iotlb_compare_entry(test, &xas, range->va, lg2sz,
						  pts.entry);
			continue;
		}
	}
	return 0;
}

static void kunit_iotlb_compare_pt(struct kunit *test,
				   struct kunit_iommu_priv *priv)
{
	struct kunit_iommu_iotlb_walk_arg cmp = {
		.test = test,
		.iotlb = priv->iotlb,
	};
	struct pt_range range = pt_top_range(priv->common);

	KUNIT_ASSERT_NO_ERRNO(test,
			      pt_walk_range(&range, __iotlb_compare_pt, &cmp));
	if (pt_feature(priv->common, PT_FEAT_SIGN_EXTEND)) {
		range = pt_upper_range(priv->common);
		KUNIT_ASSERT_NO_ERRNO(
			test, pt_walk_range(&range, __iotlb_compare_pt, &cmp));
	}
}

/*
 * Load new map'd entries into the table and validate that existing entries are
 * correct.
 */
static int __iotlb_update_pt(struct pt_range *range, void *arg,
			     unsigned int level, struct pt_table_p *table)
{
	struct pt_state pts = pt_init(range, level, table);
	struct kunit_iommu_iotlb_walk_arg *cmp = arg;
	struct kunit *test = cmp->test;

	for_each_pt_level_entry(&pts) {
		if (pts.type == PT_ENTRY_TABLE) {
			unsigned int lg2sz = pt_table_item_lg2sz(&pts);
			XA_STATE(xas, &cmp->iotlb->table_entries[lg2sz], 0);

			kunit_iotlb_update_entry(test, &xas, range->va, lg2sz,
						 pts.entry);
			pt_descend(&pts, arg, __iotlb_update_pt);
			continue;
		}
		if (pts.type == PT_ENTRY_OA && !cmp->table_only) {
			unsigned int lg2sz;

			kunit_iotlb_normalize_contig(&pts);
			lg2sz = pt_entry_oa_lg2sz(&pts);
			{
			XA_STATE(xas, &cmp->iotlb->leaf_entries[lg2sz], 0);

			kunit_iotlb_update_entry(test, &xas, range->va, lg2sz,
						 pts.entry);
			}
			continue;
		}
	}
	return 0;
}

/*
 * Ensure that every iotlb entry was seen by the compare/update walk. Unseen
 * entries means the page table removed an entry and did not invalidate it.
 */
static void kunit_iotlb_check_no_extra(struct kunit *test,
				       struct kunit_iommu_inv_iotlb *iotlb)
{
	unsigned int pgsz_lg2;
	unsigned long idx;
	void *entry;

	for (pgsz_lg2 = 0; pgsz_lg2 < PT_VADDR_MAX_LG2; pgsz_lg2++) {
		xa_for_each_marked(&iotlb->leaf_entries[pgsz_lg2], idx, entry,
				   INV_XA_MARK_UNSEEN)
			KUNIT_ASSERT_TRUE_MSG(
				test, false,
				KUNIT_SUBSUBTEST_INDENT
				"extra leaf in iotlb at va %llx lg2sz %u",
				(u64)log2_mul(idx, pgsz_lg2), pgsz_lg2);

		xa_for_each_marked(&iotlb->table_entries[pgsz_lg2], idx, entry,
				   INV_XA_MARK_UNSEEN)
			KUNIT_ASSERT_TRUE_MSG(
				test, false,
				KUNIT_SUBSUBTEST_INDENT
				"extra table in iotlb at va %llx lg2sz %u",
				(u64)log2_mul(idx, pgsz_lg2), pgsz_lg2);
	}
}

static void kunit_iotlb_reload_range(struct kunit_iommu_priv *priv,
				     pt_vaddr_t start, pt_vaddr_t last,
				     bool table_only)
{
	struct kunit_iommu_iotlb_walk_arg cmp = {
		.test = priv->test,
		.iotlb = priv->iotlb,
		.table_only = table_only,
	};
	struct pt_range range;

	/*
	 * If start/last spans the entire hole because of
	 * kunit_iotlb_erase_all() split into two walks covering each valid
	 * half.
	 */
	if (pt_feature(priv->common, PT_FEAT_SIGN_EXTEND)) {
		struct pt_range lower = pt_top_range(priv->common);
		struct pt_range upper = pt_upper_range(priv->common);

		if (start <= lower.last_va && last >= upper.va) {
			lower.va = start;
			KUNIT_ASSERT_NO_ERRNO(priv->test,
					      pt_check_range(&lower));
			KUNIT_ASSERT_NO_ERRNO(
				priv->test,
				pt_walk_range(&lower, __iotlb_update_pt, &cmp));

			upper.last_va = last;
			KUNIT_ASSERT_NO_ERRNO(priv->test,
					      pt_check_range(&upper));
			KUNIT_ASSERT_NO_ERRNO(
				priv->test,
				pt_walk_range(&upper, __iotlb_update_pt, &cmp));
			return;
		}
	}

	range = pt_make_range(priv->common, start, last);
	KUNIT_ASSERT_NO_ERRNO(priv->test, pt_check_range(&range));
	KUNIT_ASSERT_NO_ERRNO(priv->test,
			      pt_walk_range(&range, __iotlb_update_pt, &cmp));
}

struct kunit_iotlb_range {
	pt_vaddr_t start;
	pt_vaddr_t last;
	u64 leaf_invals;
	u64 table_invals;
};
#define IOTLB_RANGE_INIT (struct kunit_iotlb_range){ .start = PT_VADDR_MAX }

static void kunit_iotlb_erase_xa_range(struct kunit_iommu_inv_iotlb *iotlb,
				       bool leaf, unsigned long start_idx,
				       unsigned long last_idx,
				       unsigned int pgsz_lg2,
				       struct kunit_iotlb_range *real_inval)
{
	bool erased = false;
	unsigned long idx;
	struct xarray *xa;
	void *entry;

	if (leaf)
		xa = &iotlb->leaf_entries[pgsz_lg2];
	else
		xa = &iotlb->table_entries[pgsz_lg2];

	xa_for_each_range(xa, idx, entry, start_idx, last_idx) {
		u64 start = log2_mul_t(u64, idx, pgsz_lg2);
		u64 last = log2_set_mod_max_t(u64, start, pgsz_lg2);

		if (start < real_inval->start)
			real_inval->start = start;
		if (last > real_inval->last)
			real_inval->last = last;
		xa_erase(xa, idx);
		erased = true;
	}

	if (erased) {
		if (leaf)
			real_inval->leaf_invals |= 1ULL << pgsz_lg2;
		else
			real_inval->table_invals |= 1ULL << pgsz_lg2;
	}
}

/*
 * leaf_sizes and table_sizes are a bitmap of pages sizes that are effected by
 * the invalidation. U64_MAX selects all.
 */
static void kunit_iotlb_erase_range(struct kunit_iommu_inv_iotlb *iotlb,
				    u64 start, u64 last, u64 leaf_sizes,
				    u64 table_sizes,
				    struct kunit_iotlb_range *real_inval)
{
	unsigned long start_idx;
	unsigned long last_idx;
	unsigned int pgsz_lg2;

	for (pgsz_lg2 = 1; pgsz_lg2 < PT_VADDR_MAX_LG2; pgsz_lg2++) {
		start_idx = log2_div_t(u64, start, pgsz_lg2);
		last_idx = log2_div_t(u64, last, pgsz_lg2);

		if (leaf_sizes & BIT_U64(pgsz_lg2))
			kunit_iotlb_erase_xa_range(iotlb, true, start_idx,
						   last_idx, pgsz_lg2,
						   real_inval);
		if (table_sizes & BIT_U64(pgsz_lg2))
			kunit_iotlb_erase_xa_range(iotlb, false, start_idx,
						   last_idx, pgsz_lg2,
						   real_inval);
	}
}

static void kunit_iotlb_erase_all(struct kunit_iommu_inv_iotlb *iotlb,
				  struct kunit_iotlb_range *real_inval)
{
	kunit_iotlb_erase_range(iotlb, 0, U64_MAX, U64_MAX, U64_MAX,
				real_inval);
}

/*
 * The invalidation emulations can remove more from the IOTLB than the unmap
 * actually requires. We need to check and remap the stuff that shouldn't have
 * been removed to make the validation steps work. real_inval contains the
 * actual range of entries that were altered by the invalidation.
 */
static void kunit_iotlb_unmap_fixup(struct kunit_iommu_priv *priv,
				    struct kunit_iotlb_range *real_inval,
				    struct iommu_iotlb_gather *gather)
{
	if (real_inval->start < gather->start)
		kunit_iotlb_reload_range(priv, real_inval->start,
					 gather->start - 1, false);
	if (gather->end < real_inval->last)
		kunit_iotlb_reload_range(priv, gather->end + 1,
					 real_inval->last, false);

	/*
	 * The gather can cross multiple tables but the actual unmap may not
	 * have if the gather was constructed from multiple unmap calls. In this
	 * case the tables within the unmapped range need to be restored.
	 */
	if (real_inval->table_invals)
		kunit_iotlb_reload_range(priv, gather->start, gather->end,
					 true);
}

/* INVALIDATE_IOMMU_PAGES command for AMD IOMMU */
struct kunit_amd_inval {
	u64 address;
	/* page directoy entries */
	bool pde;
	/* Easy version of the 1's size encoding in the address */
	unsigned int sz_lg2;
	struct kunit_iotlb_range real_inval;
};

static void kunit_iotlb_amd_cmd(struct kunit *test,
				struct kunit_iommu_inv_iotlb *iotlb,
				struct kunit_amd_inval *cmd)
{
	u64 start = cmd->address;
	u64 last = log2_set_mod_max_t(u64, cmd->address, cmd->sz_lg2);

	if (cmd->sz_lg2 >= 52) {
		kunit_iotlb_erase_all(iotlb, &cmd->real_inval);
		return;
	}

	/*
	 * Spec says:
	 *   Software Note: When issuing INVALIDATE_IOMMU_PAGES
	 *   commands, the size of each invalidate must be greater than
	 *   or equal to the size of the largest page being invalidated.
	 * Meaning it never invalidates any IOTLB entry sized greater than
	 * sz_lg2. Vasant confirmed this.
	 */
	kunit_iotlb_erase_range(iotlb, start, last,
				GENMASK_U64(cmd->sz_lg2, 0),
				cmd->pde ? GENMASK_U64(cmd->sz_lg2, 0) : 0,
				&cmd->real_inval);
}

static void kunit_iotlb_range_gather_amd(struct kunit_iommu_priv *priv,
					 struct iommu_iotlb_gather *gather)
{
	struct kunit *test = priv->test;
	struct kunit_iommu_inv_iotlb *iotlb = priv->iotlb;
	struct kunit_amd_inval cmd = {
		.pde = !iommu_pages_list_empty(&gather->freelist),
		.real_inval = IOTLB_RANGE_INIT,
	};
	u64 cmd_last;

	/*
	 * AMD can do power of two ranges with an aligned starting point.
	 * Compute the smallest power of two that covers all the addresses.
	 */
	cmd.sz_lg2 = fls_t(unsigned long, gather->start ^ gather->end);
	/* S bit is 0 if sz_lg2 == 12 */
	if (cmd.sz_lg2 < 12)
		cmd.sz_lg2 = 12;
	cmd.address =
		log2_set_mod_t(unsigned long, gather->start, 0, cmd.sz_lg2);
	cmd_last = log2_set_mod_max_t(u64, cmd.address, cmd.sz_lg2);

	KUNIT_ASSERT_LE(test, cmd.address, gather->start);
	KUNIT_ASSERT_GE(test, cmd_last, gather->end);
	kunit_iotlb_amd_cmd(test, iotlb, &cmd);
	kunit_iotlb_unmap_fixup(priv, &cmd.real_inval, gather);
}

/*
 * Emulate HW with a range invalidation operation using the detailed gather
 * bitmaps to restrict which IOTLB entries are invalidated:
 *  - Invalidate leaf entries only at page sizes belonging to levels in
 *    leaf_levels_bitmap, starting from min_leaf_lg2sz
 *  - Invalidate walk cache entries only at page sizes belonging to levels in
 *    table_levels_bitmap
 *
 * Exclusive means if the start -> end do not fully cover an IOTLB entry it is
 * not invalidated. If HW supports inclusive then it will be compatible with
 * this implementation as it will be invalidating more than required.
 */
static void
kunit_iotlb_range_gather_generic(struct kunit_iommu_priv *priv,
				 struct iommu_iotlb_gather *gather)
{
	struct kunit_iotlb_range real_inval = IOTLB_RANGE_INIT;
	struct kunit_iommu_inv_iotlb *iotlb = priv->iotlb;
	pt_vaddr_t gstart = gather->start;
	pt_vaddr_t gend = gather->end;
	unsigned int pgsz_lg2;

	for (pgsz_lg2 = 1; pgsz_lg2 < PT_VADDR_MAX_LG2; pgsz_lg2++) {
		unsigned int level =
			pt_pgsz_lg2_to_level(priv->common, pgsz_lg2);
		unsigned long start_idx = log2_div(gstart, pgsz_lg2);
		unsigned long end_idx = log2_div(gend, pgsz_lg2);

		/* Round up and down to implement the exclusive rules */
		if (log2_mod(gstart, pgsz_lg2))
			start_idx++;
		if (!log2_mod_eq_max(gend, pgsz_lg2)) {
			if (end_idx == 0)
				continue;
			end_idx--;
		}

		if (start_idx > end_idx)
			continue;

		if (gather->pt.leaf_levels_bitmap & BIT(level))
			kunit_iotlb_erase_xa_range(iotlb, true, start_idx,
						   end_idx, pgsz_lg2,
						   &real_inval);
		if (gather->pt.table_levels_bitmap & BIT(level))
			kunit_iotlb_erase_xa_range(iotlb, false, start_idx,
						   end_idx, pgsz_lg2,
						   &real_inval);
	}
}

static void kunit_iotlb_remark_xa(struct xarray *xa)
{
	XA_STATE(xas, xa, 0);
	void *entry;

	xas_lock(&xas);
	xas_for_each(&xas, entry, ULONG_MAX)
		xas_set_mark(&xas, INV_XA_MARK_UNSEEN);
	xas_unlock(&xas);
}

/*
 * Re-set INV_XA_MARK_UNSEEN on all iotlb entries. After a successful compare
 * walk all marks have been cleared, so this prepares the iotlb for the next
 * verify cycle without re-walking the page table.
 */
static void kunit_iotlb_remark(struct kunit_iommu_inv_iotlb *iotlb)
{
	unsigned int pgsz_lg2;

	for (pgsz_lg2 = 0; pgsz_lg2 < PT_VADDR_MAX_LG2; pgsz_lg2++) {
		kunit_iotlb_remark_xa(&iotlb->leaf_entries[pgsz_lg2]);
		kunit_iotlb_remark_xa(&iotlb->table_entries[pgsz_lg2]);
	}
}

/*
 * iotlb_sync callback: verify the gather covers all changes, then re-mark
 * entries for the next cycle. After the successful compare walk, all remaining
 * iotlb entries already match the page table so re-marking is sufficient.
 */
static void kunit_iotlb_sync(struct kunit_iommu_priv *priv,
			     struct iommu_iotlb_gather *gather)
{
	struct kunit_iommu_inv_iotlb *iotlb = priv->iotlb;
	struct kunit *test = priv->test;
	struct pt_range range =
		pt_make_range(priv->common, gather->start, gather->end);

	if (!pt_feature(priv->common, PT_FEAT_DETAILED_GATHER))
		return;

	/*
	 * The gather range must be aligned to at least the domain's page size
	 */
	KUNIT_ASSERT_EQ(test,
			log2_mod_t(unsigned long, gather->start,
				   priv->smallest_pgsz_lg2),
			0);
	KUNIT_ASSERT_TRUE(test, log2_mod_eq_max_t(unsigned long, gather->end,
						  priv->smallest_pgsz_lg2));
	KUNIT_ASSERT_NO_ERRNO(test, pt_check_range(&range));

	KUNIT_ASSERT_TRUE(test, gather->pt.leaf_levels_bitmap ||
					gather->pt.table_levels_bitmap);
	KUNIT_ASSERT_EQ(test, !gather->pt.table_levels_bitmap,
			iommu_pages_list_empty(&gather->freelist));

	switch (iotlb->model) {
	case KUNIT_IOMMU_MODEL_AMD:
		kunit_iotlb_range_gather_amd(priv, gather);
		break;
	case KUNIT_IOMMU_MODEL_GENERIC:
	default:
		kunit_iotlb_range_gather_generic(priv, gather);
		break;
	}

	/*
	 * This is a table only invalidation from map which means the caller may
	 * have populated IOPTEs that have not been through
	 * kunit_iotlb_sync_map() yet so we cannot compare.
	 */
	if (!gather->pt.leaf_levels_bitmap)
		return;

	kunit_iotlb_compare_pt(test, priv);
	kunit_iotlb_check_no_extra(test, iotlb);
	kunit_iotlb_remark(iotlb);
}

/*
 * iotlb_sync_map callback: validate the iotlb against the current page table
 * after a map operation. Existing entries must match exactly (detects changes
 * without invalidation), new entries are added, and entries present in the
 * iotlb but missing from the page table are test failures.
 */
static void kunit_iotlb_sync_map(struct kunit_iommu_priv *priv,
				 unsigned long iova, size_t size)
{
	struct kunit *test = priv->test;
	struct kunit_iommu_inv_iotlb *iotlb = priv->iotlb;

	if (!pt_feature(priv->common, PT_FEAT_DETAILED_GATHER))
		return;

	if (!size)
		return;

	/* Update the iotlb only within the mapped range */
	kunit_iotlb_reload_range(priv, iova, iova + size - 1, false);

	/* Recheck the entire iotlb against the page table */
	kunit_iotlb_compare_pt(test, priv);
	kunit_iotlb_check_no_extra(test, iotlb);
	kunit_iotlb_remark(iotlb);
}

/*
 * When the top is changed then the newly added table entries needs to be loaded
 * into the xarrays. Called under a spinlock.
 */
static void kunit_iotlb_change_top(struct kunit_iommu_priv *priv,
				   phys_addr_t top_paddr,
				   unsigned int top_level)
{
	unsigned int old_level = pt_top_get_level(priv->common);
	struct pt_table_p *table = phys_to_virt(top_paddr);
	unsigned int level = top_level;

	if (!pt_feature(priv->common, PT_FEAT_DETAILED_GATHER))
		return;

	while (level > old_level) {
		struct pt_range range =
			_pt_top_range(priv->common, _pt_top_set(table, level));
		struct pt_state pts = pt_init_top(&range);
		unsigned int lg2sz;

		pt_load_single_entry(&pts);
		KUNIT_ASSERT_EQ(priv->test, pts.type, PT_ENTRY_TABLE);
		lg2sz = pt_table_item_lg2sz(&pts);
		xa_store(&priv->iotlb->table_entries[lg2sz],
			 log2_div(range.va, lg2sz), inv_pte_to_xa(pts.entry),
			 GFP_ATOMIC);

		table = pts.table_lower;
		level--;
	}
}

/*
 * Activate the iotlb. Once priv->iotlb is non-NULL the iotlb_sync and
 * iotlb_sync_map callbacks will automatically verify page table changes.
 */
static void kunit_iotlb_start(struct kunit *test, struct kunit_iommu_priv *priv)
{
	const char *format_name = __stringify(PTPFX_RAW);
	unsigned int i;

	priv->iotlb = kunit_kzalloc(test, sizeof(*priv->iotlb), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->iotlb);

	if (strstr(format_name, "amd"))
		priv->iotlb->model = KUNIT_IOMMU_MODEL_AMD;

	for (i = 0; i < PT_VADDR_MAX_LG2; i++) {
		xa_init(&priv->iotlb->leaf_entries[i]);
		xa_init(&priv->iotlb->table_entries[i]);
	}
}

#endif
