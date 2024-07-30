/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 *
 * DART Page Table
 *
 * This is derived from io-pgtable-dart.c
 *
 * Use a three level structure:
 *  L1 - 0
 *  L2 - 1
 *  PGD/TTBR's - 2
 *
 * The latter level is folded into some other datastructure, in the
 * io-pgtable-dart implementation this was a naked array, but here we make it a
 * full level.
 *
 * FIXME: is it a mistake to put v1 and v2 into the same file? They seem quite
 * different and if v1 is always a 4k granule and v2 always 16k it would make
 * sense to split them.
 *
 * FIXME: core code should prepopulate the level 2 table
 */
#ifndef __GENERIC_PT_FMT_DART_H
#define __GENERIC_PT_FMT_DART_H

#include "defs_dart.h"
#include "../pt_defs.h"

#include <linux/bitfield.h>
#include <linux/container_of.h>
#include <linux/log2.h>

enum {
	PT_ENTRY_WORD_SIZE = sizeof(u64),
	PT_MAX_TOP_LEVEL = 2,
	DART_NUM_TTBRS_LG2 = ilog2(4),
	/*
	 * This depends on dartv1/v2 and the granule size. max_vasz_lg2 has the
	 * right value
	 */
	PT_MAX_VA_ADDRESS_LG2 = 44,
};

enum {
	DART_FMT_VALID = BIT(0),
	DART_FMT_PTE_SUBPAGE_START = GENMASK_ULL(63, 52),
	DART_FMT_PTE_SUBPAGE_END = GENMASK_ULL(51, 40),
};

/* DART v1 PTE layout */
enum {
	DART_FMT1_PTE_PROT_SP_DIS = BIT(1),
	DART_FMT1_PTE_PROT_NO_WRITE = BIT(7),
	DART_FMT1_PTE_PROT_NO_READ = BIT(8),
	DART_FMT1_PTE_OA = GENMASK_ULL(35, 12),
};

/* DART v2 PTE layout */
enum {
	DART_FMT2_PTE_PROT_NO_CACHE = BIT(1),
	DART_FMT2_PTE_PROT_NO_WRITE = BIT(2),
	DART_FMT2_PTE_PROT_NO_READ = BIT(3),
	DART_FMT2_PTE_OA = GENMASK_ULL(37, 10),
};

#define common_to_dartpt(common_ptr) \
	container_of_const(common_ptr, struct pt_dart, common)
#define to_dartpt(pts) common_to_dartpt((pts)->range->common)

static inline unsigned int dartpt_granule_lg2sz(const struct pt_common *common)
{
	const struct pt_dart *dartpt = common_to_dartpt(common);

	return dartpt->granule_lg2sz;
}

static inline pt_oaddr_t dartpt_oa(const struct pt_state *pts)
{
	if (pts_feature(pts, PT_FEAT_DART_V2))
		return log2_mul(FIELD_GET(DART_FMT2_PTE_OA, pts->entry), 14);
	return log2_mul(FIELD_GET(DART_FMT1_PTE_OA, pts->entry), 12);
}

static inline u64 dartpt_make_oa(const struct pt_state *pts, pt_oaddr_t oa)
{
	if (pts_feature(pts, PT_FEAT_DART_V2))
		return FIELD_PREP(DART_FMT2_PTE_OA, log2_div(oa, 14));
	return FIELD_PREP(DART_FMT1_PTE_OA, log2_div(oa, 12));
}

static inline unsigned int
dartpt_max_output_address_lg2(const struct pt_common *common)
{
	/* Width of the OA field plus the pfn size */
	if (pt_feature(common, PT_FEAT_DART_V2))
		return (37 - 10 + 1) + 14;
	return (35 - 12 + 1) + 12;
}
#define pt_max_output_address_lg2 dartpt_max_output_address_lg2

static inline pt_oaddr_t dartpt_table_pa(const struct pt_state *pts)
{
	return dartpt_oa(pts);
}
#define pt_table_pa dartpt_table_pa

static inline pt_oaddr_t dartpt_entry_oa(const struct pt_state *pts)
{
	return dartpt_oa(pts);
}
#define pt_entry_oa dartpt_entry_oa

static inline bool dartpt_can_have_leaf(const struct pt_state *pts)
{
	return pts->level == 0;
}
#define pt_can_have_leaf dartpt_can_have_leaf

static inline unsigned int dartpt_table_item_lg2sz(const struct pt_state *pts)
{
	unsigned int granule_lg2sz = dartpt_granule_lg2sz(pts->range->common);

	return granule_lg2sz +
	       (granule_lg2sz - ilog2(sizeof(u64))) * pts->level;
}
#define pt_table_item_lg2sz dartpt_table_item_lg2sz

static inline unsigned int dartpt_num_items_lg2(const struct pt_state *pts)
{
	/* Level 3 is the TTBRs
	if (pts->level == 3)
		return DART_NUM_TTBRS_LG2;
	*/
	return dartpt_granule_lg2sz(pts->range->common) - ilog2(sizeof(u64));
}
#define pt_num_items_lg2 dartpt_num_items_lg2

static inline enum pt_entry_type dartpt_load_entry_raw(struct pt_state *pts)
{
	const u64 *tablep = pt_cur_table(pts, u64);
	u64 entry;

	pts->entry = entry = READ_ONCE(tablep[pts->index]);
	if (!(entry & DART_FMT_VALID))
		return PT_ENTRY_EMPTY;
	if (pts->level == 0)
		return PT_ENTRY_OA;
	return PT_ENTRY_TABLE;
}
#define pt_load_entry_raw dartpt_load_entry_raw

static inline void dartpt_install_leaf_entry(struct pt_state *pts,
					     pt_oaddr_t oa,
					     unsigned int oasz_lg2,
					     const struct pt_write_attrs *attrs)
{
	u64 *tablep = pt_cur_table(pts, u64);
	u64 entry;

	entry = DART_FMT_VALID | dartpt_make_oa(pts, oa) |
		attrs->descriptor_bits;
	/* subpage protection: always allow access to the entire page */
	entry |= FIELD_PREP(DART_FMT_PTE_SUBPAGE_START, 0) |
		 FIELD_PREP(DART_FMT_PTE_SUBPAGE_END, 0xfff);

	WRITE_ONCE(tablep[pts->index], entry);
	pts->entry = entry;
}
#define pt_install_leaf_entry dartpt_install_leaf_entry

static inline bool dartpt_install_table(struct pt_state *pts,
					pt_oaddr_t table_pa,
					const struct pt_write_attrs *attrs)
{
	u64 *tablep = pt_cur_table(pts, u64);
	u64 entry;

	entry = DART_FMT_VALID | dartpt_make_oa(pts, table_pa);
	return pt_table_install64(&tablep[pts->index], entry, pts->entry);
}
#define pt_install_table dartpt_install_table

static inline void dartpt_attr_from_entry(const struct pt_state *pts,
					  struct pt_write_attrs *attrs)
{
	if (pts_feature(pts, PT_FEAT_DART_V2))
		attrs->descriptor_bits = pts->entry &
					 (DART_FMT2_PTE_PROT_NO_CACHE |
					  DART_FMT2_PTE_PROT_NO_WRITE |
					  DART_FMT2_PTE_PROT_NO_READ);
	else
		attrs->descriptor_bits = pts->entry &
					 (DART_FMT1_PTE_PROT_SP_DIS |
					  DART_FMT1_PTE_PROT_NO_WRITE |
					  DART_FMT1_PTE_PROT_NO_READ);
}
#define pt_attr_from_entry dartpt_attr_from_entry

/* --- iommu */
#include <linux/generic_pt/iommu.h>
#include <linux/iommu.h>

#define pt_iommu_table pt_iommu_dart

/* The common struct is in the per-format common struct */
static inline struct pt_common *common_from_iommu(struct pt_iommu *iommu_table)
{
	return &container_of(iommu_table, struct pt_iommu_table, iommu)
			->dartpt.common;
}

static inline struct pt_iommu *iommu_from_common(struct pt_common *common)
{
	return &container_of(common, struct pt_iommu_table, dartpt.common)
			->iommu;
}

static inline int dartpt_iommu_set_prot(struct pt_common *common,
					struct pt_write_attrs *attrs,
					unsigned int iommu_prot)
{
	u64 pte = 0;

	if (pt_feature(common, PT_FEAT_DART_V2)) {
		if (!(iommu_prot & IOMMU_WRITE))
			pte |= DART_FMT2_PTE_PROT_NO_WRITE;
		if (!(iommu_prot & IOMMU_READ))
			pte |= DART_FMT2_PTE_PROT_NO_READ;
		if (!(iommu_prot & IOMMU_CACHE))
			pte |= DART_FMT2_PTE_PROT_NO_CACHE;

		/*
		 * FIXME is this a bug in io-pgtable-dart? It uncondtionally
		 * sets DART_FMT1_PTE_PROT_SP_DIS which is called NO_CACHE on
		 * v2
		 */
		pte |= DART_FMT2_PTE_PROT_NO_CACHE;
	} else {
		if (!(iommu_prot & IOMMU_WRITE))
			pte |= DART_FMT1_PTE_PROT_NO_WRITE;
		if (!(iommu_prot & IOMMU_READ))
			pte |= DART_FMT1_PTE_PROT_NO_READ;
		pte |= DART_FMT1_PTE_PROT_SP_DIS;
	}

	attrs->descriptor_bits = pte;
	return 0;
}
#define pt_iommu_set_prot dartpt_iommu_set_prot

static inline int dartpt_iommu_fmt_init(struct pt_iommu_dart *iommu_table,
					const struct pt_iommu_dart_cfg *cfg)
{
	struct pt_dart *table = &iommu_table->dartpt;
	unsigned int vasz_lg2 = cfg->common.hw_max_vasz_lg2;
	unsigned int oasz_lg2 = cfg->common.hw_max_oasz_lg2;
	u64 pgsize_bitmap = cfg->pgsize_bitmap;
	unsigned int l3_num_items_lg2;
	unsigned int l2_va_lg2sz;

	/* The V2 OA requires a 16k page size */
	if (pt_feature(&table->common, PT_FEAT_DART_V2))
		pgsize_bitmap =
			log2_set_mod(cfg->pgsize_bitmap, 0, ilog2(SZ_16K));

	if ((oasz_lg2 != 36 && oasz_lg2 != 42) || vasz_lg2 > oasz_lg2 ||
	    !(pgsize_bitmap & (SZ_4K | SZ_16K)))
		return -EOPNOTSUPP;

	/*
	 * The page size reflects both the size of the tables and the minimum
	 * granule size.
	 */
	table->granule_lg2sz = log2_ffs(pgsize_bitmap);

	/* Size of VA covered by the first two levels */
	l2_va_lg2sz = table->granule_lg2sz +
		      (table->granule_lg2sz - ilog2(sizeof(u64))) * 2;

	table->common.max_vasz_lg2 = vasz_lg2;
	if (vasz_lg2 <= l2_va_lg2sz) {
		/*
		 * Only a single TTBR, don't use the TTBR table, the table_root
		 * pointer will be TTBR[0]
		 */
		l3_num_items_lg2 = ilog2(1);
		pt_top_set_level(&table->common, 1);
	} else {
		l3_num_items_lg2 = vasz_lg2 - l2_va_lg2sz;
		if (l3_num_items_lg2 > DART_NUM_TTBRS_LG2)
			return -EOPNOTSUPP;
		/*
		 * Otherwise the level=3 table holds the TTBR values encoded as
		 * page table entries.
		 */
		pt_top_set_level(&table->common, 2);
	}
	return 0;
}
#define pt_iommu_fmt_init dartpt_iommu_fmt_init

#if defined(GENERIC_PT_KUNIT)
static const struct pt_iommu_dart_cfg dart_kunit_fmt_cfgs[] = {
	[0] = { .common.features = 0,
		.common.hw_max_oasz_lg2 = 36,
		.common.hw_max_vasz_lg2 = 30,
		.pgsize_bitmap = SZ_4K },
	[1] = { .common.features = BIT(PT_FEAT_DART_V2),
		.common.hw_max_oasz_lg2 = 42,
		.common.hw_max_vasz_lg2 = 36,
		.pgsize_bitmap = SZ_16K },
};
#define kunit_fmt_cfgs dart_kunit_fmt_cfgs
enum { KUNIT_FMT_FEATURES = BIT(PT_FEAT_DART_V2) };
#endif

#if defined(GENERIC_PT_KUNIT) && IS_ENABLED(CONFIG_IOMMU_IO_PGTABLE_DART)
#include <linux/io-pgtable.h>

static struct io_pgtable_ops *
dartpt_iommu_alloc_io_pgtable(struct pt_iommu_dart_cfg *cfg,
			      struct device *iommu_dev,
			      struct io_pgtable_cfg **unused_pgtbl_cfg)
{
	struct io_pgtable_cfg pgtbl_cfg = {};
	enum io_pgtable_fmt fmt;

	pgtbl_cfg.ias = cfg->common.hw_max_vasz_lg2;
	pgtbl_cfg.oas = cfg->common.hw_max_oasz_lg2;
	pgtbl_cfg.pgsize_bitmap = cfg->pgsize_bitmap;
	pgtbl_cfg.coherent_walk = true;

	if (cfg->common.features & BIT(PT_FEAT_DART_V2))
		fmt = APPLE_DART2;
	else
		fmt = APPLE_DART;

	return alloc_io_pgtable_ops(fmt, &pgtbl_cfg, NULL);
}
#define pt_iommu_alloc_io_pgtable dartpt_iommu_alloc_io_pgtable

static void dartpt_iommu_setup_ref_table(struct pt_iommu_dart *iommu_table,
					 struct io_pgtable_ops *pgtbl_ops)
{
	struct io_pgtable_cfg *pgtbl_cfg =
		&io_pgtable_ops_to_pgtable(pgtbl_ops)->cfg;
	struct pt_common *common = &iommu_table->dartpt.common;

	/* FIXME should test multi-ttbr tables */
	WARN_ON(pgtbl_cfg->apple_dart_cfg.n_ttbrs != 1);
	pt_top_set(common, __va(pgtbl_cfg->apple_dart_cfg.ttbr[0]), 1);
}
#define pt_iommu_setup_ref_table dartpt_iommu_setup_ref_table

static u64 dartpt_kunit_cmp_mask_entry(struct pt_state *pts)
{
	if (pts->type == PT_ENTRY_TABLE) {
		if (pts_feature(pts, PT_FEAT_DART_V2))
			return pts->entry & (~(u64)DART_FMT2_PTE_OA);
		return pts->entry & (~(u64)DART_FMT1_PTE_OA);
	}
	return pts->entry;
}
#define pt_kunit_cmp_mask_entry dartpt_kunit_cmp_mask_entry
#endif

#endif
