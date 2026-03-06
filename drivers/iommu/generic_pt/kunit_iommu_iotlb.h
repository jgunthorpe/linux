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
	KUNIT_IOMMU_MODEL_ARMV8,
	KUNIT_IOMMU_MODEL_VTD,
	KUNIT_IOMMU_MODEL_RISCV,
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

static void __kunit_iotlb_erase_xa_range(
	struct kunit_iommu_inv_iotlb *iotlb, bool leaf, unsigned long start_idx,
	unsigned long last_idx, unsigned int pgsz_lg2, u64 inval_start,
	u64 inval_last, bool require_full,
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

		if (require_full &&
		    (start < inval_start || last > inval_last))
			continue;

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

static void kunit_iotlb_erase_xa_range(struct kunit_iommu_inv_iotlb *iotlb,
				       bool leaf, unsigned long start_idx,
				       unsigned long last_idx,
				       unsigned int pgsz_lg2,
				       struct kunit_iotlb_range *real_inval)
{
	__kunit_iotlb_erase_xa_range(iotlb, leaf, start_idx, last_idx,
				      pgsz_lg2, 0, 0, false, real_inval);
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

/* Erase only leaf entries that are fully contained by the range */
static void kunit_iotlb_erase_full_leaf_range(
	struct kunit_iommu_inv_iotlb *iotlb, u64 start, u64 last, u64 leaf_sizes,
	struct kunit_iotlb_range *real_inval)
{
	unsigned long start_idx;
	unsigned long last_idx;
	unsigned int pgsz_lg2;

	for (pgsz_lg2 = 1; pgsz_lg2 < PT_VADDR_MAX_LG2; pgsz_lg2++) {
		start_idx = log2_div_t(u64, start, pgsz_lg2);
		last_idx = log2_div_t(u64, last, pgsz_lg2);

		if (leaf_sizes & BIT_U64(pgsz_lg2))
			__kunit_iotlb_erase_xa_range(iotlb, true, start_idx,
						     last_idx, pgsz_lg2, start,
						     last, true, real_inval);
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

/*
 * ARM SMMUv3 CMD_TLBI_NH_VA / CMD_TLBI_NH_VAA / CMD_TLBI_S2_IPA commands
 * SMMUv3 F.b Sections 4.4.2.3, 4.4.2.4, 4.4.3.1
 *
 * Fields use natural types rather than the HW bit encodings. "ARM level" (-1
 * through 3) is used here rather than the generic pt level of (4 through 0)
 */
struct kunit_armv8_inval {
	u64 address;
	/*
	 * Translation Granule log2 size. 0 = unspecified (TG=0b00: no range
	 * invalidation, no TTL hint). 12/14/16 = 4K/16K/64K.
	 */
	unsigned int tg;
	/*
	 * Translation Table Level hint (ARM level numbering).
	 * 0 = any level. 1/2/3 = ARM level 1/2/3.
	 */
	unsigned int ttl;
	/*
	 * Range granule multiplier (5 bits, 0-31). Range in granule-sized
	 * pages is (num + 1) * 2^scale.
	 */
	unsigned int num;
	/* Range scale exponent (six bits, limited to 31 without DS, 39 with DS). */
	unsigned int scale;
	/*
	 * Leaf=true (Leaf=1): only last-level (leaf) cached entries.
	 * Leaf=false (Leaf=0): table descriptor cache entries also invalidated.
	 */
	bool leaf;
	/* The SMMU and page table use the DS format. */
	bool ds;
	/* A single RIL must fully cover a CONT entry to invalidate it. */
	bool full_cont_ril;
	struct kunit_iotlb_range real_inval;
};


static u64 armv8_tg_to_table_sizes(unsigned int tg)
{
	switch (tg) {
	case 12:
		return SZ_2M | SZ_1G | SZ_512G | BIT_U64(48);
	case 14:
		return SZ_32M | SZ_64G | SZ_128T;
	case 16:
		return SZ_512M | SZ_4T;
	}
	return 0;
}

static u64 armv8_tg_to_leaf_sizes(unsigned int tg)
{
	switch (tg) {
	case 12:
		return SZ_4K | SZ_64K | SZ_2M | SZ_32M | SZ_1G | SZ_16G;
	case 14:
		return SZ_16K | SZ_2M | SZ_32M | SZ_1G | SZ_64G;
	case 16:
		return SZ_64K | SZ_2M | SZ_512M | SZ_16G;
	}
	return 0;
}

static u64 armv8_tg_to_cont_sizes(unsigned int tg)
{
	switch (tg) {
	case 12:
		return SZ_64K | SZ_32M | SZ_16G;
	case 14:
		return SZ_2M | SZ_1G;
	case 16:
		return SZ_2M | SZ_16G;
	}
	return 0;
}

/* Return the entry lg2sz for the given ARM level */
static unsigned int armv8_level_to_pgsz_lg2(unsigned int tg, int arm_level)
{
	return (tg - 3) * (3 - arm_level) + tg;
}

/*
 * Spec has requirements for what bits in the address are zero, basically
 * the bits that don't select a table item.
 */
static unsigned int armv8_ttl_addr_upper(unsigned int tg, unsigned int ttl)
{
	WARN_ON(ttl == 0);
	return armv8_level_to_pgsz_lg2(tg, ttl) - 1;
}

/*
 * Assert that the address meets the alignment requirements for the given
 * granule/TTL combination.
 */
static void kunit_iotlb_armv8_assert_addr_align(struct kunit *test,
					      const struct kunit_armv8_inval *cmd)
{
	unsigned int upper;

	if (cmd->ttl) {
		upper = armv8_ttl_addr_upper(cmd->tg, cmd->ttl);
		/*
		 * 16K granule with DS=0, TTL=1 is reserved. HW treats as TTL=0
		 * but Address[13:12] must still be zero to avoid unpredictable
		 * invalidation range.
		 */
		if (cmd->tg == 14 && cmd->ttl == 1 && !cmd->ds)
			upper = 13;
	} else {
		upper = armv8_ttl_addr_upper(cmd->tg, 3);
	}

	/* In the real cmd bits 11:0 are not encoded, check they are zero too */
	KUNIT_ASSERT_EQ(test, cmd->address & GENMASK_U64(upper, 0), 0);
}

/*
 * SMMUv3 F.b Section 4.4 p167:
 *   "addresses that are provided for TLB invalidation are not required to be
 *    aligned to the start of a TLB entry address range. To match a TLB entry,
 *    the least significant bits of the address are ignored as needed, given
 *    the size of the entry."
 */
static void kunit_iotlb_armv8_cmd(struct kunit *test,
				struct kunit_iommu_inv_iotlb *iotlb,
				struct kunit_armv8_inval *cmd)
{
	u64 leaf_sizes;
	u64 table_sizes;
	u64 cont_sizes = 0;
	u64 range_bytes;
	u64 last;

	KUNIT_ASSERT_TRUE(test, cmd->tg == 0 || cmd->tg == 12 ||
					cmd->tg == 14 || cmd->tg == 16);
	KUNIT_ASSERT_LE(test, cmd->ttl, 3U);
	KUNIT_ASSERT_LE(test, cmd->num, 31U);
	KUNIT_ASSERT_LE(test, cmd->scale, cmd->ds ? 39U : 31U);

	if (cmd->tg == 0) {
		/*
		 * Non-range mode invalidates all leafs and tables which contain
		 * the single address.
		 */
		KUNIT_ASSERT_EQ(test, cmd->num, 0);
		KUNIT_ASSERT_EQ(test, cmd->scale, 0);
		KUNIT_ASSERT_EQ(test, cmd->ttl, 0);

		last = cmd->address;
		leaf_sizes = U64_MAX;
		table_sizes = U64_MAX;
	} else {
		/* Range mode */
		/*
		 * Section 4.4.1 p170: TG!=0, NUM==0, SCALE==0, TTL==0 is
		 * Reserved (CERROR_ILL).
		 */
		KUNIT_ASSERT_FALSE(test, cmd->num == 0 && cmd->scale == 0 &&
						 cmd->ttl == 0);

		/*
		 * Section 4.4.1 p169 TTL table: TG=0b10 (16K), TTL=0b01 is
		 * Reserved (hardware treats as TTL=0b00).
		 */
		KUNIT_ASSERT_FALSE(test,
				   cmd->tg == 14 && cmd->ttl == 1 && !cmd->ds);

		kunit_iotlb_armv8_assert_addr_align(test, cmd);

		/*
		 * R_QNPXY: if the range exceeds the top of the address space
		 * (e.g. TTBR1 ending at U64_MAX), hardware does not wrap
		 */
		range_bytes = (u64)(cmd->num + 1) << (cmd->scale + cmd->tg);
		if (check_add_overflow(cmd->address, range_bytes - 1, &last))
			last = U64_MAX;

		leaf_sizes = armv8_tg_to_leaf_sizes(cmd->tg);
		if (cmd->full_cont_ril)
			cont_sizes = armv8_tg_to_cont_sizes(cmd->tg);
		if (cmd->ttl == 0) {
			/*
			 * Invalidate leaf entries at all levels of the matching
			 * granule, including contiguous sizes, and all table
			 * entries (Section 4.4.1 p169 TTL table, row TTL=0b00
			 * with TG!=0b00).
			 */
			table_sizes = U64_MAX;
		} else {
			/*
			 * TTL hint only targets leafs at the exact level and
			 * tables above the hint.
			 */
			leaf_sizes &= GENMASK_U64(
				armv8_level_to_pgsz_lg2(cmd->tg, cmd->ttl - 1) -
					1,
				armv8_level_to_pgsz_lg2(cmd->tg, cmd->ttl));
			table_sizes =
				armv8_tg_to_table_sizes(cmd->tg) &
				GENMASK_U64(63, armv8_level_to_pgsz_lg2(
							cmd->tg, cmd->ttl - 1));
		}
	}

	if (cmd->leaf)
		table_sizes = 0;

	cont_sizes &= leaf_sizes;
	kunit_iotlb_erase_range(iotlb, cmd->address, last,
				leaf_sizes & ~cont_sizes,
				table_sizes, &cmd->real_inval);
	kunit_iotlb_erase_full_leaf_range(iotlb, cmd->address, last,
					  cont_sizes, &cmd->real_inval);
}

static bool armv8_ttl_addr_aligned(u64 address, unsigned int tg, unsigned int ttl)
{
	unsigned int upper = armv8_ttl_addr_upper(tg, ttl);

	return !(address & GENMASK_U64(upper, 0));
}

struct kunit_armv8_ril_range {
	u64 start_tg;
	/* Normal integer, not encoded. 0 means no RIL. */
	u64 num;
	unsigned int scale;
};

static u64 armv8_ril_last_tg(const struct kunit_armv8_ril_range *ril)
{
	return ril->start_tg + (ril->num << ril->scale) - 1;
}

/* RIL a encloses the range covered by RIL b */
static bool armv8_ril_encloses(const struct kunit_armv8_ril_range *a,
			       const struct kunit_armv8_ril_range *b)
{
	return a->start_tg <= b->start_tg &&
	       armv8_ril_last_tg(a) >= armv8_ril_last_tg(b);
}

/* Initialize the smallest RIL covering num_tg and ending at last_tg. */
static struct kunit_armv8_ril_range
armv8_ril_init_end(u64 last_tg, u64 num_tg)
{
	struct kunit_armv8_ril_range ril = {};

	if (!num_tg)
		return ril;

	ril.scale = fls64((num_tg - 1) / 32);
	ril.num = DIV_ROUND_UP_ULL(num_tg, 1ULL << ril.scale);
	if (check_sub_overflow(last_tg, (ril.num << ril.scale) - 1,
			       &ril.start_tg))
		ril.num = 0;
	return ril;
}

static void armv8_add_ril(struct kunit *test,
			  struct kunit_iommu_inv_iotlb *iotlb,
			  struct kunit_armv8_inval *cmd,
			  const struct kunit_armv8_ril_range *ril,
			  unsigned int ttl, unsigned int tg)
{
	KUNIT_ASSERT_NE(test, ril->num, 0);

	cmd->address = ril->start_tg << tg;
	cmd->tg = tg;
	cmd->ttl = ttl;
	cmd->num = ril->num - 1;
	cmd->scale = ril->scale;

	/* Verify address alignment for the TTL hint. */
	if (cmd->ttl &&
	    !armv8_ttl_addr_aligned(cmd->address, cmd->tg, cmd->ttl))
		cmd->ttl = 0;

	/* A one-TG RIL without a TTL hint uses a non-range command. */
	if (!cmd->num && !cmd->scale && !cmd->ttl)
		cmd->tg = 0;

	kunit_iotlb_armv8_cmd(test, iotlb, cmd);
}

static int armv8_bitmap_to_level(u8 pt_bitmap)
{
	return 3 - (int)__ffs(pt_bitmap);
}

static unsigned int armv8_compute_ttl(u8 leaf_bitmap, u8 table_bitmap,
				      unsigned int tg)
{
	int ttl;

	if (leaf_bitmap) {
		/* If TTL is used then only leaves at the TTL are invalidated */
		if (!is_power_of_2(leaf_bitmap))
			return 0;

		ttl = armv8_bitmap_to_level(leaf_bitmap);
		if (table_bitmap) {
			int table_ttl = armv8_bitmap_to_level(
						table_bitmap) +
					1;

			/*
			 * A RIL invalidation with !leaf_only clears out all
			 * table levels above the leaf level ttl only.
			 */
			if (table_ttl > ttl)
				return 0;
		}
	} else if (table_bitmap) {
		/*
		 * Table-only invalidation. Spec says:
		 *  For operations with Leaf=0, invalidation of cached Table
		 *  descriptors for the address and scope additionally occurs at
		 *  levels between the start of the walk and the level before
		 *  the last level given by TTL.
		 * Choose a TTL hint that covers the only target table
		 * descriptor levels.
		 */
		ttl = armv8_bitmap_to_level(table_bitmap) + 1;

		/*
		 * 16K granule, ARM TTL=1 is reserved (SMMUv3 H.a Section
		 * 4.4.1.1) if DS=0, avoid it always for table invalidations
		 * since we don't know what instance this will be applied to
		 * yet.
		 */
		if (tg == 14 && ttl == 1)
			return 0;
	} else {
		/* Both bitmaps zero is not allowed */
		WARN_ON(true);
		return 0;
	}

	/*
	 * Assumes the page table is formed properly and does not trigger the
	 * 16K TTL=1 condition for leaf-only unless DS is enabled.
	 *
	 * ARM level -1 never has a leaf so something has gone wrong. ARM level
	 * 0 cannot be hinted because TTL=0 means no hint.
	 */
	if (WARN_ON(ttl < 0))
		return 0;
	return ttl;
}

enum kunit_armv8_ril_mode {
	KUNIT_ARMV8_RIL_NORMAL,
	KUNIT_ARMV8_RIL_SINGLE,
	KUNIT_ARMV8_RIL_CONT_ERRATA,
};

/* Number of contiguous entries grouped by CONT at an iommupt leaf level. */
static unsigned int armv8_cont_count_lg2(unsigned int tg,
					 unsigned int level)
{
	if (tg == 12 && level <= 3)
		return ilog2(16); /* 64KB, 32MB, 16GB, 8TB */
	else if (tg == 14 && level == 1)
		return ilog2(32); /* 1GB */
	else if (tg == 14 && level == 0)
		return ilog2(128); /* 2MB */
	else if (tg == 16 && level <= 2)
		return ilog2(32); /* 2MB, 16GB, 128TB */
	return 0;
}

/*
 * For the ARM_SMMU_OPT_FULL_CONT_RIL errata any CONT group must be fully
 * enclosed by a single RIL. The double RIL algorithm in
 * arm_smmu_tlbi_calc_range() does not guarantee this. So if two RILs were
 * produced we may need to extend the trailing RIL or add a third RIL to cover
 * a sliced CONT. Search for the largest CONT that could have been sliced,
 * extend the trailing RIL when that stays inside the gathered range, or queue
 * an exact third RIL.
 */
static bool armv8_cont_errata(struct kunit_iommu_priv *priv,
			      struct iommu_iotlb_gather *gather,
			      const struct kunit_armv8_ril_range *first,
			      struct kunit_armv8_ril_range *trail,
			      struct kunit_armv8_inval *cmd,
			      unsigned int ttl, unsigned int tg)
{
	struct kunit_iommu_inv_iotlb *iotlb = priv->iotlb;
	u8 leaf_levels = gather->pt.leaf_levels_bitmap;
	u64 range_start_tg = gather->start >> tg;
	u64 range_last_tg = gather->end >> tg;
	unsigned int scale_max = cmd->ds ? 39 : 31;
	struct kunit *test = priv->test;

	/* Table-only invalidation cannot slice a CONT. */
	if (!leaf_levels)
		return false;

	/*
	 * Iterate through all the leaf levels that were changed, from highest
	 * to lowest iommupt level. Check if a possible CONT at that level has
	 * been sliced by the double RIL.
	 */
	while (leaf_levels) {
		struct kunit_armv8_ril_range cont = { .num = 1 };
		unsigned int level = fls(leaf_levels) - 1;
		struct kunit_armv8_ril_range new_trail;
		unsigned int cont_count_lg2;
		u64 new_trail_num_tg;
		u64 cont_last_tg;

		leaf_levels &= ~BIT(level);
		cont_count_lg2 = armv8_cont_count_lg2(tg, level);
		if (!cont_count_lg2)
			continue;

		/* A NUM=0 RIL with this SCALE covers the entire CONT. */
		cont.scale = (tg - 3) * level + cont_count_lg2;
		if (WARN_ON(cont.scale > scale_max))
			return true;

		cont.start_tg =
			round_down(trail->start_tg - 1, BIT_ULL(cont.scale));
		cont_last_tg = armv8_ril_last_tg(&cont);
		if (cont.start_tg < range_start_tg ||
		    cont_last_tg > range_last_tg ||
		    armv8_ril_encloses(first, &cont) ||
		    armv8_ril_encloses(trail, &cont))
			continue;

		new_trail_num_tg = range_last_tg - cont.start_tg + 1;
		new_trail = armv8_ril_init_end(range_last_tg, new_trail_num_tg);
		if (new_trail.num && new_trail.start_tg >= range_start_tg) {
			*trail = new_trail;
			return false;
		}

		armv8_add_ril(test, iotlb, cmd, &cont, ttl, tg);
		return false;
	}
	return false;
}

/* Use range invalidation with SMMUv3 commands */
static void kunit_iotlb_range_gather_armv8(struct kunit_iommu_priv *priv,
					   struct iommu_iotlb_gather *gather,
					   enum kunit_armv8_ril_mode ril_mode)
{
	struct kunit *test = priv->test;
	struct kunit_iommu_inv_iotlb *iotlb = priv->iotlb;
	unsigned int tgsz_lg2 = priv->smallest_pgsz_lg2;
	unsigned int ttl = armv8_compute_ttl(gather->pt.leaf_levels_bitmap,
					     gather->pt.table_levels_bitmap,
					     tgsz_lg2);
	struct kunit_armv8_inval cmd = {
		.leaf = iommu_pages_list_empty(&gather->freelist),
		.ds = pt_feature(priv->common, PT_FEAT_ARMV8_LPA2),
		.full_cont_ril = ril_mode != KUNIT_ARMV8_RIL_NORMAL,
		.real_inval = IOTLB_RANGE_INIT,
	};
	struct kunit_armv8_ril_range first = {
		.start_tg = gather->start >> tgsz_lg2,
	};
	u64 last_tg = gather->end >> tgsz_lg2;
	u64 num_tg = last_tg - first.start_tg + 1;
	unsigned int scale_max = cmd.ds ? 39 : 31;
	struct kunit_armv8_ril_range trail;

	KUNIT_ASSERT_NE(test, num_tg, 0);

	/*
	 * The spec defines the invalidated range as:
	 *   Range = ((NUM+1) * 2^SCALE) * Translation_Granule_Size
	 * NUM is 5 bits, so (NUM+1) covers 1..32 granules. Find the smallest
	 * SCALE at which a single command could cover num_tg.
	 *
	 * Unlike other IOMMUs the spec has no alignment requirement on the
	 * address beyond alignment to tg (so long as TTL=0).
	 */
	first.scale = fls_t(u64, (num_tg - 1) / 32);
	if (first.scale > scale_max) {
		/* Range too large for a single command do full invalidation */
		kunit_iotlb_erase_all(iotlb, &cmd.real_inval);
		goto out;
	}

	if (ril_mode == KUNIT_ARMV8_RIL_SINGLE) {
		/*
		 * Produce a single invalidation by rounding up and disabling
		 * the trailer.
		 */
		first.num = DIV_ROUND_UP_ULL(num_tg, 1ULL << first.scale);
		trail = (struct kunit_armv8_ril_range){};
	} else {
		/*
		 * Produce two invalidations by rounding down and adding a
		 * second trailing RIL anchored at the end.
		 */
		first.num = num_tg >> first.scale;
		trail = armv8_ril_init_end(last_tg,
					   num_tg - (first.num << first.scale));
	}
	armv8_add_ril(test, iotlb, &cmd, &first, ttl, tgsz_lg2);

	if (trail.num) {
		if (ril_mode == KUNIT_ARMV8_RIL_CONT_ERRATA)
			armv8_cont_errata(priv, gather, &first, &trail, &cmd,
					  ttl, tgsz_lg2);
		armv8_add_ril(test, iotlb, &cmd, &trail, ttl, tgsz_lg2);
	}
out:
	kunit_iotlb_unmap_fixup(priv, &cmd.real_inval, gather);
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
 * IOTLB Invalidate
 * PASID-based IOTLB Invalidate Descriptor (P_IOTLB)
 */
struct kunit_vtd_iotlb_inval {
	u64 address;
	/*
	 * Address Mask (AM): number of low-order ADDR bits above bit 12 to
	 * mask for the invalidation. AM=0 means single 4K page. The
	 * invalidated byte range is [address, address + (4K << am) - 1].
	 * Spec Table 19.
	 */
	unsigned int am;
	/*
	 * Invalidation Hint (IH):
	 *  false (IH=0): Invalidate paging-structure-cache entries in range
	 *  true  (IH=1): Preserve paging-structure-cache entries
	 */
	bool ih;

	struct kunit_iotlb_range real_inval;
};

/*
 * The VT-d spec is hard to read here, but consensus is these sections support
 * the idea that VT-d is the opposite of AMD and removes all the paging
 * structure, not just the structure enclosed by the range.
 *
 * 6.5.3.3 Guidance to Software for Invalidations:
 *
 *   - If software modifies a paging-structure entry that references another
 *     paging structure, it may use one of the following approaches depending
 *     upon the type and number of translations controlled by the modified
 *     entry:
 *
 *     - Execute page-selective type of IOTLB invalidation command for any
 *       addresses with each of the page numbers with translations that will use
 *       the entry. These invalidations must specify an Invalidation Hint (IH)
 *       value of 0 (so that it invalidates the paging-structure caches).
 *       However, if no page numbers that will use the entry have translations
 *       (e.g., because the P flags are 0 in all entries in the paging structure
 *       referenced by the modified entry), it remains necessary to execute the
 *       page-selective type of IOTLB invalidation command at least once.
 *
 * Jann says this interpretation matches the Intel CPU TLBI behavior.
 */
static void kunit_iotlb_vtd_cmd(struct kunit *test,
				struct kunit_iommu_inv_iotlb *iotlb,
				struct kunit_vtd_iotlb_inval *cmd)
{
	unsigned int sz_lg2 = 12 + cmd->am;
	u64 last = log2_set_mod_max_t(u64, cmd->address, sz_lg2);
	u64 start = cmd->address;

	KUNIT_ASSERT_EQ(test, log2_mod_t(u64, start, sz_lg2), 0);
	KUNIT_ASSERT_LE(test, start, last);

	if (cmd->am >= 52) {
		kunit_iotlb_erase_all(iotlb, &cmd->real_inval);
		return;
	}

	kunit_iotlb_erase_range(iotlb, start, last, U64_MAX,
				cmd->ih ? 0 : U64_MAX, &cmd->real_inval);
}

static void kunit_iotlb_range_gather_vtd(struct kunit_iommu_priv *priv,
					 struct iommu_iotlb_gather *gather)
{
	struct kunit *test = priv->test;
	struct kunit_iommu_inv_iotlb *iotlb = priv->iotlb;
	struct kunit_vtd_iotlb_inval cmd = {
		.ih = iommu_pages_list_empty(&gather->freelist),
		.real_inval = IOTLB_RANGE_INIT,
	};
	unsigned int sz_lg2;
	u64 cmd_last;

	/*
	 * VT-d PSI uses Address + AM to specify a power-of-two aligned range
	 * of 4K pages. Compute the smallest such range covering [start, end].
	 *
	 * This is equivalent to calculate_psi_aligned_address() in
	 * drivers/iommu/intel/cache.c: both find the smallest power-of-two
	 * aligned region covering the gather range.
	 */
	sz_lg2 = fls_t(unsigned long, gather->start ^ gather->end);
	if (sz_lg2 < 12)
		sz_lg2 = 12;
	cmd.am = sz_lg2 - 12;
	cmd.address = log2_set_mod_t(unsigned long, gather->start, 0, sz_lg2);
	cmd_last = log2_set_mod_max_t(u64, cmd.address, sz_lg2);

	KUNIT_ASSERT_LE(test, cmd.address, gather->start);
	KUNIT_ASSERT_GE(test, cmd_last, gather->end);
	kunit_iotlb_vtd_cmd(test, iotlb, &cmd);
	kunit_iotlb_unmap_fixup(priv, &cmd.real_inval, gather);
}

/*
 * RISC-V IOMMU IOTINVAL.VMA/GVMA command
 * GV/PSCV are used to select the address space and can be ignored for this test
 */
struct kunit_riscv_inval {
	u64 addr;
	bool av;
	/* Non-leaf PTE Invalidation Extension, v1.0.1 Section 9.2 */
	bool nl;
	/* Address Range Invalidation Extension, v1.0.1 Section 9.3 */
	unsigned int sz_lg2;
	struct kunit_iotlb_range real_inval;
};

static void kunit_iotlb_riscv_cmd(struct kunit *test,
				  struct kunit_iommu_inv_iotlb *iotlb,
				  struct kunit_riscv_inval *cmd)
{
	if (!cmd->av) {
		kunit_iotlb_erase_all(iotlb, &cmd->real_inval);
		return;
	}

	/*
	 * Without the S extension (sz_lg2 == 0) only a single address is
	 * given.
	 *
	 * The meaning of the address is not clearly specified in the spec,
	 * assume the intention is to invalidate any IOTLB entry that contains
	 * addr, or contains any part of the range.
	 */
	kunit_iotlb_erase_range(iotlb, cmd->addr,
				log2_set_mod_max_t(u64, cmd->addr, cmd->sz_lg2),
				U64_MAX, cmd->nl ? U64_MAX : 0,
				&cmd->real_inval);
}

static void kunit_iotlb_range_gather_riscv(struct kunit_iommu_priv *priv,
					   struct iommu_iotlb_gather *gather)
{
	struct kunit *test = priv->test;
	struct kunit_iommu_inv_iotlb *iotlb = priv->iotlb;
	struct kunit_riscv_inval cmd = {
		.real_inval = IOTLB_RANGE_INIT,
	};

	if (!iommu_pages_list_empty(&gather->freelist) ||
	    (gather->end - gather->start) >= SZ_2M - 1) {
		kunit_iotlb_riscv_cmd(test, iotlb, &cmd);
	} else {
		u64 iova = gather->start;

		cmd.av = true;
		do {
			cmd.addr = iova;
			kunit_iotlb_riscv_cmd(test, iotlb, &cmd);
		} while (!check_add_overflow(
				 iova, log2_to_int(priv->smallest_pgsz_lg2),
				 &iova) &&
			 iova <= gather->end);
	}
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
	case KUNIT_IOMMU_MODEL_ARMV8:
		if (pt_feature(priv->common, PT_FEAT_ARMV8_DBM))
			kunit_iotlb_range_gather_armv8(priv, gather,
						       KUNIT_ARMV8_RIL_SINGLE);
		else if (pt_feature(priv->common, PT_FEAT_ARMV8_TTBR1))
			kunit_iotlb_range_gather_armv8(
				priv, gather, KUNIT_ARMV8_RIL_CONT_ERRATA);
		else
			kunit_iotlb_range_gather_armv8(priv, gather,
						       KUNIT_ARMV8_RIL_NORMAL);
		break;
	case KUNIT_IOMMU_MODEL_RISCV:
		kunit_iotlb_range_gather_riscv(priv, gather);
		break;
	case KUNIT_IOMMU_MODEL_VTD:
		kunit_iotlb_range_gather_vtd(priv, gather);
		break;
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

	if (strstr(format_name, "armv8"))
		priv->iotlb->model = KUNIT_IOMMU_MODEL_ARMV8;
	else if (strstr(format_name, "amd"))
		priv->iotlb->model = KUNIT_IOMMU_MODEL_AMD;
	else if (strstr(format_name, "vtdss"))
		priv->iotlb->model = KUNIT_IOMMU_MODEL_VTD;
	else if (strstr(format_name, "riscv"))
		priv->iotlb->model = KUNIT_IOMMU_MODEL_RISCV;

	for (i = 0; i < PT_VADDR_MAX_LG2; i++) {
		xa_init(&priv->iotlb->leaf_entries[i]);
		xa_init(&priv->iotlb->table_entries[i]);
	}
}

#endif
