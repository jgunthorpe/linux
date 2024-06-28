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

/* Allocate a table, the empty table will be ready to be installed. */
static inline struct pt_table_p *_table_alloc(struct pt_common *common,
					      size_t lg2sz, gfp_t gfp,
					      bool no_incoherent_start)
{
	struct pt_iommu *iommu_table = iommu_from_common(common);
	struct pt_table_p *table_mem;

	table_mem = pt_radix_alloc(common, iommu_table->nid, lg2sz, gfp);
	if (pt_feature(common, PT_FEAT_DMA_INCOHERENT) &&
	    !no_incoherent_start) {
		int ret = pt_radix_start_incoherent(
			table_mem, iommu_table->iommu_device, true);
		if (ret) {
			pt_radix_free(table_mem);
			return ERR_PTR(ret);
		}
	}
	return table_mem;
}

static inline struct pt_table_p *table_alloc_top(struct pt_common *common,
						 uintptr_t top_of_table,
						 gfp_t gfp,
						 bool no_incoherent_start)
{
	/*
	 * FIXME top is special it doesn't need RCU or the list, and it might be
	 * small. For now just waste a page on it regardless.
	 */
	return _table_alloc(common,
			    max(pt_top_memsize_lg2(common, top_of_table),
				PAGE_SHIFT),
			    gfp, no_incoherent_start);
}

/* Allocate an interior table */
static inline struct pt_table_p *table_alloc(struct pt_state *pts, gfp_t gfp,
					     bool no_incoherent_start)
{
	return _table_alloc(pts->range->common,
			    pt_num_items_lg2(pts) + ilog2(PT_ENTRY_WORD_SIZE),
			    gfp, no_incoherent_start);
}

static inline int pt_iommu_new_table(struct pt_state *pts,
				     struct pt_write_attrs *attrs,
				     bool no_incoherent_start)
{
	struct pt_table_p *table_mem;
	phys_addr_t phys;

	/* Given PA/VA/length can't be represented */
	if (unlikely(!pt_can_have_table(pts)))
		return -ENXIO;

	table_mem = table_alloc(pts, attrs->gfp, no_incoherent_start);
	if (IS_ERR(table_mem))
		return PTR_ERR(table_mem);

	phys = virt_to_phys(table_mem);
	if (!pt_install_table(pts, phys, attrs)) {
		pt_radix_free(table_mem);
		return -EAGAIN;
	}

	if (IS_ENABLED(CONFIG_DEBUG_GENERIC_PT)) {
		/*
		 * The underlying table can't store the physical table address.
		 * This happens when kunit testing tables outside their normal
		 * environment where a CPU might be limited.
		 */
		pt_load_single_entry(pts);
		if (PT_WARN_ON(pt_table_pa(pts) != phys)) {
			pt_clear_entry(pts, ilog2(1));
			pt_radix_free(table_mem);
			return -EINVAL;
		}
	}

	pts->table_lower = table_mem;
	return 0;
}

struct pt_iommu_map_args {
	struct pt_radix_list_head free_list;
	struct pt_write_attrs attrs;
	pt_oaddr_t oa;
};

/*
 * Check that the items in a contiguous block are all empty. This will
 * recursively check any tables in the block to validate they are empty and
 * accumulate them on the free list. Makes no change on failure. On success
 * caller must fill the items.
 */
static int pt_iommu_clear_contig(const struct pt_state *start_pts,
				 struct pt_iommu_map_args *map,
				 struct iommu_write_log *wlog,
				 unsigned int pgsize_lg2)
{
	struct pt_range range = *start_pts->range;
	struct pt_state pts =
		pt_init(&range, start_pts->level, start_pts->table);
	struct pt_iommu_collect_args collect = {
		.free_list = PT_RADIX_LIST_INIT,
	};
	int ret;

	pts.index = start_pts->index;
	pts.table_lower = start_pts->table_lower;
	pts.end_index = start_pts->index +
			log2_to_int(pgsize_lg2 - pt_table_item_lg2sz(&pts));
	pts.type = start_pts->type;
	pts.entry = start_pts->entry;
	while (true) {
		if (pts.type == PT_ENTRY_TABLE) {
			ret = pt_walk_child_all(&pts, __collect_tables,
						&collect);
			if (ret)
				return ret;
			pt_radix_add_list(&collect.free_list,
					  pt_table_ptr(&pts));
		} else if (pts.type != PT_ENTRY_EMPTY) {
			return -EADDRINUSE;
		}

		_pt_advance(&pts, ilog2(1));
		if (pts.index == pts.end_index)
			break;
		pt_load_entry(&pts);
	}
	pt_radix_list_splice(&map->free_list, &collect.free_list);
	return 0;
}

static int __map_range(struct pt_range *range, void *arg, unsigned int level,
		       struct pt_table_p *table)
{
	struct iommu_write_log wlog __cleanup(done_writes) = { .range = range };
	struct pt_state pts = pt_init(range, level, table);
	struct pt_iommu_map_args *map = arg;
	int ret;

again:
	for_each_pt_level_item(&pts) {
		/*
		 * FIXME: This allows us to segment on our own, but there is
		 * probably a better performing way to implement it.
		 */
		unsigned int pgsize_lg2 = pt_compute_best_pgsize(&pts, map->oa);

		/*
		 * Our mapping fully covers this page size of items starting
		 * here
		 */
		if (pgsize_lg2) {
			if (pgsize_lg2 != pt_table_item_lg2sz(&pts) ||
			    pts.type != PT_ENTRY_EMPTY) {
				ret = pt_iommu_clear_contig(&pts, map, &wlog,
							    pgsize_lg2);
				if (ret)
					return ret;
			}

			record_write(&wlog, &pts, pgsize_lg2);
			pt_install_leaf_entry(&pts, map->oa, pgsize_lg2,
					      &map->attrs);
			pts.type = PT_ENTRY_OA;
			map->oa += log2_to_int(pgsize_lg2);
			continue;
		}

		/* Otherwise we need to descend to a child table */

		if (pts.type == PT_ENTRY_EMPTY) {
			record_write(&wlog, &pts, ilog2(1));
			ret = pt_iommu_new_table(&pts, &map->attrs, false);
			if (ret) {
				/*
				 * Racing with another thread installing a table
				 */
				if (ret == -EAGAIN)
					goto again;
				return ret;
			}
			if (pts_feature(&pts, PT_FEAT_DMA_INCOHERENT)) {
				done_writes(&wlog);
				pt_radix_done_incoherent_flush(pts.table_lower);
			}
		} else if (pts.type == PT_ENTRY_TABLE) {
			/*
			 * Racing with a shared pt_iommu_new_table()? The other
			 * thread is still flushing the cache, so we have to
			 * also flush it to ensure that when our thread's map
			 * completes our mapping is working.
			 *
			 * Using the folio memory means we don't have to rely on
			 * an available PTE bit to keep track.
			 *
			 */
			if (pts_feature(&pts, PT_FEAT_DMA_INCOHERENT) &&
			    pt_radix_incoherent_still_flushing(pts.table_lower))
				record_write(&wlog, &pts, ilog2(1));
		} else {
			return -EADDRINUSE;
		}

		/*
		 * Notice the already present table can possibly be shared with
		 * another concurrent map.
		 */
		ret = pt_descend(&pts, arg, __map_range);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Add a table to the top, increasing the top level as much as necessary to
 * encompass range.
 */
static int increase_top(struct pt_iommu *iommu_table, struct pt_range *range,
			struct pt_write_attrs *attrs)
{
	struct pt_common *common = common_from_iommu(iommu_table);
	uintptr_t top_of_table = READ_ONCE(common->top_of_table);
	struct pt_radix_list_head free_list = PT_RADIX_LIST_INIT;
	uintptr_t new_top_of_table = top_of_table;
	struct pt_table_p *table_mem;
	unsigned int new_level;
	spinlock_t *domain_lock;
	unsigned long flags;
	int ret;

	while (true) {
		struct pt_range top_range =
			_pt_top_range(common, new_top_of_table);
		struct pt_state pts = pt_init_top(&top_range);

		top_range.va = range->va;
		top_range.last_va = range->last_va;

		if (!pt_check_range(&top_range))
			break;

		pts.level++;
		if (pts.level > PT_MAX_TOP_LEVEL ||
		    pt_table_item_lg2sz(&pts) >= common->max_vasz_lg2) {
			ret = -ERANGE;
			goto err_free;
		}

		new_level = pts.level;
		table_mem = table_alloc_top(
			common, _pt_top_set(NULL, pts.level), attrs->gfp, true);
		if (IS_ERR(table_mem))
			return PTR_ERR(table_mem);
		pt_radix_add_list(&free_list, table_mem);

		/* The new table links to the lower table always at index 0 */
		top_range.va = 0;
		pts.table_lower = pts.table;
		pts.table = table_mem;
		pt_load_single_entry(&pts);
		PT_WARN_ON(pts.index != 0);
		pt_install_table(&pts, virt_to_phys(pts.table_lower), attrs);
		new_top_of_table = _pt_top_set(pts.table, pts.level);

		top_range = _pt_top_range(common, new_top_of_table);
	}

	if (pt_feature(common, PT_FEAT_DMA_INCOHERENT)) {
		ret = pt_radix_start_incoherent_list(
			&free_list, iommu_from_common(common)->iommu_device);
		if (ret)
			goto err_free;
	}

	/*
	 * top_of_table is write locked by the spinlock, but readers can use
	 * READ_ONCE() to get the value. Since we encode both the level and the
	 * pointer in one quanta the lockless reader will always see something
	 * valid. The HW must be updated to the new level under the spinlock
	 * before top_of_table is updated so that concurrent readers don't map
	 * into the new level until it is fully functional. If another thread
	 * already updated it while we were working then throw everything away
	 * and try again.
	 */
	domain_lock = iommu_table->hw_flush_ops->get_top_lock(iommu_table);
	spin_lock_irqsave(domain_lock, flags);
	if (common->top_of_table != top_of_table) {
		spin_unlock_irqrestore(domain_lock, flags);
		ret = -EAGAIN;
		goto err_free;
	}

	iommu_table->hw_flush_ops->change_top(
		iommu_table, virt_to_phys(table_mem), new_level);
	WRITE_ONCE(common->top_of_table, new_top_of_table);
	spin_unlock_irqrestore(domain_lock, flags);

	*range = pt_make_range(common, range->va, range->last_va);
	PT_WARN_ON(pt_check_range(range));
	return 0;

err_free:
	if (pt_feature(common, PT_FEAT_DMA_INCOHERENT))
		pt_radix_stop_incoherent_list(
			&free_list, iommu_from_common(common)->iommu_device);
	pt_radix_free_list(&free_list);
	return ret;
}

/*
 * If the range spans the whole current table it may be better to increase
 * the top and then place a single OA
 */
static int check_full_table(struct pt_iommu *iommu_table,
			    struct pt_range *range,
			    struct pt_iommu_map_args *map, pt_oaddr_t oa)
{
	struct pt_common *common = range->common;
	struct pt_state pts = pt_init_top(range);
	struct pt_range bigger_range = *range;
	int ret;

	/* Range spans the entire current VA size */
	if (log2_mod(range->va, range->max_vasz_lg2) != 0 ||
	    !log2_mod_eq_max(range->last_va, range->max_vasz_lg2))
		return 0;

	/* Room for expansion */
	pts.level++;
	if (pts.level > PT_MAX_TOP_LEVEL ||
	    pt_table_item_lg2sz(&pts) >= common->max_vasz_lg2)
		return 0;

	/* A single IOPTE is available for this mapping in the higher level */
	if (!pt_compute_best_pgsize(&pts, oa))
		return 0;

	/* Force an increase */
	bigger_range.last_va++;
	ret = increase_top(iommu_table, &bigger_range, &map->attrs);
	if (ret)
		return ret;
	return -EAGAIN;
}

static int NS(map_range)(struct pt_iommu *iommu_table, dma_addr_t iova,
			 phys_addr_t paddr, dma_addr_t len, unsigned int prot,
			 gfp_t gfp, size_t *mapped,
			 struct iommu_iotlb_gather *iotlb_gather)
{
	struct pt_common *common = common_from_iommu(iommu_table);
	struct pt_iommu_map_args map = {
		.free_list = PT_RADIX_LIST_INIT,
		.oa = paddr,
	};
	struct pt_range range;
	int ret;

	if (WARN_ON(!(prot & (IOMMU_READ | IOMMU_WRITE))))
		return -EINVAL;

	if ((sizeof(pt_oaddr_t) > sizeof(paddr) && paddr > PT_VADDR_MAX) ||
	    (common->max_oasz_lg2 != PT_VADDR_MAX_LG2 &&
	     oalog2_div(paddr, common->max_oasz_lg2)))
		return -ERANGE;

	ret = pt_iommu_set_prot(common, &map.attrs, prot);
	if (ret)
		return ret;
	map.attrs.gfp = gfp;

	while (1) {
		ret = make_range(common_from_iommu(iommu_table), &range, iova,
				 len);
		if (pt_feature(common, PT_FEAT_DYNAMIC_TOP)) {
			if (!ret)
				ret = check_full_table(iommu_table, &range,
						       &map, paddr);
			if (ret == -ERANGE)
				ret = increase_top(iommu_table, &range,
						   &map.attrs);
			if (ret == -EAGAIN)
				continue;
		}
		if (ret)
			return ret;
		break;
	}

	ret = pt_walk_range(&range, __map_range, &map);

	/* FIXME into gather */
	pt_radix_free_list_rcu(&map.free_list);

	/* Bytes successfully mapped */
	*mapped += map.oa - paddr;
	return ret;
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
	.map_range = NS(map_range),
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
