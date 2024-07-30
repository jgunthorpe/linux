/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 *
 * x86 PAE page table. Supports the 3, 4 and 5 level variations.
 *
 * The 3 level version is described in:
 *   Section "4.4 PAE Paging" of the Intel Software Developer's Manual Volume 3
 *
 *   Section "9.7 First-Stage Paging Entries" of the "Intel Virtualization
 *   Technology for Directed I/O Architecture Specification"
 *
 * The 4 and 5 level version is described in:
 *   Section "4.4 4-Level Paging and 5-Level Paging" of the Intel Software
 *   Developer's Manual Volume 3
 *
 *   Section "9.7 First-Stage Paging Entries" of the "Intel Virtualization
 *   Technology for Directed I/O Architecture Specification"
 *
 *   Section "2.2.6 I/O Page Tables for Guest Translations" of the "AMD I/O
 *   Virtualization Technology (IOMMU) Specification"
 *
 * It is used by x86 CPUs, AMD and VT-D IOMMU HW.
 *
 * Note the 3 level format has some differences outside what is implemented here
 * when compared to the 4/5 level format. The reserved/ignored layout is
 * different and there are functional bit differences.
 *
 * The named levels in the spec map to the pts->level as:
 *   Table/PTE - 0
 *   Directory/PDE - 1
 *   Directory Ptr/PDPTE - 2
 *   PML4/PML4E - 3
 *   PML5/PML5E - 4
 * FIXME: __sme_set
 */
#ifndef __GENERIC_PT_FMT_X86PAE_H
#define __GENERIC_PT_FMT_X86PAE_H

#include "defs_x86pae.h"
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
	X86PAE_FMT_P = BIT(0),
	X86PAE_FMT_RW = BIT(1),
	X86PAE_FMT_U = BIT(2),
	X86PAE_FMT_A = BIT(5),
	X86PAE_FMT_D = BIT(6),
	X86PAE_FMT_OA = GENMASK_ULL(51, 12),
	X86PAE_FMT_XD = BIT_ULL(63),
};

/* PDPTE/PDE */
enum {
	X86PAE_FMT_PS = BIT(7),
};

#define common_to_x86pae_pt(common_ptr) \
	container_of_const(common_ptr, struct pt_x86pae, common)
#define to_x86pae_pt(pts) common_to_x86pae_pt((pts)->range->common)

static inline pt_oaddr_t x86pae_pt_table_pa(const struct pt_state *pts)
{
	return log2_mul(FIELD_GET(X86PAE_FMT_OA, pts->entry),
			PT_TABLEMEM_LG2SZ);
}
#define pt_table_pa x86pae_pt_table_pa

static inline pt_oaddr_t x86pae_pt_entry_oa(const struct pt_state *pts)
{
	return log2_mul(FIELD_GET(X86PAE_FMT_OA, pts->entry), PT_GRANULE_LG2SZ);
}
#define pt_entry_oa x86pae_pt_entry_oa

static inline bool x86pae_pt_can_have_leaf(const struct pt_state *pts)
{
	return pts->level <= 2;
}
#define pt_can_have_leaf x86pae_pt_can_have_leaf

static inline unsigned int
x86pae_pt_table_item_lg2sz(const struct pt_state *pts)
{
	return PT_GRANULE_LG2SZ +
	       (PT_TABLEMEM_LG2SZ - ilog2(sizeof(u64))) * pts->level;
}
#define pt_table_item_lg2sz x86pae_pt_table_item_lg2sz

static inline unsigned int x86pae_pt_num_items_lg2(const struct pt_state *pts)
{
	return PT_TABLEMEM_LG2SZ - ilog2(sizeof(u64));
}
#define pt_num_items_lg2 x86pae_pt_num_items_lg2

static inline enum pt_entry_type x86pae_pt_load_entry_raw(struct pt_state *pts)
{
	const u64 *tablep = pt_cur_table(pts, u64);
	u64 entry;

	pts->entry = entry = READ_ONCE(tablep[pts->index]);
	if (!(entry & X86PAE_FMT_P))
		return PT_ENTRY_EMPTY;
	if (pts->level == 0 ||
	    (x86pae_pt_can_have_leaf(pts) && (pts->entry & X86PAE_FMT_PS)))
		return PT_ENTRY_OA;
	return PT_ENTRY_TABLE;
}
#define pt_load_entry_raw x86pae_pt_load_entry_raw

static inline void
x86pae_pt_install_leaf_entry(struct pt_state *pts, pt_oaddr_t oa,
			     unsigned int oasz_lg2,
			     const struct pt_write_attrs *attrs)
{
	u64 *tablep = pt_cur_table(pts, u64);
	u64 entry;

	entry = X86PAE_FMT_P |
		FIELD_PREP(X86PAE_FMT_OA, log2_div(oa, PT_GRANULE_LG2SZ)) |
		attrs->descriptor_bits;
	if (pts->level != 0)
		entry |= X86PAE_FMT_PS;

	WRITE_ONCE(tablep[pts->index], entry);
	pts->entry = entry;
}
#define pt_install_leaf_entry x86pae_pt_install_leaf_entry

static inline bool x86pae_pt_install_table(struct pt_state *pts,
					   pt_oaddr_t table_pa,
					   const struct pt_write_attrs *attrs)
{
	u64 *tablep = pt_cur_table(pts, u64);
	u64 entry;

	entry = X86PAE_FMT_P | X86PAE_FMT_RW | X86PAE_FMT_U | X86PAE_FMT_A |
		FIELD_PREP(X86PAE_FMT_OA, log2_div(table_pa, PT_GRANULE_LG2SZ));
	return pt_table_install64(&tablep[pts->index], entry, pts->entry);
}
#define pt_install_table x86pae_pt_install_table

static inline void x86pae_pt_attr_from_entry(const struct pt_state *pts,
					     struct pt_write_attrs *attrs)
{
	attrs->descriptor_bits = pts->entry &
				 (X86PAE_FMT_RW | X86PAE_FMT_U | X86PAE_FMT_A |
				  X86PAE_FMT_D | X86PAE_FMT_XD);
}
#define pt_attr_from_entry x86pae_pt_attr_from_entry

/* --- iommu */
#include <linux/generic_pt/iommu.h>
#include <linux/iommu.h>

#define pt_iommu_table pt_iommu_x86pae

/* The common struct is in the per-format common struct */
static inline struct pt_common *common_from_iommu(struct pt_iommu *iommu_table)
{
	return &container_of(iommu_table, struct pt_iommu_table, iommu)
			->x86pae_pt.common;
}

static inline struct pt_iommu *iommu_from_common(struct pt_common *common)
{
	return &container_of(common, struct pt_iommu_table, x86pae_pt.common)
			->iommu;
}

static inline int x86pae_pt_iommu_set_prot(struct pt_common *common,
					   struct pt_write_attrs *attrs,
					   unsigned int iommu_prot)
{
	u64 pte;

	pte = X86PAE_FMT_U | X86PAE_FMT_A | X86PAE_FMT_D;
	if (iommu_prot & IOMMU_WRITE)
		pte |= X86PAE_FMT_RW;

	attrs->descriptor_bits = pte;
	return 0;
}
#define pt_iommu_set_prot x86pae_pt_iommu_set_prot

static inline int
x86pae_pt_iommu_fmt_init(struct pt_iommu_x86pae *iommu_table,
			 const struct pt_iommu_x86pae_cfg *cfg)
{
	struct pt_x86pae *table = &iommu_table->x86pae_pt;

	/*
	 * FIXME: Not totally sure what the AGW stuff in the VTD spec is about
	 * Do we expect to have other values for ias here and limit the
	 * aperture?
	 */
	switch (cfg->common.hw_max_vasz_lg2) {
		case 39:
			pt_top_set_level(&table->common, 2);
			break;
		case 48:
			pt_top_set_level(&table->common, 3);
			break;
		case 57:
			pt_top_set_level(&table->common, 4);
			break;
		default:
			return -EINVAL;
	}
	return 0;
}
#define pt_iommu_fmt_init x86pae_pt_iommu_fmt_init

static inline void
x86pae_pt_iommu_fmt_hw_info(struct pt_iommu_x86pae *table,
			    const struct pt_range *top_range,
			    struct pt_iommu_x86pae_hw_info *info)
{
	info->gcr3_pt = virt_to_phys(top_range->top_table);
	PT_WARN_ON(log2_mod_t(phys_addr_t, info->gcr3_pt, 12));
	info->levels = top_range->top_level + 1;
}
#define pt_iommu_fmt_hw_info x86pae_pt_iommu_fmt_hw_info

#if defined(GENERIC_PT_KUNIT)
static const struct pt_iommu_x86pae_cfg x86pae_kunit_fmt_cfgs[] = {
	[0] = { .common.hw_max_vasz_lg2 = 48 },
	[1] = { .common.hw_max_vasz_lg2 = 57 },
};
#define kunit_fmt_cfgs x86pae_kunit_fmt_cfgs
enum { KUNIT_FMT_FEATURES = 0 };
#endif

#if defined(GENERIC_PT_KUNIT) && IS_ENABLED(CONFIG_AMD_IOMMU)
#include <linux/io-pgtable.h>
#include "../../amd/amd_iommu_types.h"

static struct io_pgtable_ops *
x86pae_pt_iommu_alloc_io_pgtable(struct pt_iommu_x86pae_cfg *cfg,
				 struct device *iommu_dev,
				 struct io_pgtable_cfg **pgtbl_cfg)
{
	struct amd_io_pgtable *pgtable;
	struct io_pgtable_ops *pgtbl_ops;

	/* Matches what io_pgtable does */
	if (cfg->common.hw_max_vasz_lg2 != 48)
		return ERR_PTR(-EOPNOTSUPP);

	/*
	 * AMD expects that io_pgtable_cfg is allocated to its type by the
	 * caller.
	 */
	pgtable = kzalloc(sizeof(*pgtable), GFP_KERNEL);
	if (!pgtable)
		return NULL;

	pgtable->pgtbl.cfg.iommu_dev = iommu_dev;
	pgtable->pgtbl.cfg.amd.nid = NUMA_NO_NODE;
	pgtbl_ops =
		alloc_io_pgtable_ops(AMD_IOMMU_V2, &pgtable->pgtbl.cfg, NULL);
	if (!pgtbl_ops) {
		kfree(pgtable);
		return NULL;
	}
	*pgtbl_cfg = &pgtable->pgtbl.cfg;
	return pgtbl_ops;
}
#define pt_iommu_alloc_io_pgtable x86pae_pt_iommu_alloc_io_pgtable

static void x86pae_pt_iommu_free_pgtbl_cfg(struct io_pgtable_cfg *pgtbl_cfg)
{
	struct amd_io_pgtable *pgtable =
		container_of(pgtbl_cfg, struct amd_io_pgtable, pgtbl.cfg);

	kfree(pgtable);
}
#define pt_iommu_free_pgtbl_cfg x86pae_pt_iommu_free_pgtbl_cfg

static void x86pae_pt_iommu_setup_ref_table(struct pt_iommu_x86pae *iommu_table,
					    struct io_pgtable_ops *pgtbl_ops)
{
	struct io_pgtable_cfg *pgtbl_cfg =
		&io_pgtable_ops_to_pgtable(pgtbl_ops)->cfg;
	struct amd_io_pgtable *pgtable =
		container_of(pgtbl_cfg, struct amd_io_pgtable, pgtbl.cfg);
	struct pt_common *common = &iommu_table->x86pae_pt.common;

	/* FIXME Why is IOMMU_IN_ADDR_BIT_SIZE 52? */
	if (pgtbl_cfg->ias == 52)
		pt_top_set(common, (struct pt_table_p *)pgtable->pgd, 3);
	else if (pgtbl_cfg->ias == 57)
		pt_top_set(common, (struct pt_table_p *)pgtable->pgd, 4);
	else
		WARN_ON(true);
}
#define pt_iommu_setup_ref_table x86pae_pt_iommu_setup_ref_table

static u64 x86pae_pt_kunit_cmp_mask_entry(struct pt_state *pts)
{
	if (pts->type == PT_ENTRY_TABLE)
		return pts->entry & (~(u64)(X86PAE_FMT_OA));
	return pts->entry;
}
#define pt_kunit_cmp_mask_entry x86pae_pt_kunit_cmp_mask_entry
#endif

#endif
