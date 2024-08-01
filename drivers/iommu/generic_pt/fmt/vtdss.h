/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 *
 * Intel VT-D Second Stange 5/4 level page table
 *
 * This is described in
 *   Section "3.7 Second-Stage Translation"
 *   Section "9.8 Second-Stage Paging Entries"
 *
 * Of the "Intel Virtualization Technology for Directed I/O Architecture
 * Specification".
 *
 * The named levels in the spec map to the pts->level as:
 *   Table/SS-PTE - 0
 *   Directory/SS-PDE - 1
 *   Directory Ptr/SS-PDPTE - 2
 *   PML4/SS-PML4E - 3
 *   PML5/SS-PML5E - 4
 * FIXME:
 *  force_snooping
 *  1g optional
 *  forbid read-only
 *  Use of direct clflush instead of DMA API
 */
#ifndef __GENERIC_PT_FMT_VTDSS_H
#define __GENERIC_PT_FMT_VTDSS_H

#include "defs_vtdss.h"
#include "../pt_defs.h"

#include <linux/bitfield.h>
#include <linux/container_of.h>
#include <linux/log2.h>

enum {
	PT_MAX_OUTPUT_ADDRESS_LG2 = 52,
	PT_MAX_VA_ADDRESS_LG2 = 57,
	PT_ENTRY_WORD_SIZE = sizeof(u64),
	PT_MAX_TOP_LEVEL = 4,
	PT_GRANULE_LG2SZ = 12,
	PT_TABLEMEM_LG2SZ = 12,
};

/* Shared descriptor bits */
enum {
	VTDSS_FMT_R = BIT(0),
	VTDSS_FMT_W = BIT(1),
	VTDSS_FMT_A = BIT(8),
	VTDSS_FMT_D = BIT(9),
	VTDSS_FMT_SNP = BIT(11),
	VTDSS_FMT_OA = GENMASK_ULL(51, 12),
};

/* PDPTE/PDE */
enum {
	VTDSS_FMT_PS = BIT(7),
};

#define common_to_vtdss_pt(common_ptr) \
	container_of_const(common_ptr, struct pt_vtdss, common)
#define to_vtdss_pt(pts) common_to_vtdss_pt((pts)->range->common)

static inline pt_oaddr_t vtdss_pt_table_pa(const struct pt_state *pts)
{
	return log2_mul(FIELD_GET(VTDSS_FMT_OA, pts->entry), PT_TABLEMEM_LG2SZ);
}
#define pt_table_pa vtdss_pt_table_pa

static inline pt_oaddr_t vtdss_pt_entry_oa(const struct pt_state *pts)
{
	return log2_mul(FIELD_GET(VTDSS_FMT_OA, pts->entry), PT_GRANULE_LG2SZ);
}
#define pt_entry_oa vtdss_pt_entry_oa

static inline bool vtdss_pt_can_have_leaf(const struct pt_state *pts)
{
	return pts->level <= 2;
}
#define pt_can_have_leaf vtdss_pt_can_have_leaf

static inline unsigned int vtdss_pt_table_item_lg2sz(const struct pt_state *pts)
{
	return PT_GRANULE_LG2SZ +
	       (PT_TABLEMEM_LG2SZ - ilog2(sizeof(u64))) * pts->level;
}
#define pt_table_item_lg2sz vtdss_pt_table_item_lg2sz

static inline unsigned int vtdss_pt_num_items_lg2(const struct pt_state *pts)
{
	return PT_TABLEMEM_LG2SZ - ilog2(sizeof(u64));
}
#define pt_num_items_lg2 vtdss_pt_num_items_lg2

static inline enum pt_entry_type vtdss_pt_load_entry_raw(struct pt_state *pts)
{
	const u64 *tablep = pt_cur_table(pts, u64);
	u64 entry;

	pts->entry = entry = READ_ONCE(tablep[pts->index]);
	if (!entry)
		return PT_ENTRY_EMPTY;
	if (pts->level == 0 ||
	    (vtdss_pt_can_have_leaf(pts) && (pts->entry & VTDSS_FMT_PS)))
		return PT_ENTRY_OA;
	return PT_ENTRY_TABLE;
}
#define pt_load_entry_raw vtdss_pt_load_entry_raw

static inline void
vtdss_pt_install_leaf_entry(struct pt_state *pts, pt_oaddr_t oa,
			    unsigned int oasz_lg2,
			    const struct pt_write_attrs *attrs)
{
	u64 *tablep = pt_cur_table(pts, u64);
	u64 entry;

	entry = FIELD_PREP(VTDSS_FMT_OA, log2_div(oa, PT_GRANULE_LG2SZ)) |
		attrs->descriptor_bits;
	if (pts->level != 0)
		entry |= VTDSS_FMT_PS;

	WRITE_ONCE(tablep[pts->index], entry);
	pts->entry = entry;
}
#define pt_install_leaf_entry vtdss_pt_install_leaf_entry

static inline bool vtdss_pt_install_table(struct pt_state *pts,
					  pt_oaddr_t table_pa,
					  const struct pt_write_attrs *attrs)
{
	u64 *tablep = pt_cur_table(pts, u64);
	u64 entry;

	/*
	 * FIXME according to the SDM D is ignored by HW on table pointers?
	 * io_pgtable_v2 sets it
	 */
	entry = VTDSS_FMT_R | VTDSS_FMT_W |
		FIELD_PREP(VTDSS_FMT_OA, log2_div(table_pa, PT_GRANULE_LG2SZ));
	return pt_table_install64(&tablep[pts->index], entry, pts->entry);
}
#define pt_install_table vtdss_pt_install_table

static inline void vtdss_pt_attr_from_entry(const struct pt_state *pts,
					    struct pt_write_attrs *attrs)
{
	attrs->descriptor_bits = pts->entry &
				 (VTDSS_FMT_R | VTDSS_FMT_W | VTDSS_FMT_SNP);
}
#define pt_attr_from_entry vtdss_pt_attr_from_entry

/* --- iommu */
#include <linux/generic_pt/iommu.h>
#include <linux/iommu.h>

#define pt_iommu_table pt_iommu_vtdss

/* The common struct is in the per-format common struct */
static inline struct pt_common *common_from_iommu(struct pt_iommu *iommu_table)
{
	return &container_of(iommu_table, struct pt_iommu_table, iommu)
			->vtdss_pt.common;
}

static inline struct pt_iommu *iommu_from_common(struct pt_common *common)
{
	return &container_of(common, struct pt_iommu_table, vtdss_pt.common)
			->iommu;
}

static inline int vtdss_pt_iommu_set_prot(struct pt_common *common,
					  struct pt_write_attrs *attrs,
					  unsigned int iommu_prot)
{
	u64 pte = 0;

	/*
	 * VTDSS does not have a present bit, so we tell if any entry is present
	 * by checking for R or W.
	 */
	if (!(iommu_prot & (IOMMU_READ | IOMMU_WRITE)))
		return -EINVAL;

	/*
	 * FIXME: The VTD driver has a bug setting DMA_FL_PTE_PRESENT on the SS
	 * table, which forces R on always.
	 */
	pte |= VTDSS_FMT_R;

	if (iommu_prot & IOMMU_READ)
		pte |= VTDSS_FMT_R;
	if (iommu_prot & IOMMU_WRITE)
		pte |= VTDSS_FMT_W;
/* FIXME	if (dmar_domain->set_pte_snp)
		pte |= VTDSS_FMT_SNP; */

	attrs->descriptor_bits = pte;
	return 0;
}
#define pt_iommu_set_prot vtdss_pt_iommu_set_prot

static inline int vtdss_pt_iommu_fmt_init(struct pt_iommu_vtdss *iommu_table,
					  const struct pt_iommu_vtdss_cfg *cfg)
{
	struct pt_vtdss *table = &iommu_table->vtdss_pt;

	/* FIXME configurable */
	pt_top_set_level(&table->common, 3);
	return 0;
}
#define pt_iommu_fmt_init vtdss_pt_iommu_fmt_init

#if defined(GENERIC_PT_KUNIT)
#endif

/*
 * Requires Tina's series:
 *  https://patch.msgid.link/r/20231106071226.9656-3-tina.zhang@intel.com
 * See my github for an integrated version
 */
#if defined(GENERIC_PT_KUNIT) && IS_ENABLED(CONFIG_CONFIG_IOMMU_IO_PGTABLE_VTD)
#include <linux/io-pgtable.h>

static struct io_pgtable_ops *
vtdss_pt_iommu_alloc_io_pgtable(struct pt_iommu_vtdss_cfg *cfg,
				struct device *iommu_dev,
				struct io_pgtable_cfg **unused_pgtbl_cfg)
{
	struct io_pgtable_cfg pgtbl_cfg = {};

	pgtbl_cfg.ias = 48;
	pgtbl_cfg.oas = 52;
	pgtbl_cfg.vtd_cfg.cap_reg = 4 << 8;
	pgtbl_cfg.vtd_cfg.ecap_reg = BIT(26) | BIT_ULL(60) | BIT_ULL(48) |
				     BIT_ULL(56);
	pgtbl_cfg.pgsize_bitmap = SZ_4K;
	pgtbl_cfg.coherent_walk = true;
	return alloc_io_pgtable_ops(INTEL_IOMMU, &pgtbl_cfg, NULL);
}
#define pt_iommu_alloc_io_pgtable vtdss_pt_iommu_alloc_io_pgtable

static void vtdss_pt_iommu_setup_ref_table(struct pt_iommu_vtdss *iommu_table,
					   struct io_pgtable_ops *pgtbl_ops)
{
	struct io_pgtable_cfg *pgtbl_cfg =
		&io_pgtable_ops_to_pgtable(pgtbl_ops)->cfg;
	struct pt_common *common = &iommu_table->vtdss_pt.common;

	pt_top_set(common, __va(pgtbl_cfg->vtd_cfg.pgd), 3);
}
#define pt_iommu_setup_ref_table vtdss_pt_iommu_setup_ref_table

static u64 vtdss_pt_kunit_cmp_mask_entry(struct pt_state *pts)
{
	if (pts->type == PT_ENTRY_TABLE)
		return pts->entry & (~(u64)(VTDSS_FMT_OA));
	return pts->entry;
}
#define pt_kunit_cmp_mask_entry vtdss_pt_kunit_cmp_mask_entry
#endif

#endif
