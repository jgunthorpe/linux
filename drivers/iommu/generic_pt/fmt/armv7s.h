/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 *
 * ARMv7 Short-descriptor format. This is described by the ARMv8 VMSAv8-32
 * Short-descriptor chapter in the Architecture Reference Manual.
 *
 * NOTE! The level numbering is consistent with the Generic Page Table API, but
 * is backwards from what the ARM documents use. What ARM calls level 2 this
 * calls level 0.
 *
 * This was called io-pgtable-armv7s.c and ARM_V7S
 *
 * FIXME:
 * - mtk encoding
 */
#ifndef __GENERIC_PT_FMT_ARMV7S_H
#define __GENERIC_PT_FMT_ARMV7S_H

#include "defs_armv7s.h"
#include "../pt_defs.h"

#include <linux/bitfield.h>
#include <linux/container_of.h>
#include <linux/gfp_types.h>
#include <linux/log2.h>

enum {
	PT_MAX_OUTPUT_ADDRESS_LG2 = 40,
	PT_MAX_VA_ADDRESS_LG2 = 32,
	PT_ENTRY_WORD_SIZE = sizeof(u32),
	PT_MAX_TOP_LEVEL = 1,
	PT_GRANULE_LG2SZ = 12,
	PT_TABLEMEM_LG2SZ = 10,
};

#define PT_FIXED_TOP_LEVEL PT_MAX_TOP_LEVEL

enum {
	ARMV7S_PT_FMT_TYPE = GENMASK(1, 0),
};

/* Top most level (ARM Level 1, pts level 1) */
enum {
	/* Translation Table */
	ARMV7S_PT_FMT1_TTB = GENMASK(31, 10),

	ARMV7S_PT_FMT1_B = BIT(2),
	ARMV7S_PT_FMT1_C = BIT(3),
	ARMV7S_PT_FMT1_XN = BIT(4),
	ARMV7S_PT_FMT1_AP0 = BIT(10),
	ARMV7S_PT_FMT1_AP1 = BIT(11),
	ARMV7S_PT_FMT1_TEX = GENMASK(14, 12),
	ARMV7S_PT_FMT1_AP2 = BIT(15),
	ARMV7S_PT_FMT1_S = BIT(16),
	ARMV7S_PT_FMT1_NG = BIT(17),
	ARMV7S_PT_FMT1_NS = BIT(19),

	/* Section */
	ARMV7S_PT_FMT1S_OA = GENMASK(31, 20),

	/* Supersection */
	ARMV7S_PT_FMT1SS_OA_C = GENMASK(8, 5),
	ARMV7S_PT_FMT1_SUPER_SECTION = BIT(18),
	ARMV7S_PT_FMT1SS_OA_B = GENMASK(23, 20),
	ARMV7S_PT_FMT1SS_OA_A = GENMASK(31, 24),
};

enum {
	ARMV7S_PT_FMT1_TYPE_TABLE = 1,
	/* PXN is not supported */
	ARMV7S_PT_FMT1_TYPE_SECTION = 2,
};

/* Lowest level (ARM Level 2, pts level 0) */
enum {
	ARMV7S_PT_FMT2_SMALL_PAGE = BIT(1),
	ARMV7S_PT_FMT2_B = BIT(2),
	ARMV7S_PT_FMT2_C = BIT(3),
	ARMV7S_PT_FMT2_AP0 = BIT(4),
	ARMV7S_PT_FMT2_AP1 = BIT(5),
	ARMV7S_PT_FMT2_AP2 = BIT(9),
	ARMV7S_PT_FMT2_S = BIT(10),
	ARMV7S_PT_FMT2_NG = BIT(11),

	/* Small Page */
	ARMV7S_PT_FMT2S_XN = BIT(0),
	ARMV7S_PT_FMT2S_TEX = GENMASK(8, 6),
	ARMV7S_PT_FMT2S_OA = GENMASK(31, 12),

	/* Large Page */
	ARMV7S_PT_FMT2L_XN = BIT(15),
	ARMV7S_PT_FMT2L_TEX = GENMASK(14, 12),
	ARMV7S_PT_FMT2L_OA = GENMASK(31, 16),
};

enum {
	ARMV7S_PT_FMT2_TYPE_LARGE_PAGE = 1,
	ARMV7S_PT_FMT2_TYPE_SMALL_PAGE = 2,
};

/* Table pointer bits */
enum {
	ARMV7S_PT_FMT_TABLE_NS = BIT(3),
};

#define common_to_armv7s_pt(common_ptr) \
	container_of_const(common_ptr, struct pt_armv7s, common)
#define to_armv7s_pt(pts) common_to_armv7s_pt((pts)->range->common)

static inline pt_oaddr_t armv7s_pt_table_pa(const struct pt_state *pts)
{
	return oalog2_mul(FIELD_GET(ARMV7S_PT_FMT1_TTB, pts->entry),
			PT_TABLEMEM_LG2SZ);
}
#define pt_table_pa armv7s_pt_table_pa

/* Returns the oa for the start of the contiguous entry */
static inline pt_oaddr_t armv7s_pt_entry_oa(const struct pt_state *pts)
{
	if (pts->level == 0) {
		if (pts->entry & ARMV7S_PT_FMT2_SMALL_PAGE)
			return oalog2_mul(FIELD_GET(ARMV7S_PT_FMT2S_OA,
						  pts->entry),
					PT_GRANULE_LG2SZ);
		return oalog2_mul(FIELD_GET(ARMV7S_PT_FMT2L_OA, pts->entry), 16);
	}
	if (pts->entry & ARMV7S_PT_FMT1_SUPER_SECTION)
		return oalog2_mul(FIELD_GET(ARMV7S_PT_FMT1SS_OA_A, pts->entry),
				24) |
		       oalog2_mul(FIELD_GET(ARMV7S_PT_FMT1SS_OA_B, pts->entry),
				32) |
		       oalog2_mul(FIELD_GET(ARMV7S_PT_FMT1SS_OA_C, pts->entry),
				36);
	return oalog2_mul(FIELD_GET(ARMV7S_PT_FMT1S_OA, pts->entry), 20);
}
#define pt_entry_oa armv7s_pt_entry_oa

static inline bool armv7s_pt_can_have_leaf(const struct pt_state *pts)
{
	return true;
}
#define pt_can_have_leaf armv7s_pt_can_have_leaf

static inline unsigned int
armv7s_pt_table_item_lg2sz(const struct pt_state *pts)
{
	return PT_GRANULE_LG2SZ +
	       (PT_TABLEMEM_LG2SZ - ilog2(sizeof(u32))) * pts->level;
}
#define pt_table_item_lg2sz armv7s_pt_table_item_lg2sz

static inline unsigned short
armv7s_pt_contig_count_lg2(const struct pt_state *pts)
{
	return ilog2(16);
}
#define pt_contig_count_lg2 armv7s_pt_contig_count_lg2

static inline unsigned int
armv7s_pt_entry_num_contig_lg2(const struct pt_state *pts)
{
	if ((pts->level == 0 && !(pts->entry & ARMV7S_PT_FMT2_SMALL_PAGE)) ||
	    (pts->level != 0 && pts->entry & ARMV7S_PT_FMT1_SUPER_SECTION))
		return armv7s_pt_contig_count_lg2(pts);
	return ilog2(1);
}
#define pt_entry_num_contig_lg2 armv7s_pt_entry_num_contig_lg2

static inline pt_vaddr_t armv7s_pt_full_va_prefix(const struct pt_common *common)
{
	if (pt_feature(common, PT_FEAT_ARMV7S_TTBR1))
		return PT_VADDR_MAX;
	return 0;
}
#define pt_full_va_prefix armv7s_pt_full_va_prefix

/* Number of indexes in the current table level */
static inline unsigned int armv7s_pt_num_items_lg2(const struct pt_state *pts)
{
	/* if (pts->level == 1)
		return 12;
	*/
	return PT_TABLEMEM_LG2SZ - ilog2(sizeof(u32));
}
#define pt_num_items_lg2 armv7s_pt_num_items_lg2

static inline enum pt_entry_type armv7s_pt_load_entry_raw(struct pt_state *pts)
{
	const u32 *tablep = pt_cur_table(pts, u32);
	unsigned int type;
	u32 entry;

	pts->entry = entry = READ_ONCE(tablep[pts->index]);
	type = FIELD_GET(ARMV7S_PT_FMT_TYPE, entry);
	if (type == 0)
		return PT_ENTRY_EMPTY;
	if (pts->level == 1 && type == ARMV7S_PT_FMT1_TYPE_TABLE)
		return PT_ENTRY_TABLE;
	return PT_ENTRY_OA;
}
#define pt_load_entry_raw armv7s_pt_load_entry_raw

static inline void
armv7s_pt_install_leaf_entry(struct pt_state *pts, pt_oaddr_t oa,
			     unsigned int oasz_lg2,
			     const struct pt_write_attrs *attrs)
{
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);
	u32 *tablep = pt_cur_table(pts, u32);
	u32 entry = 0;

	PT_WARN_ON(oalog2_mod(oa, oasz_lg2));
	tablep += pts->index;

	if (oasz_lg2 == isz_lg2) {
		if (pts->level == 0)
			entry = FIELD_PREP(ARMV7S_PT_FMT_TYPE,
					   ARMV7S_PT_FMT2_TYPE_SMALL_PAGE) |
				FIELD_PREP(ARMV7S_PT_FMT2S_OA,
					   oalog2_div(oa, PT_GRANULE_LG2SZ)) |
				attrs->pte2;
		else
			entry = FIELD_PREP(ARMV7S_PT_FMT_TYPE,
					   ARMV7S_PT_FMT1_TYPE_SECTION) |
				FIELD_PREP(ARMV7S_PT_FMT1S_OA,
					   oalog2_div(oa, 20)) |
				attrs->pte1;
		WRITE_ONCE(*tablep, entry);
	} else {
		u32 *end;

		if (pts->level == 0)
			entry = FIELD_PREP(ARMV7S_PT_FMT_TYPE,
					   ARMV7S_PT_FMT2_TYPE_LARGE_PAGE) |
				FIELD_PREP(ARMV7S_PT_FMT2L_OA,
					   oalog2_div(oa, 16)) |
				attrs->pte2l;
		else
			entry = FIELD_PREP(ARMV7S_PT_FMT_TYPE,
					   ARMV7S_PT_FMT1_TYPE_SECTION) |
				ARMV7S_PT_FMT1_SUPER_SECTION |
				FIELD_PREP(ARMV7S_PT_FMT1SS_OA_A,
					   oalog2_div(oa, 24)) |
				FIELD_PREP(ARMV7S_PT_FMT1SS_OA_B,
					   oalog2_div(oa, 32)) |
				FIELD_PREP(ARMV7S_PT_FMT1SS_OA_C,
					   oalog2_div(oa, 36)) |
				attrs->pte1;

		PT_WARN_ON(oasz_lg2 !=
			   isz_lg2 + armv7s_pt_contig_count_lg2(pts));
		PT_WARN_ON(
			log2_mod(pts->index, armv7s_pt_contig_count_lg2(pts)));

		end = tablep + log2_to_int(armv7s_pt_contig_count_lg2(pts));
		for (; tablep != end; tablep++)
			WRITE_ONCE(*tablep, entry);
	}
	pts->entry = entry;
}
#define pt_install_leaf_entry armv7s_pt_install_leaf_entry

static inline bool armv7s_pt_install_table(struct pt_state *pts,
					   pt_oaddr_t table_pa,
					   const struct pt_write_attrs *attrs)
{
	u32 *tablep = pt_cur_table(pts, u32);
	u32 entry;

	entry = FIELD_PREP(ARMV7S_PT_FMT_TYPE, ARMV7S_PT_FMT1_TYPE_TABLE) |
		FIELD_PREP(ARMV7S_PT_FMT1_TTB,
			   oalog2_div(table_pa, PT_TABLEMEM_LG2SZ));

	if (pts_feature(pts, PT_FEAT_ARMV7S_NS))
		entry |= ARMV7S_PT_FMT_TABLE_NS;

	return pt_table_install32(&tablep[pts->index], entry, pts->entry);
}
#define pt_install_table armv7s_pt_install_table

/*
 * Trivial translation of the different bit assignments. pt_attr_from_entry() is
 * not a performance path to justify something more optimized.
 */
#define _COPY_PTE_MASK(mask, l1, l2, l2l)                         \
	{                                                         \
		attrs->pte1 |= FIELD_PREP(l1, FIELD_GET(mask, entry));   \
		attrs->pte2 |= FIELD_PREP(l2, FIELD_GET(mask, entry));   \
		attrs->pte2l |= FIELD_PREP(l2l, FIELD_GET(mask, entry)); \
	}
#define COPY_PTE_MASK(name, entry, l1, l2, l2l)                             \
	_COPY_PTE_MASK(ARMV7S_PT_##entry##_##name, ARMV7S_PT_##l1##_##name, \
		       ARMV7S_PT_##l2##_##name, ARMV7S_PT_##l2l##_##name)

static inline void armv7s_pt_attr_from_pte1(u32 entry,
					    struct pt_write_attrs *attrs)
{
	COPY_PTE_MASK(NG, FMT1, FMT1, FMT2, FMT2);
	COPY_PTE_MASK(S, FMT1, FMT1, FMT2, FMT2);
	COPY_PTE_MASK(TEX, FMT1, FMT1, FMT2S, FMT2L);
	COPY_PTE_MASK(AP0, FMT1, FMT1, FMT2, FMT2);
	COPY_PTE_MASK(AP1, FMT1, FMT1, FMT2, FMT2);
	COPY_PTE_MASK(AP2, FMT1, FMT1, FMT2, FMT2);
	COPY_PTE_MASK(XN, FMT1, FMT1, FMT2S, FMT2L);
	COPY_PTE_MASK(B, FMT1, FMT1, FMT2, FMT2);
	COPY_PTE_MASK(C, FMT1, FMT1, FMT2, FMT2);
}

static inline void armv7s_pt_attr_from_pte2(u32 entry,
					     struct pt_write_attrs *attrs)
{
	COPY_PTE_MASK(NG, FMT2, FMT1, FMT2, FMT2);
	COPY_PTE_MASK(S, FMT2, FMT1, FMT2, FMT2);
	COPY_PTE_MASK(AP0, FMT2, FMT1, FMT2, FMT2);
	COPY_PTE_MASK(AP1, FMT2, FMT1, FMT2, FMT2);
	COPY_PTE_MASK(AP2, FMT2, FMT1, FMT2, FMT2);
	COPY_PTE_MASK(B, FMT2, FMT1, FMT2, FMT2);
	COPY_PTE_MASK(C, FMT2, FMT1, FMT2, FMT2);
}

static inline void armv7s_pt_attr_from_pte2s(u32 entry,
					     struct pt_write_attrs *attrs)
{
	COPY_PTE_MASK(TEX, FMT2S, FMT1, FMT2S, FMT2L);
	COPY_PTE_MASK(XN, FMT2S, FMT1, FMT2S, FMT2L);
}

static inline void armv7s_pt_attr_from_pte2l(u32 entry,
					     struct pt_write_attrs *attrs)
{
	COPY_PTE_MASK(TEX, FMT2L, FMT1, FMT2S, FMT2L);
	COPY_PTE_MASK(XN, FMT2L, FMT1, FMT2S, FMT2L);
}
#undef _COPY_PTE_MASK
#undef COPY_PTE_MASK

static inline void armv7s_pt_attr_from_entry(const struct pt_state *pts,
					     struct pt_write_attrs *attrs)
{
	attrs->pte1 = 0;
	attrs->pte2 = 0;
	attrs->pte2l = 0;
	if (pts->level == 0) {
		armv7s_pt_attr_from_pte2(pts->entry, attrs);
		if (pts->entry & ARMV7S_PT_FMT2_SMALL_PAGE)
			armv7s_pt_attr_from_pte2s(pts->entry, attrs);
		else
			armv7s_pt_attr_from_pte2l(pts->entry, attrs);
	} else {
		armv7s_pt_attr_from_pte1(pts->entry, attrs);
	}

	if (pts_feature(pts, PT_FEAT_ARMV7S_NS))
		attrs->pte1 |= ARMV7S_PT_FMT1_NS;
}
#define pt_attr_from_entry armv7s_pt_attr_from_entry

/* --- iommu */
#include <linux/generic_pt/iommu.h>
#include <linux/iommu.h>

#define pt_iommu_table pt_iommu_armv7s

/* The common struct is in the per-format common struct */
static inline struct pt_common *common_from_iommu(struct pt_iommu *iommu_table)
{
	return &container_of(iommu_table, struct pt_iommu_table, iommu)
			->armpt.common;
}

static inline struct pt_iommu *iommu_from_common(struct pt_common *common)
{
	return &container_of(common, struct pt_iommu_table, armpt.common)
			->iommu;
}

/*
 * There are three encodings of the PTE bits. We compute each of the three and
 * store them in the pt_write_attrs. install will use the right one.
 */
#define _SET_PTE_MASK(l1, l2, l2l, val)        \
	({                                     \
		pte1 |= FIELD_PREP(l1, val);   \
		pte2 |= FIELD_PREP(l2, val);   \
		pte2l |= FIELD_PREP(l2l, val); \
	})
#define SET_PTE_MASK(name, l1, l2, l2l, val)                            \
	_SET_PTE_MASK(ARMV7S_PT_##l1##_##name, ARMV7S_PT_##l2##_##name, \
		      ARMV7S_PT_##l2l##_##name, val)

static inline int armv7s_pt_iommu_set_prot(struct pt_common *common,
					   struct pt_write_attrs *attrs,
					   unsigned int iommu_prot)
{
	bool ap = true; // FIXME IO_PGTABLE_QUIRK_NO_PERMS
	u32 pte1 = 0;
	u32 pte2 = 0;
	u32 pte2l = 0;

	SET_PTE_MASK(NG, FMT1, FMT2, FMT2, 1);
	SET_PTE_MASK(S, FMT1, FMT2, FMT2, 1);

	if (!(iommu_prot & IOMMU_MMIO))
		SET_PTE_MASK(TEX, FMT1, FMT2S, FMT2L, 1);

	/*
	 * Simplified access permissions: AF = AP0, UNPRIV = AP1, RDONLY = AP2
	 */
	if (ap) {
		/* AF */
		SET_PTE_MASK(AP0, FMT1, FMT2, FMT2, 1);
		if (!(iommu_prot & IOMMU_PRIV))
			SET_PTE_MASK(AP1, FMT1, FMT2, FMT2, 1);
		if (!(iommu_prot & IOMMU_WRITE))
			SET_PTE_MASK(AP2, FMT1, FMT2, FMT2, 1);
	}

	if ((iommu_prot & IOMMU_NOEXEC) && ap)
		SET_PTE_MASK(XN, FMT1, FMT2S, FMT2L, 1);

	if (iommu_prot & IOMMU_MMIO) {
		SET_PTE_MASK(B, FMT1, FMT2, FMT2, 1);
	} else if (iommu_prot & IOMMU_CACHE) {
		SET_PTE_MASK(B, FMT1, FMT2, FMT2, 1);
		SET_PTE_MASK(C, FMT1, FMT2, FMT2, 1);
	}

	if (pt_feature(common, PT_FEAT_ARMV7S_NS))
		pte1 |= ARMV7S_PT_FMT1_NS;

	attrs->pte1 = pte1;
	attrs->pte2 = pte2;
	attrs->pte2l = pte2l;

	/*
	 * ARMV7S_PT_FMT1_TTB can only hold a 32 bit pointer so we must use
	 * DMA32 to allocate table pointers.
	 */
	attrs->gfp |= GFP_DMA32;

	return 0;
}
#define pt_iommu_set_prot armv7s_pt_iommu_set_prot
#undef _SET_PTE_MASK
#undef SET_PTE_MASK

static inline int
armv7s_pt_iommu_fmt_init(struct pt_iommu_armv7s *iommu_table,
			 const struct pt_iommu_armv7s_cfg *cfg)
{
	return 0;
}
#define pt_iommu_fmt_init armv7s_pt_iommu_fmt_init

#if defined(GENERIC_PT_KUNIT)
static const struct pt_iommu_armv7s_cfg armv7s_kunit_fmt_cfgs[] = {
	[0] = { .common.features = 0, .common.hw_max_oasz_lg2 = 32 },
	[1] = { .common.features = BIT(PT_FEAT_ARMV7S_NS), .common.hw_max_oasz_lg2 = 32 },
	[2] = { .common.features = BIT(PT_FEAT_ARMV7S_TTBR1), },
};
#define kunit_fmt_cfgs armv7s_kunit_fmt_cfgs
enum {
	KUNIT_FMT_FEATURES = BIT(PT_FEAT_ARMV7S_TTBR1) | BIT(PT_FEAT_ARMV7S_NS)
};
#endif

#if defined(GENERIC_PT_KUNIT) && IS_ENABLED(CONFIG_IOMMU_IO_PGTABLE_ARMV7S)
#include <linux/io-pgtable.h>

static struct io_pgtable_ops *
armv7s_pt_iommu_alloc_io_pgtable(struct pt_iommu_armv7s_cfg *cfg,
			       struct device *iommu_dev,
			       struct io_pgtable_cfg **unused_pgtbl_cfg)
{
	struct io_pgtable_cfg pgtbl_cfg = {};

	if (cfg->common.features & BIT(PT_FEAT_ARMV7S_TTBR1) ||
	    cfg->common.hw_max_oasz_lg2 != 32)
		return ERR_PTR(-EOPNOTSUPP);

	pgtbl_cfg.ias = PT_MAX_VA_ADDRESS_LG2;
	pgtbl_cfg.oas = cfg->common.hw_max_oasz_lg2;
	pgtbl_cfg.pgsize_bitmap |= SZ_4K;
	pgtbl_cfg.coherent_walk = true;
	if (cfg->common.features & BIT(PT_FEAT_ARMV7S_NS))
		pgtbl_cfg.quirks |= IO_PGTABLE_QUIRK_ARM_NS;

	return alloc_io_pgtable_ops(ARM_V7S, &pgtbl_cfg, NULL);
}
#define pt_iommu_alloc_io_pgtable armv7s_pt_iommu_alloc_io_pgtable

static void armv7s_pt_iommu_setup_ref_table(struct pt_iommu_armv7s *iommu_table,
					  struct io_pgtable_ops *pgtbl_ops)
{
	struct io_pgtable_cfg *pgtbl_cfg =
		&io_pgtable_ops_to_pgtable(pgtbl_ops)->cfg;
	struct pt_common *common = &iommu_table->armpt.common;

	pt_top_set(common,
		   __va(log2_set_mod_t(u32, pgtbl_cfg->arm_v7s_cfg.ttbr, 0, 7)),
		   PT_FIXED_TOP_LEVEL);
}
#define pt_iommu_setup_ref_table armv7s_pt_iommu_setup_ref_table

static u64 armv7s_pt_kunit_cmp_mask_entry(struct pt_state *pts)
{
	if (pts->type == PT_ENTRY_TABLE)
		return pts->entry & (~(u32)(ARMV7S_PT_FMT1_TTB));
	return pts->entry;
}
#define pt_kunit_cmp_mask_entry armv7s_pt_kunit_cmp_mask_entry
#endif

#endif
