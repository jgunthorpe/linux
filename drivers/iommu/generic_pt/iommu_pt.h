/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 *
 * "Templated C code" for implementing the iommu operations for page tables.
 * This is compiled multiple times, over all the page table formats to pick up
 * the per-format definitions.
 */
#ifndef __GENERIC_PT_IOMMU_PT_H
#define __GENERIC_PT_IOMMU_PT_H

#include "pt_iter.h"
#include "pt_alloc.h"

#include <linux/iommu.h>
#include <linux/export.h>

struct pt_iommu_collect_args {
	struct pt_radix_list_head free_list;
	u8 ignore_mapped : 1;
};

static int __collect_tables(struct pt_range *range, void *arg,
			    unsigned int level, struct pt_table_p *table)
{
	struct pt_state pts = pt_init(range, level, table);
	struct pt_iommu_collect_args *collect = arg;
	int ret;

	if (collect->ignore_mapped && !pt_can_have_table(&pts))
		return 0;

	for_each_pt_level_item(&pts) {
		if (pts.type == PT_ENTRY_TABLE) {
			pt_radix_add_list(&collect->free_list, pts.table_lower);
			ret = pt_descend(&pts, arg, __collect_tables);
			if (ret)
				return ret;
			continue;
		}
		if (pts.type == PT_ENTRY_OA && !collect->ignore_mapped)
			return -EADDRINUSE;
	}
	return 0;
}

static void NS(get_info)(struct pt_iommu *iommu_table,
			 struct pt_iommu_info *info)
{
	struct pt_common *common = common_from_iommu(iommu_table);
	struct pt_range range = pt_top_range(common);
	struct pt_state pts = pt_init_top(&range);
	pt_vaddr_t pgsize_bitmap = 0;

	if (pt_feature(common, PT_FEAT_DYNAMIC_TOP)) {
		for (pts.level = 0; pts.level <= PT_MAX_TOP_LEVEL;
		     pts.level++) {
			if (pt_table_item_lg2sz(&pts) >= common->max_vasz_lg2)
				break;
			pgsize_bitmap |= pt_possible_sizes(&pts);
		}
	} else {
		for (pts.level = 0; pts.level <= range.top_level; pts.level++)
			pgsize_bitmap |= pt_possible_sizes(&pts);
	}

	/* Hide page sizes larger than the maximum OA */
	info->pgsize_bitmap = oalog2_mod(pgsize_bitmap, common->max_oasz_lg2);
}

static void NS(deinit)(struct pt_iommu *iommu_table)
{
	struct pt_common *common = common_from_iommu(iommu_table);
	struct pt_range range = pt_top_range(common);
	struct pt_iommu_collect_args collect = {
		.free_list = PT_RADIX_LIST_INIT,
		.ignore_mapped = true,
	};

	pt_radix_add_list(&collect.free_list, range.top_table);
	pt_walk_range(&range, __collect_tables, &collect);
	if (pt_feature(common, PT_FEAT_DMA_INCOHERENT))
		pt_radix_stop_incoherent_list(&collect.free_list,
					      iommu_table->iommu_device);
	pt_radix_free_list(&collect.free_list);
}

static const struct pt_iommu_ops NS(ops) = {
	.iova_to_phys = NS(iova_to_phys),
	.get_info = NS(get_info),
	.deinit = NS(deinit),
};

static int pt_init_common(struct pt_common *common)
{
	struct pt_range top_range = pt_top_range(common);

	if (PT_WARN_ON(top_range.top_level > PT_MAX_TOP_LEVEL))
		return -EINVAL;

	if (top_range.top_level == PT_MAX_TOP_LEVEL ||
	    common->max_vasz_lg2 == top_range.max_vasz_lg2)
		common->features &= ~BIT(PT_FEAT_DYNAMIC_TOP);

	if (!pt_feature(common, PT_FEAT_DYNAMIC_TOP))
		common->max_vasz_lg2 = top_range.max_vasz_lg2;

	if (top_range.max_vasz_lg2 == PT_VADDR_MAX_LG2)
		common->features |= BIT(PT_FEAT_FULL_VA);

	/* Requested features must match features compiled into this format */
	if ((common->features & ~(unsigned int)PT_SUPPORTED_FEATURES) ||
	    (common->features & PT_FORCE_ENABLED_FEATURES) !=
		    PT_FORCE_ENABLED_FEATURES)
		return -EOPNOTSUPP;

	/* FIXME generalize the oa/va maximums from HW better in the cfg */
	if (common->max_oasz_lg2 == 0)
		common->max_oasz_lg2 = pt_max_output_address_lg2(common);
	else
		common->max_oasz_lg2 = min(common->max_oasz_lg2,
					   pt_max_output_address_lg2(common));
	return 0;
}

static void pt_iommu_init_domain(struct pt_iommu *iommu_table,
				 struct iommu_domain *domain)
{
	struct pt_common *common = common_from_iommu(iommu_table);
	struct pt_iommu_info info;

	NS(get_info)(iommu_table, &info);

	domain->geometry.aperture_start = fvalog2_set_mod(
		pt_full_va_prefix(common), 0, common->max_vasz_lg2);
	/* aperture_end is a last */
	domain->geometry.aperture_end = fvalog2_set_mod_max(
		pt_full_va_prefix(common), common->max_vasz_lg2);
	domain->pgsize_bitmap = info.pgsize_bitmap;
	domain->type = __IOMMU_DOMAIN_PAGING;
	domain->iommupt = iommu_table;
}

static void pt_iommu_zero(struct pt_iommu_table *fmt_table)
{
	struct pt_iommu *iommu_table = &fmt_table->iommu;
	struct pt_iommu cfg = *iommu_table;

	memset(fmt_table, 0, sizeof(*fmt_table));

	/* The caller can initialize some of these values */
	iommu_table->iommu_device = cfg.iommu_device;
	iommu_table->hw_flush_ops = cfg.hw_flush_ops;
	iommu_table->nid = cfg.nid;
}

#define pt_iommu_table_cfg CONCATENATE(pt_iommu_table, _cfg)
#define pt_iommu_init CONCATENATE(CONCATENATE(pt_iommu_, PTPFX), init)
int pt_iommu_init(struct pt_iommu_table *fmt_table,
		  const struct pt_iommu_table_cfg *cfg, gfp_t gfp)
{
	struct pt_iommu *iommu_table = &fmt_table->iommu;
	struct pt_common *common = common_from_iommu(iommu_table);
	struct pt_table_p *table_mem;
	int ret;

	if (cfg->common.hw_max_vasz_lg2 > PT_MAX_VA_ADDRESS_LG2 ||
	    !cfg->common.hw_max_vasz_lg2 || !cfg->common.hw_max_oasz_lg2)
		return -EINVAL;

	if (PT_WARN_ON(!iommu_table->hw_flush_ops))
		return -EINVAL;

	pt_iommu_zero(fmt_table);
	common->features = cfg->common.features;
	common->max_vasz_lg2 = cfg->common.hw_max_vasz_lg2;
	common->max_oasz_lg2 = cfg->common.hw_max_oasz_lg2;
	ret = pt_iommu_fmt_init(fmt_table, cfg);
	if (ret)
		return ret;

	if (cfg->common.hw_max_oasz_lg2 > pt_max_output_address_lg2(common))
		return -EINVAL;

	ret = pt_init_common(common);
	if (ret)
		return ret;

	if (pt_feature(common, PT_FEAT_DYNAMIC_TOP) &&
	    WARN_ON(!iommu_table->hw_flush_ops->change_top ||
		    !iommu_table->hw_flush_ops->get_top_lock))
		return -EINVAL;

	table_mem = table_alloc_top(common, common->top_of_table, gfp, false);
	if (IS_ERR(table_mem))
		return PTR_ERR(table_mem);
#ifdef PT_FIXED_TOP_LEVEL
	pt_top_set(common, table_mem, PT_FIXED_TOP_LEVEL);
#else
	pt_top_set(common, table_mem, pt_top_get_level(common));
#endif
	iommu_table->ops = &NS(ops);
	if (cfg->common.domain)
		pt_iommu_init_domain(iommu_table, cfg->common.domain);
	return 0;
}
EXPORT_SYMBOL_NS_GPL(pt_iommu_init, GENERIC_PT_IOMMU);

#ifdef pt_iommu_fmt_hw_info
#define pt_iommu_table_hw_info CONCATENATE(pt_iommu_table, _hw_info)
#define pt_iommu_hw_info CONCATENATE(CONCATENATE(pt_iommu_, PTPFX), hw_info)
void pt_iommu_hw_info(struct pt_iommu_table *fmt_table,
		      struct pt_iommu_table_hw_info *info)
{
	struct pt_iommu *iommu_table = &fmt_table->iommu;
	struct pt_common *common = common_from_iommu(iommu_table);
	struct pt_range top_range = pt_top_range(common);

	pt_iommu_fmt_hw_info(fmt_table, &top_range, info);
}
EXPORT_SYMBOL_NS_GPL(pt_iommu_hw_info, GENERIC_PT_IOMMU);
#endif

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("IOMMU Pagetable implementation for " __stringify(PTPFX_RAW));
MODULE_IMPORT_NS(GENERIC_PT);

#endif
