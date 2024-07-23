/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 *
 * The page table format described by the ARMv8 VMSAv8-64 chapter in the
 * Architecture Reference Manual. With the right cfg this will also implement
 * the VMSAv8-32 Long Descriptor format.
 *
 * This was called io-pgtable-arm.c and ARM_xx_LPAE_Sx.
 *
 * NOTE! The level numbering is consistent with the Generic Page Table API, but
 * is backwards from what the ARM documents use. What ARM calls level 3 this
 * calls level 0.
 *
 * Present in io-pgtable-arm.c but not here:
 *    ARM_MALI_LPAE
 *    IO_PGTABLE_QUIRK_ARM_OUTER_WBWA
 */
#ifndef __GENERIC_PT_FMT_ARMV8_H
#define __GENERIC_PT_FMT_ARMV8_H

#include "defs_armv8.h"
#include "../pt_defs.h"

#include <asm/page.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/container_of.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/sizes.h>

#if ARMV8_GRANULE_SIZE == 4096
enum {
	PT_MAX_TOP_LEVEL = 3,
	PT_GRANULE_LG2SZ = 12,
};
#elif ARMV8_GRANULE_SIZE == 16384
enum {
	PT_MAX_TOP_LEVEL = 3,
	PT_GRANULE_LG2SZ = 14,
};
#elif ARMV8_GRANULE_SIZE == 65536
enum {
	PT_MAX_TOP_LEVEL = 2,
	PT_GRANULE_LG2SZ = 16,
};
#else
#error "Invalid ARMV8_GRANULE_SIZE"
#endif

enum {
	PT_MAX_OUTPUT_ADDRESS_LG2 = 48,
	/*
	 * Currently only support up to 48 bits of usable address, the 64k 52
	 * bit mode is not supported.
	 */
	PT_MAX_VA_ADDRESS_LG2 = 48,
	PT_TABLEMEM_LG2SZ = PT_GRANULE_LG2SZ,
	PT_ENTRY_WORD_SIZE = sizeof(u64),
};

/* Common PTE bits */
enum {
	ARMV8PT_FMT_VALID = BIT(0),
	ARMV8PT_FMT_PAGE = BIT(1),
	ARMV8PT_FMT_TABLE = BIT(1),
	ARMV8PT_FMT_NS = BIT(5),
	ARMV8PT_FMT_SH = GENMASK(9, 8),
	ARMV8PT_FMT_AF = BIT(10),

	ARMV8PT_FMT_OA52 = GENMASK_ULL(15, 12),
	ARMV8PT_FMT_OA48 = GENMASK_ULL(47, PT_GRANULE_LG2SZ),

	ARMV8PT_FMT_DBM = BIT_ULL(51),
	ARMV8PT_FMT_CONTIG = BIT_ULL(52),
	ARMV8PT_FMT_UXN = BIT_ULL(53),
	ARMV8PT_FMT_PXN = BIT_ULL(54),
	ARMV8PT_FMT_NSTABLE = BIT_ULL(63),
};

/* S1 PTE bits */
enum {
	ARMV8PT_FMT_ATTRINDX = GENMASK(4, 2),
	ARMV8PT_FMT_AP = GENMASK(7, 6),
	ARMV8PT_FMT_nG = BIT(11),
};

enum {
	ARMV8PT_MAIR_ATTR_IDX_CACHE = 1,
	ARMV8PT_MAIR_ATTR_IDX_DEV = 2,

	ARMV8PT_SH_IS = 3,
	ARMV8PT_SH_OS = 2,

	ARMV8PT_AP_UNPRIV = 1,
	ARMV8PT_AP_RDONLY = 2,
};

/* S2 PTE bits */
enum {
	ARMV8PT_FMT_S2MEMATTR = GENMASK(5, 2),
	ARMV8PT_FMT_S2AP = GENMASK(7, 6),
};

enum {
	/*
	 * For !S2FWB these code to:
	 *  1111 = Normal outer write back cachable / Inner Write Back Cachable
	 *         Permit S1 to override
	 *  0101 = Normal Non-cachable / Inner Non-cachable
	 *  0001 = Device / Device-nGnRE
	 * For S2FWB these code to:
	 *  0110 Force Normal Write Back
	 *  0101 Normal* is forced Normal-NC, Device unchanged
	 *  0001 Force Device-nGnRE
	 */
	ARMV8PT_MEMATTR_FWB_WB = 6,
	ARMV8PT_MEMATTR_OIWB = 0xf,
	ARMV8PT_MEMATTR_NC = 5,
	ARMV8PT_MEMATTR_DEV = 1,

	ARMV8PT_S2AP_READ = 1,
	ARMV8PT_S2AP_WRITE = 2,
};

#define common_to_armv8pt(common_ptr) \
	container_of_const(common_ptr, struct pt_armv8, common)
#define to_armv8pt(pts) common_to_armv8pt((pts)->range->common)

static inline pt_oaddr_t armv8pt_oa(const struct pt_state *pts)
{
	u64 entry = pts->entry;
	pt_oaddr_t oa;

	oa = log2_mul(FIELD_GET(ARMV8PT_FMT_OA48, entry), PT_GRANULE_LG2SZ);

	/* LPA support on 64K page size */
	if (PT_GRANLE_SIZE == SZ_64K)
		oa |= ((pt_oaddr_t)FIELD_GET(ARMV8PT_FMT_OA52, entry)) << 52;
	return oa;
}

static inline pt_oaddr_t armv8pt_table_pa(const struct pt_state *pts)
{
	return armv8pt_oa(pts);
}
#define pt_table_pa armv8pt_table_pa

/*
 * Return a block or page entry pointing at a physical address Returns the
 * address adjusted for the item in a contiguous case.
 */
static inline pt_oaddr_t armv8pt_item_oa(const struct pt_state *pts)
{
	return armv8pt_oa(pts);
}
#define pt_item_oa armv8pt_item_oa

static inline bool armv8pt_can_have_leaf(const struct pt_state *pts)
{
	/*
	 * See D5-18 Translation granule sizes, with block and page sizes, and
	 * output address ranges
	 */
	if ((PT_GRANLE_SIZE == SZ_4K && pts->level > 2) ||
	    (PT_GRANLE_SIZE == SZ_16K && pts->level > 1) ||
	    (PT_GRANLE_SIZE == SZ_64K && pts_feature(pts, PT_FEAT_ARMV8_LPA) && pts->level > 2) ||
	    (PT_GRANLE_SIZE == SZ_64K && !pts_feature(pts, PT_FEAT_ARMV8_LPA) && pts->level > 1))
		return false;
	return true;
}
#define pt_can_have_leaf armv8pt_can_have_leaf

static inline unsigned int armv8pt_table_item_lg2sz(const struct pt_state *pts)
{
	return PT_GRANULE_LG2SZ +
	       (PT_TABLEMEM_LG2SZ - ilog2(sizeof(u64))) * pts->level;
}
#define pt_table_item_lg2sz armv8pt_table_item_lg2sz

/* Number contigous entries that ARMV8PT_FMT_CONTIG will join at this level */
static inline unsigned short
armv8pt_contig_count_lg2(const struct pt_state *pts)
{
	if (PT_GRANLE_SIZE == SZ_4K)
		return ilog2(16); /* 64KB, 2MB */
	else if (PT_GRANLE_SIZE == SZ_16K && pts->level == 1)
		return ilog2(32); /* 1GB */
	else if (PT_GRANLE_SIZE == SZ_16K && pts->level == 0)
		return ilog2(128); /* 2M */
	else if (PT_GRANLE_SIZE == SZ_64K)
		return ilog2(32); /* 2M, 16G */
	return ilog2(1);
}
#define pt_contig_count_lg2 armv8pt_contig_count_lg2

static inline unsigned int
armv8pt_entry_num_contig_lg2(const struct pt_state *pts)
{
	if (pts->entry & ARMV8PT_FMT_CONTIG)
		return armv8pt_contig_count_lg2(pts);
	return ilog2(1);
}
#define pt_entry_num_contig_lg2 armv8pt_entry_num_contig_lg2

static inline pt_vaddr_t armv8pt_full_va_prefix(const struct pt_common *common)
{
	if (pt_feature(common, PT_FEAT_ARMV8_TTBR1))
		return PT_VADDR_MAX;
	return 0;
}
#define pt_full_va_prefix armv8pt_full_va_prefix

static inline unsigned int armv8pt_num_items_lg2(const struct pt_state *pts)
{
	/* FIXME S2 concatenated tables */
	return PT_GRANULE_LG2SZ - ilog2(sizeof(u64));
}
#define pt_num_items_lg2 armv8pt_num_items_lg2

static inline enum pt_entry_type armv8pt_load_entry_raw(struct pt_state *pts)
{
	const u64 *tablep = pt_cur_table(pts, u64);
	u64 entry;

	pts->entry = entry = READ_ONCE(tablep[pts->index]);
	if (!(entry & ARMV8PT_FMT_VALID))
		return PT_ENTRY_EMPTY;
	if (pts->level != 0 && (entry & ARMV8PT_FMT_TABLE))
		return PT_ENTRY_TABLE;

	/*
	 * Suppress returning VALID for levels that cannot have a page to remove
	 * code.
	 */
	if (!armv8pt_can_have_leaf(pts))
		return PT_ENTRY_EMPTY;

	/* Must be a block or page, don't check the page bit on level 0 */
	return PT_ENTRY_OA;
}
#define pt_load_entry_raw armv8pt_load_entry_raw

static inline void
armv8pt_install_leaf_entry(struct pt_state *pts, pt_oaddr_t oa,
			   unsigned int oasz_lg2,
			   const struct pt_write_attrs *attrs)
{
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);
	u64 *tablep = pt_cur_table(pts, u64);
	u64 entry;

	PT_WARN_ON(log2_mod(oa, oasz_lg2));

	entry = ARMV8PT_FMT_VALID |
		FIELD_PREP(ARMV8PT_FMT_OA48, log2_div(oa, PT_GRANULE_LG2SZ)) |
		FIELD_PREP(ARMV8PT_FMT_OA52, oa >> 48) | attrs->descriptor_bits;

	/*
	 * On the last level the leaf is called a page and has the page/table bit set,
	 * on other levels it is called a block and has it clear.
	 */
	if (pts->level == 0)
		entry |= ARMV8PT_FMT_PAGE;

	if (oasz_lg2 != isz_lg2) {
		u64 *end;

		PT_WARN_ON(oasz_lg2 != isz_lg2 + armv8pt_contig_count_lg2(pts));
		PT_WARN_ON(log2_mod(pts->index, armv8pt_contig_count_lg2(pts)));

		entry |= ARMV8PT_FMT_CONTIG;
		tablep += pts->index;
		end = tablep + log2_to_int(armv8pt_contig_count_lg2(pts));
		for (; tablep != end; tablep++) {
			WRITE_ONCE(*tablep, entry);
			entry += FIELD_PREP(
				ARMV8PT_FMT_OA48,
				log2_to_int(isz_lg2 - PT_GRANULE_LG2SZ));
		}
	} else {
		WRITE_ONCE(tablep[pts->index], entry);
	}
	pts->entry = entry;
}
#define pt_install_leaf_entry armv8pt_install_leaf_entry

static inline bool armv8pt_install_table(struct pt_state *pts,
					 pt_oaddr_t table_pa,
					 const struct pt_write_attrs *attrs)
{
	u64 *tablep = pt_cur_table(pts, u64);
	u64 entry;

	entry = ARMV8PT_FMT_VALID | ARMV8PT_FMT_TABLE |
		FIELD_PREP(ARMV8PT_FMT_OA48,
			   log2_div(table_pa, PT_GRANULE_LG2SZ)) |
		FIELD_PREP(ARMV8PT_FMT_OA52, table_pa >> 48);

	if (pts_feature(pts, PT_FEAT_ARMV8_NS))
		entry |= ARMV8PT_FMT_NSTABLE;

	return pt_table_install64(&tablep[pts->index], entry, pts->entry);
}
#define pt_install_table armv8pt_install_table

static inline void armv8pt_attr_from_entry(const struct pt_state *pts,
					   struct pt_write_attrs *attrs)
{
	attrs->descriptor_bits =
		pts->entry &
		(ARMV8PT_FMT_SH | ARMV8PT_FMT_AF | ARMV8PT_FMT_UXN |
		 ARMV8PT_FMT_PXN | ARMV8PT_FMT_ATTRINDX | ARMV8PT_FMT_AP |
		 ARMV8PT_FMT_nG | ARMV8PT_FMT_S2MEMATTR | ARMV8PT_FMT_S2AP);
}
#define pt_attr_from_entry armv8pt_attr_from_entry

/*
 * Call fn over all the items in an entry. If the entry is contiguous this
 * iterates over the entire contiguous entry, including items preceding
 * pts->va. always_inline avoids an indirect function call.
 */
static __always_inline bool armv8pt_reduce_contig(const struct pt_state *pts,
						  bool (*fn)(u64 *tablep,
							     u64 entry))
{
	u64 *tablep = pt_cur_table(pts, u64);

	if (pts->entry & ARMV8PT_FMT_CONTIG) {
		unsigned int num_contig_lg2 = armv8pt_contig_count_lg2(pts);
		u64 *end;

		tablep += log2_set_mod(pts->index, 0, num_contig_lg2);
		end = tablep + log2_to_int(num_contig_lg2);
		for (; tablep != end; tablep++)
			if (fn(tablep, READ_ONCE(*tablep)))
				return true;
		return false;
	}
	return fn(tablep + pts->index, pts->entry);
}

static inline bool armv8pt_check_is_dirty_s1(u64 *tablep, u64 entry)
{
	return (entry & (ARMV8PT_FMT_DBM |
			 FIELD_PREP(ARMV8PT_FMT_AP, ARMV8PT_AP_RDONLY))) ==
	       ARMV8PT_FMT_DBM;
}

static bool armv6pt_clear_dirty_s1(u64 *tablep, u64 entry)
{
	WRITE_ONCE(*tablep,
		   entry | FIELD_PREP(ARMV8PT_FMT_AP, ARMV8PT_AP_RDONLY));
	return false;
}

static inline bool armv8pt_check_is_dirty_s2(u64 *tablep, u64 entry)
{
	const u64 DIRTY = ARMV8PT_FMT_DBM |
			  FIELD_PREP(ARMV8PT_FMT_S2AP, ARMV8PT_S2AP_WRITE);

	return (entry & DIRTY) == DIRTY;
}

static bool armv6pt_clear_dirty_s2(u64 *tablep, u64 entry)
{
	WRITE_ONCE(*tablep, entry & ~(u64)FIELD_PREP(ARMV8PT_FMT_S2AP,
						     ARMV8PT_S2AP_WRITE));
	return false;
}

static inline bool armv8pt_entry_write_is_dirty(const struct pt_state *pts)
{
	if (!pts_feature(pts, PT_FEAT_ARMV8_S2))
		return armv8pt_reduce_contig(pts, armv8pt_check_is_dirty_s1);
	else
		return armv8pt_reduce_contig(pts, armv8pt_check_is_dirty_s2);
}
#define pt_entry_write_is_dirty armv8pt_entry_write_is_dirty

static inline void armv8pt_entry_set_write_clean(struct pt_state *pts)
{
	if (!pts_feature(pts, PT_FEAT_ARMV8_S2))
		armv8pt_reduce_contig(pts, armv6pt_clear_dirty_s1);
	else
		armv8pt_reduce_contig(pts, armv6pt_clear_dirty_s2);
}
#define pt_entry_set_write_clean armv8pt_entry_set_write_clean

/* --- iommu */
#include <linux/generic_pt/iommu.h>
#include <linux/iommu.h>

#define pt_iommu_table pt_iommu_armv8

/* The common struct is in the per-format common struct */
static inline struct pt_common *common_from_iommu(struct pt_iommu *iommu_table)
{
	return &container_of(iommu_table, struct pt_iommu_table, iommu)
			->armpt.common;
}

static inline struct pt_iommu *iommu_from_common(struct pt_common *common)
{
	return &container_of(common, struct pt_iommu_table, armpt.common)->iommu;
}

static inline int armv8pt_iommu_set_prot(struct pt_common *common,
					 struct pt_write_attrs *attrs,
					 unsigned int iommu_prot)
{
	bool is_s1 = !pt_feature(common, PT_FEAT_ARMV8_S2);
	u64 pte = 0;

	if (is_s1) {
		u64 ap = 0;

		if (!(iommu_prot & IOMMU_WRITE) && (iommu_prot & IOMMU_READ))
			ap |= ARMV8PT_AP_RDONLY;
		if (!(iommu_prot & IOMMU_PRIV))
			ap |= ARMV8PT_AP_UNPRIV;
		pte = ARMV8PT_FMT_nG | FIELD_PREP(ARMV8PT_FMT_AP, ap);

		if (iommu_prot & IOMMU_MMIO)
			pte |= FIELD_PREP(ARMV8PT_FMT_ATTRINDX,
					  ARMV8PT_MAIR_ATTR_IDX_DEV);
		else if (iommu_prot & IOMMU_CACHE)
			pte |= FIELD_PREP(ARMV8PT_FMT_ATTRINDX,
					  ARMV8PT_MAIR_ATTR_IDX_CACHE);
	} else {
		u64 s2ap = 0;

		if (iommu_prot & IOMMU_READ)
			s2ap |= ARMV8PT_S2AP_READ;
		if (iommu_prot & IOMMU_WRITE)
			s2ap |= ARMV8PT_S2AP_WRITE;
		pte = FIELD_PREP(ARMV8PT_FMT_S2AP, s2ap);

		if (iommu_prot & IOMMU_MMIO)
			pte |= FIELD_PREP(ARMV8PT_FMT_S2MEMATTR,
					  ARMV8PT_MEMATTR_DEV);
		else if ((iommu_prot & IOMMU_CACHE) &&
			 pt_feature(common, PT_FEAT_ARMV8_S2FWB))
			pte |= FIELD_PREP(ARMV8PT_FMT_S2MEMATTR,
					  ARMV8PT_MEMATTR_FWB_WB);
		else if (iommu_prot & IOMMU_CACHE)
			pte |= FIELD_PREP(ARMV8PT_FMT_S2MEMATTR,
					  ARMV8PT_MEMATTR_OIWB);
		else
			pte |= FIELD_PREP(ARMV8PT_FMT_S2MEMATTR,
					  ARMV8PT_MEMATTR_NC);
	}

	/*
	 * For DBM the writable entry starts out dirty to avoid the HW doing
	 * memory accesses to dirty it. We can just leave the DBM bit
	 * permanently set with no cost.
	 */
	if (pt_feature(common, PT_FEAT_ARMV8_DBM) && (iommu_prot & IOMMU_WRITE))
		pte |= ARMV8PT_FMT_DBM;

	if (iommu_prot & IOMMU_CACHE)
		pte |= FIELD_PREP(ARMV8PT_FMT_SH, ARMV8PT_SH_IS);
	else
		pte |= FIELD_PREP(ARMV8PT_FMT_SH, ARMV8PT_SH_OS);

	/* FIXME for mali:
		pte |= ARM_LPAE_PTE_SH_OS;
	*/

	if (iommu_prot & IOMMU_NOEXEC)
		pte |= ARMV8PT_FMT_UXN | ARMV8PT_FMT_PXN;

	if (pt_feature(common, PT_FEAT_ARMV8_NS))
		pte |= ARMV8PT_FMT_NS;

	// FIXME not on mali:
	pte |= ARMV8PT_FMT_AF;

	attrs->descriptor_bits = pte;
	return 0;
}
#define pt_iommu_set_prot armv8pt_iommu_set_prot

static inline int armv8pt_iommu_fmt_init(struct pt_iommu_armv8 *iommu_table,
					 const struct pt_iommu_armv8_cfg *cfg)
{
	struct pt_armv8 *armv8pt = &iommu_table->armpt;
	unsigned int vasz_lg2 = cfg->common.hw_max_vasz_lg2;
	unsigned int oasz_lg2 = cfg->common.hw_max_oasz_lg2;
	unsigned int levels;

	/* Atomicity of dirty bits conflicts with an incoherent cache */
	if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_DBM) &&
	    pt_feature(&armv8pt->common, PT_FEAT_DMA_INCOHERENT))
		return -EOPNOTSUPP;

	/* The NS quirk doesn't apply at stage 2 */
	if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_NS) &&
	    pt_feature(&armv8pt->common, PT_FEAT_ARMV8_S2))
		return -EOPNOTSUPP;

	if (vasz_lg2 <= PT_GRANULE_LG2SZ)
		return -EINVAL;

	/* LVA is always supported by the format */
	/* Limit the OA */
	if (PT_GRANLE_SIZE == SZ_64K)
		armv8pt->common.max_oasz_lg2 = min(52, oasz_lg2);
	else
		armv8pt->common.max_oasz_lg2 = min(58, oasz_lg2);

	levels = DIV_ROUND_UP(vasz_lg2 - PT_GRANULE_LG2SZ,
			      PT_GRANULE_LG2SZ - ilog2(sizeof(u64)));
	if (levels > PT_MAX_TOP_LEVEL + 1)
		return -EINVAL;

	/*
	 * Table D5-6 PA size implications for the VTCR_EL2.{T0SZ, SL0}
	 * Single level is not supported without FEAT_TTST, which we are not
	 * implementing.
	 */
	if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_S2) &&
	    PT_GRANLE_SIZE == SZ_4K && levels == 1)
		return -EINVAL;

	/*
	 * Use the S2 concatenated tables feature to fold a top level of up to
	 * 16 tables into the lower level.
	 */
	if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_S2) && levels > 1) {
		unsigned int topsz_lg2 =
			vasz_lg2 - (PT_GRANULE_LG2SZ +
				    (PT_TABLEMEM_LG2SZ - ilog2(sizeof(u64))) *
					    (levels - 1));
		if (topsz_lg2 <= ilog2(16))
			levels--;
	}
	pt_top_set_level(&armv8pt->common, levels - 1);
	return 0;
}
#define pt_iommu_fmt_init armv8pt_iommu_fmt_init

#if defined(GENERIC_PT_KUNIT)
static const struct pt_iommu_armv8_cfg armv8_kunit_fmt_cfgs[] = {
	[0] = { .common.features = 0,
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
	[1] = { .common.features = BIT(PT_FEAT_ARMV8_NS),
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
	[2] = { .common.features = BIT(PT_FEAT_ARMV8_S2),
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
	[3] = { .common.features = BIT(PT_FEAT_ARMV8_TTBR1),
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
};
#define kunit_fmt_cfgs armv8_kunit_fmt_cfgs
enum {
	KUNIT_FMT_FEATURES = BIT(PT_FEAT_ARMV8_TTBR1) | BIT(PT_FEAT_ARMV8_S2) |
			     BIT(PT_FEAT_ARMV8_DBM) | BIT(PT_FEAT_ARMV8_S2FWB) |
			     BIT(PT_FEAT_ARMV8_NS)
};
#endif

#if defined(GENERIC_PT_KUNIT) && IS_ENABLED(CONFIG_IOMMU_IO_PGTABLE_LPAE)
#include <linux/io-pgtable.h>

static struct io_pgtable_ops *
armv8pt_iommu_alloc_io_pgtable(struct pt_iommu_armv8_cfg *cfg,
			       struct device *iommu_dev,
			       struct io_pgtable_cfg **unused_pgtbl_cfg)
{
	struct io_pgtable_cfg pgtbl_cfg = {};
	enum io_pgtable_fmt fmt;

	pgtbl_cfg.ias = cfg->common.hw_max_vasz_lg2;
	pgtbl_cfg.oas = cfg->common.hw_max_oasz_lg2;
	if (PT_GRANLE_SIZE == SZ_64K)
		pgtbl_cfg.pgsize_bitmap |= SZ_64K | SZ_512M;
	if (PT_GRANLE_SIZE == SZ_16K)
		pgtbl_cfg.pgsize_bitmap |= SZ_16K | SZ_32M;
	if (PT_GRANLE_SIZE == SZ_4K)
		pgtbl_cfg.pgsize_bitmap |= SZ_4K | SZ_2M | SZ_1G;
	pgtbl_cfg.coherent_walk = true;

	if (cfg->common.features & BIT(PT_FEAT_ARMV8_S2))
		fmt = ARM_64_LPAE_S2;
	else
		fmt = ARM_64_LPAE_S1;
	if (cfg->common.features & BIT(PT_FEAT_ARMV8_NS))
		pgtbl_cfg.quirks |= IO_PGTABLE_QUIRK_ARM_NS;
	if (cfg->common.features & BIT(PT_FEAT_ARMV8_TTBR1))
		pgtbl_cfg.quirks |= IO_PGTABLE_QUIRK_ARM_TTBR1;

	return alloc_io_pgtable_ops(fmt, &pgtbl_cfg, NULL);
}
#define pt_iommu_alloc_io_pgtable armv8pt_iommu_alloc_io_pgtable

static void armv8pt_iommu_setup_ref_table(struct pt_iommu_armv8 *iommu_table,
					  struct io_pgtable_ops *pgtbl_ops)
{
	struct io_pgtable_cfg *pgtbl_cfg =
		&io_pgtable_ops_to_pgtable(pgtbl_ops)->cfg;
	struct pt_common *common = &iommu_table->armpt.common;

	/* FIXME should determine the level from the pgtbl_cfg */
	if (pt_feature(common, PT_FEAT_ARMV8_S2))
		pt_top_set(common, __va(pgtbl_cfg->arm_lpae_s2_cfg.vttbr),
			   pt_top_get_level(common));
	else
		pt_top_set(common, __va(pgtbl_cfg->arm_lpae_s1_cfg.ttbr),
			   pt_top_get_level(common));
}
#define pt_iommu_setup_ref_table armv8pt_iommu_setup_ref_table

static u64 armv8pt_kunit_cmp_mask_entry(struct pt_state *pts)
{
	if (pts->type == PT_ENTRY_TABLE)
		return pts->entry & (~(u64)(ARMV8PT_FMT_OA48));
	return pts->entry & (~(u64)ARMV8PT_FMT_CONTIG);
}
#define pt_kunit_cmp_mask_entry armv8pt_kunit_cmp_mask_entry
#endif

#endif
