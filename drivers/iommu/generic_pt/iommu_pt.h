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
#include <linux/cleanup.h>
#include <linux/dma-mapping.h>

/*
 * Keep track of what table items are being written too during mutation
 * operations. When the HW is DMA Incoherent these have to be cache flushed
 * before they are visible. The write_log batches flushes together and uses a C
 * cleanup to make sure the table memory is flushed before walking concludes
 * with that table.
 *
 * There are two notable cases that need special flushing:
 *  1) Installing a table entry requires the new table memory (and all of it's
 *     children) are flushed.
 *  2) Installing a shared table requires that other threads using the shared
 *     table ensure it is flushed before they attempt to use it.
 */
struct iommu_write_log {
	struct pt_range *range;
	struct pt_table_p *table;
	unsigned int start_idx;
	unsigned int last_idx;
};

static void record_write(struct iommu_write_log *wlog,
			 const struct pt_state *pts,
			 unsigned int index_count_lg2)
{
	if (!(PT_SUPPORTED_FEATURES & BIT(PT_FEAT_DMA_INCOHERENT)))
		return;

	if (!wlog->table) {
		wlog->table = pts->table;
		wlog->start_idx = pts->index;
	}
	wlog->last_idx =
		max(wlog->last_idx,
		    log2_set_mod(pts->index + log2_to_int(index_count_lg2), 0,
				 index_count_lg2));
}

static void done_writes(struct iommu_write_log *wlog)
{
	struct pt_iommu *iommu_table = iommu_from_common(wlog->range->common);
	dma_addr_t dma;

	if (!pt_feature(wlog->range->common, PT_FEAT_DMA_INCOHERENT) ||
	    !wlog->table)
		return;

	dma = virt_to_phys(wlog->table);
	dma_sync_single_for_device(iommu_table->iommu_device,
				   dma + wlog->start_idx * PT_ENTRY_WORD_SIZE,
				   (wlog->last_idx - wlog->start_idx + 1) *
					   PT_ENTRY_WORD_SIZE,
				   DMA_TO_DEVICE);
	wlog->table = NULL;
}

static int make_range(struct pt_common *common, struct pt_range *range,
		      dma_addr_t iova, dma_addr_t len)
{
	dma_addr_t last;

	if (unlikely(len == 0))
		return -EINVAL;

	if (check_add_overflow(iova, len - 1, &last))
		return -EOVERFLOW;

	*range = pt_make_range(common, iova, last);
	if (sizeof(iova) > sizeof(range->va)) {
		if (unlikely(range->va != iova || range->last_va != last))
			return -EOVERFLOW;
	}
	return pt_check_range(range);
}

static __always_inline int __do_iova_to_phys(struct pt_range *range, void *arg,
					     unsigned int level,
					     struct pt_table_p *table,
					     pt_level_fn_t descend_fn)
{
	struct pt_state pts = pt_init(range, level, table);
	pt_oaddr_t *res = arg;

	switch (pt_load_single_entry(&pts)) {
	case PT_ENTRY_EMPTY:
		return -ENOENT;
	case PT_ENTRY_TABLE:
		return pt_descend(&pts, arg, descend_fn);
	case PT_ENTRY_OA:
		*res = pt_entry_oa_full(&pts);
		return 0;
	}
	return -ENOENT;
}
PT_MAKE_LEVELS(__iova_to_phys, __do_iova_to_phys);

static phys_addr_t NS(iova_to_phys)(struct pt_iommu *iommu_table,
				    dma_addr_t iova)
{
	struct pt_range range;
	pt_oaddr_t res;
	int ret;

	ret = make_range(common_from_iommu(iommu_table), &range, iova, 1);
	if (ret)
		return ret;

	ret = pt_walk_range(&range, __iova_to_phys, &res);
	/* PHYS_ADDR_MAX would be a better error code */
	if (ret)
		return 0;
	return res;
}

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

struct pt_unmap_args {
	struct pt_radix_list_head free_list;
	pt_vaddr_t unmapped;
};

static int __unmap_range(struct pt_range *range, void *arg, unsigned int level,
			 struct pt_table_p *table)
{
	struct iommu_write_log wlog __cleanup(done_writes) = { .range = range };
	struct pt_state pts = pt_init(range, level, table);
	struct pt_unmap_args *unmap = arg;
	int ret;

	for_each_pt_level_item(&pts) {
		switch (pts.type) {
		case PT_ENTRY_TABLE: {
			bool fully_covered = pt_entry_fully_covered(
				&pts, pt_table_item_lg2sz(&pts));

			ret = pt_descend(&pts, arg, __unmap_range);
			if (ret)
				return ret;

			/*
			 * If the unmapping range fully covers the table then we
			 * can free it as well. The clear is delayed until we
			 * succeed in clearing the lower table levels.
			 */
			if (fully_covered) {
				pt_radix_add_list(&unmap->free_list,
						  pts.table_lower);
				record_write(&wlog, &pts, ilog2(1));
				pt_clear_entry(&pts, ilog2(1));
			}
			break;
		}
		case PT_ENTRY_EMPTY:
			return -EFAULT;
		case PT_ENTRY_OA: {
			unsigned int oasz_lg2 = pt_entry_oa_lg2sz(&pts);

			/*
			 * The IOMMU API does not require drivers to support
			 * unmapping parts of large pages. Long ago VFIO would
			 * try to split maps but the current version never does.
			 *
			 * Instead when unmap reaches a partial unmap of the
			 * start of a large IOPTE it should remove the entire
			 * IOPTE and return that size to the caller.
			 */
			if (log2_mod(range->va, oasz_lg2))
				return -EINVAL;

			unmap->unmapped += log2_to_int(oasz_lg2);
			record_write(&wlog, &pts,
				     pt_entry_num_contig_lg2(&pts));
			pt_clear_entry(&pts, pt_entry_num_contig_lg2(&pts));
			break;
		}
		}
	}
	return 0;
}

static size_t NS(unmap_range)(struct pt_iommu *iommu_table, dma_addr_t iova,
			      dma_addr_t len,
			      struct iommu_iotlb_gather *iotlb_gather)
{
	struct pt_common *common = common_from_iommu(iommu_table);
	struct pt_unmap_args unmap = { .free_list = PT_RADIX_LIST_INIT };
	struct pt_range range;
	int ret;

	ret = make_range(common_from_iommu(iommu_table), &range, iova, len);
	if (ret)
		return 0;

	pt_walk_range(&range, __unmap_range, &unmap);

	if (pt_feature(common, PT_FEAT_DMA_INCOHERENT))
		pt_radix_stop_incoherent_list(&unmap.free_list,
					      iommu_table->iommu_device);

	/* FIXME into gather */
	pt_radix_free_list_rcu(&unmap.free_list);
	return unmap.unmapped;
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
	.unmap_range = NS(unmap_range),
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
