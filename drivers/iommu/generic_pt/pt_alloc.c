// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
#include "pt_alloc.h"
#include "pt_log2.h"
#include <linux/mm.h>
#include <linux/dma-mapping.h>

#define RADIX_MATCH(pg, rl)                        \
	static_assert(offsetof(struct page, pg) == \
		      offsetof(struct pt_radix_meta, rl))
RADIX_MATCH(flags, __page_flags);
RADIX_MATCH(rcu_head, rcu_head);	/* Ensure bit 0 is clear */
RADIX_MATCH(mapping, __page_mapping);
RADIX_MATCH(private, free_next);
RADIX_MATCH(page_type, __page_type);
RADIX_MATCH(_refcount, __page_refcount);
#ifdef CONFIG_MEMCG
RADIX_MATCH(memcg_data, memcg_data);
#endif
#undef RADIX_MATCH
static_assert(sizeof(struct pt_radix_meta) <= sizeof(struct page));

static inline struct folio *meta_to_folio(struct pt_radix_meta *meta)
{
	return (struct folio *)meta;
}

void *pt_radix_alloc(struct pt_common *owner, int nid, size_t lg2sz, gfp_t gfp)
{
	struct pt_radix_meta *meta;
	unsigned int order;
	struct folio *folio;

	/*
	 * FIXME we need to support sub page size tables, eg to allow a 4K table
	 * on a 64K kernel. This should be done by allocating extra memory
	 * per page and placing the pointer in the meta. The extra memory can
	 * contain the additional list heads and rcu's required.
	 */
	if (lg2sz <= PAGE_SHIFT)
		order = 0;
	else
		order = lg2sz - PAGE_SHIFT;

	folio = (struct folio *)alloc_pages_node(
		nid, gfp | __GFP_ZERO | __GFP_COMP, order);
	if (!folio)
		return ERR_PTR(-ENOMEM);

	meta = folio_to_meta(folio);
	meta->owner = owner;
	meta->free_next = NULL;
	meta->lg2sz = lg2sz;

	mod_node_page_state(folio_pgdat(folio), NR_IOMMU_PAGES,
			    log2_to_int_t(long, order));
	lruvec_stat_mod_folio(folio, NR_SECONDARY_PAGETABLE,
			      log2_to_int_t(long, order));

	return folio_address(folio);
}
EXPORT_SYMBOL_NS_GPL(pt_radix_alloc, GENERIC_PT);

void pt_radix_free_list(struct pt_radix_list_head *list)
{
	struct pt_radix_meta *cur = list->head;

	while (cur) {
		struct folio *folio = meta_to_folio(cur);
		unsigned int order = folio_order(folio);
		long pgcnt = 1UL << order;

		mod_node_page_state(folio_pgdat(folio), NR_IOMMU_PAGES, -pgcnt);
		lruvec_stat_mod_folio(folio, NR_SECONDARY_PAGETABLE, -pgcnt);

		cur = cur->free_next;
		folio->mapping = NULL;
		__free_pages(&folio->page, order);
	}
}
EXPORT_SYMBOL_NS_GPL(pt_radix_free_list, GENERIC_PT);

void pt_radix_free(void *radix)
{
	struct pt_radix_meta *meta = virt_to_meta(radix);
	struct pt_radix_list_head list = { .head = meta };

	pt_radix_free_list(&list);
}
EXPORT_SYMBOL_NS_GPL(pt_radix_free, GENERIC_PT);

static void pt_radix_free_list_rcu_cb(struct rcu_head *head)
{
	struct pt_radix_meta *meta =
		container_of(head, struct pt_radix_meta, rcu_head);
	struct pt_radix_list_head list = { .head = meta };

	pt_radix_free_list(&list);
}

void pt_radix_free_list_rcu(struct pt_radix_list_head *list)
{
	if (!list->head)
		return;
	call_rcu(&list->head->rcu_head, pt_radix_free_list_rcu_cb);
}
EXPORT_SYMBOL_NS_GPL(pt_radix_free_list_rcu, GENERIC_PT);

/*
 * For incoherent memory we use the DMA API to manage the cache flushing. This
 * is a lot of complexity compared to just calling arch_sync_dma_for_device(),
 * but it is what the existing iommu drivers have been doing.
 */
int pt_radix_start_incoherent(void *radix, struct device *dma_dev,
			      bool still_flushing)
{
	struct pt_radix_meta *meta = virt_to_meta(radix);
	dma_addr_t dma;

	dma = dma_map_single(dma_dev, radix, log2_to_int_t(size_t, meta->lg2sz),
			     DMA_TO_DEVICE);
	if (dma_mapping_error(dma_dev, dma))
		return -EINVAL;

	/* The DMA API is not allowed to do anything other than DMA direct. */
	if (WARN_ON(dma != virt_to_phys(radix))) {
		dma_unmap_single(dma_dev, dma,
				 log2_to_int_t(size_t, meta->lg2sz),
				 DMA_TO_DEVICE);
		return -EOPNOTSUPP;
	}
	meta->incoherent = 1;
	meta->still_flushing = 1;
	return 0;
}
EXPORT_SYMBOL_NS_GPL(pt_radix_start_incoherent, GENERIC_PT);

int pt_radix_start_incoherent_list(struct pt_radix_list_head *list,
				   struct device *dma_dev)
{
	struct pt_radix_meta *cur;
	int ret;

	for (cur = list->head; cur; cur = cur->free_next) {
		if (cur->incoherent)
			continue;

		ret = pt_radix_start_incoherent(
			folio_address(meta_to_folio(cur)), dma_dev, false);
		if (ret)
			return ret;
	}
	return 0;
}
EXPORT_SYMBOL_NS_GPL(pt_radix_start_incoherent_list, GENERIC_PT);

void pt_radix_stop_incoherent_list(struct pt_radix_list_head *list,
				   struct device *dma_dev)
{
	struct pt_radix_meta *cur;

	for (cur = list->head; cur; cur = cur->free_next) {
		struct folio *folio = meta_to_folio(cur);

		if (!cur->incoherent)
			continue;
		dma_unmap_single(dma_dev, virt_to_phys(folio_address(folio)),
				 log2_to_int_t(size_t, cur->lg2sz),
				 DMA_TO_DEVICE);
	}
}
EXPORT_SYMBOL_NS_GPL(pt_radix_stop_incoherent_list, GENERIC_PT);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Helper functions for iommu_pt");
