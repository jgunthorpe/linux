// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 */
#include <linux/rlist_dma.h>
#include <linux/p2pdma_provider.h>
#include <linux/count_zeros.h>

#define same_memory(struct_a, member_a, struct_b, member_b)              \
	(offsetof(struct_a, member_a) == offsetof(struct_b, member_b) && \
	 __same_type(&((struct_a *)NULL)->member_a,                      \
		     &((struct_b *)NULL)->member_b))

static_assert(same_memory(struct rlist_entry, base, struct rlist_dma_entry,
			  _base) &&
	      same_memory(struct rlist_entry, extra, struct rlist_dma_entry,
			  dma_map_ops_priv));

const struct rlist_dma_segmentation rlist_no_segmentation = {
	.segment_boundary_mask = ULONG_MAX,
	.max_segment_size = ULONG_MAX,
};
EXPORT_SYMBOL_GPL(rlist_no_segmentation);

static bool rlsdma_is_cpu(struct rlist_dma_state *rls)
{
	return !rls->rlist_dma->rlist.head;
}

static void rlsdma_cpu_decode(struct rlist_dma_entry *entry)
{
	entry->dma_address = rlist_cpu_entry_physical(&entry->_cpu);
}

static void rlsdma_decode(struct rlist_dma_state *rls,
			  struct rlist_dma_entry *entry)
{
	if (entry->_entry.type == RLIST_DMA_RELATIVE)
		entry->dma_address = entry->_entry.base + rls->rlist_dma->base;
	else
		entry->dma_address = entry->_entry.base;
}

bool rlsdma_reset(struct rlist_dma_state *rls, struct rlist_dma_entry *entry)
{
	if (rlsdma_is_cpu(rls)) {
		if (!rlscpu_reset(&rls->rlscpu, &entry->_cpu))
			return false;
		rlsdma_cpu_decode(entry);
		return true;
	}

	if (!rls_reset(&rls->rls, &entry->_entry))
		return false;
	rlsdma_decode(rls, entry);
	return true;
}
EXPORT_SYMBOL_GPL(rlsdma_reset);

bool rlsdma_next(struct rlist_dma_state *rls, struct rlist_dma_entry *entry)
{
	if (rlsdma_is_cpu(rls)) {
		if (!rlscpu_next(&rls->rlscpu, &entry->_cpu))
			return false;
		rlsdma_cpu_decode(entry);
		return true;

	}
	if (!rls_next(&rls->rls, &entry->_entry))
		return false;
	rlsdma_decode(rls, entry);
	return true;
}
EXPORT_SYMBOL_GPL(rlsdma_next);

void rlist_dma_init(struct rlist_dma *rdma)
{
	rdma->cpu = NULL;
	rdma->base = 0;
	rlist_init(&rdma->rlist);
}
EXPORT_SYMBOL_GPL(rlist_dma_init);

/*
 * The rlist_dma will return the CPU physical addreses of the CPU rlist iterator
 * as dma_addr_t.
 */
void rlist_dma_init_identity_cpu(struct rlist_dma *rdma, struct rlist_cpu *rcpu)
{
	rdma->cpu = rcpu;
	rdma->base = 0;
	rlist_init(&rdma->rlist);
}
EXPORT_SYMBOL_GPL(rlist_dma_init_identity_cpu);

static int rlist_dma_init_single(struct rlist_dma *rdma, dma_addr_t dma_address,
				 u64 length, u32 dma_map_ops_priv, gfp_t gfp)
{
	struct rlist_entry entry = {
		.type = RLIST_DMA_RELATIVE,
		.base = dma_address,
		.length = length,
		.extra = dma_map_ops_priv,
	};
	rdma->base = 0;
	return rlist_init_single(&rdma->rlist, &entry, gfp);
}

void rlist_dma_destroy(struct rlist_dma *rdma)
{
	if (rdma->cpu)
		return;
	rlist_destroy(&rdma->rlist);
}
EXPORT_SYMBOL_GPL(rlist_dma_destroy);

/*
 * Calculate according to the segmentation rules what the number of segments is
 * for this entry.
 */
size_t rlist_dma_num_segments(const struct rlist_dma_segmentation *segment,
			      const struct rlist_dma_entry *entry)
{
	dma_addr_t mask = segment->segment_boundary_mask;
	dma_addr_t start = entry->dma_address;
	dma_addr_t end = start + entry->length;
	size_t num_boundaries;
	size_t boundary_segs;
	size_t first_segs;
	size_t last_segs;

	WARN_ON(mask != type_max(dma_addr_t) && !is_power_of_2(mask + 1));

	if ((start & mask) == (end & mask))
		return DIV_ROUND_UP(entry->length, segment->max_segment_size);

	first_segs = DIV_ROUND_UP((start | mask) + 1 - start,
				  segment->max_segment_size);
	boundary_segs = DIV_ROUND_UP(mask + 1, segment->max_segment_size);
	last_segs =
		DIV_ROUND_UP(end - (end & (~mask)), segment->max_segment_size);
	num_boundaries = ((end & (~mask)) - ((start | mask) + 1)) / (mask + 1);
	return first_segs + boundary_segs * num_boundaries + last_segs;
}
EXPORT_SYMBOL_GPL(rlist_dma_num_segments);

/**
 * rlist_dma_find_best_blocksz - Find best HW block size to use
 * @rdma: DMA addresses to inspect
 * @segment: Segmentation parameters
 *
 * Compute the HW block size to use in a block list. HW should always call this
 * even if it only supports a single block size. It validates that the layout of
 * the rlist_dma is correct for the single supported block size.
 *
 * This supports the rlist_dma being formed so that the start/end are sub-block,
 * which can be used if the HW supports an offset and overall length limitation.
 * If HW does not support this then it should set hwva to 0 and adjust
 * the resulting block size to match the overall length.
 *
 * Returns 0 if the rlist_dma requires a block size not supported by the segment
 * struct. Callers supporting PAGE_SIZE or smaller will never see a 0 result
 * unless the rlist_dma is malformed.
 */
unsigned long
rlist_dma_find_best_blocksz(struct rlist_dma *rdma,
			    const struct rlist_dma_segmentation *segment)
{
	dma_addr_t pgsz_bitmap = segment->block_list_supported;
	unsigned long hwva = segment->block_list_hwva;
	struct rlist_dma_entry entry;
	RLIST_DMA_STATE(rls, rdma);
	bool first = true;
	dma_addr_t mask;

	if (WARN_ON(!pgsz_bitmap))
		return 0;

	/*
	 * If the segmentation can support any starting offset then using the
	 * initial dma_addr will give the largest possible page size.
	 */
	if (!segment->has_block_list_hwva) {
		if (!rlsdma_reset(&rls, &entry))
			return 0;
		hwva = entry.dma_address;
	}

	/*
	 * The best result is the smallest page size that results in the minimum
	 * number of required pages. Compute the largest page size that could
	 * work based on VA address bits that don't change.
	 */
	mask = pgsz_bitmap &
	       GENMASK(BITS_PER_LONG - 1,
		       bits_per((rlist_dma_length(rdma) - 1 + hwva) ^ hwva));

	rlist_dma_for_each_entry(&rls, &entry) {
		/*
		 * Except for the first entry, which can be offset, every
		 * block must start at:
		 *    dma_addr & blocksz == 0
		 * So any non-zero bits cap the maximum page size.
		 */
		if (!first)
			mask |= hwva;
		else
			first = false;

		/*
		 * Walk SGL and reduce max page size if VA/PA bits differ for
		 * any address.
		 */
		mask |= entry.dma_address ^ hwva;
		hwva += entry.length;
	}

	/*
	 * The mask accumulates 1's in each position where the VA and physical
	 * address differ, thus the length of trailing 0 is the largest page
	 * size that can pass the VA through to the physical.
	 */
	if (mask)
		pgsz_bitmap &= GENMASK(count_trailing_zeros(mask), 0);
	return pgsz_bitmap ? rounddown_pow_of_two(pgsz_bitmap) : 0;
}
EXPORT_SYMBOL_GPL(rlist_dma_find_best_blocksz);

/* Number of bytes from the first block that the data starts */
dma_addr_t rlist_dma_block_offset(struct rlist_dma *rdma, dma_addr_t blocksz)
{
	struct rlist_dma_entry entry;

	if (!rlist_dma_first(rdma, &entry))
		return 0;
	return entry.dma_address & (blocksz - 1);
}
EXPORT_SYMBOL_GPL(rlist_dma_block_offset);

/* Number of iterations of rlist_dma_for_each_block() */
dma_addr_t rlist_dma_num_blocks(struct rlist_dma *rdma, dma_addr_t blocksz)
{
	struct rlist_dma_entry entry;
	dma_addr_t leading_gap;

	if (!rlist_dma_first(rdma, &entry))
		return 0;

	leading_gap = entry.dma_address & (blocksz - 1);
	return ALIGN(rlist_dma_length(rdma) + leading_gap, blocksz) / blocksz;
}
EXPORT_SYMBOL_GPL(rlist_dma_num_blocks);

static void set_first_block(struct rlist_dma_entry *entry,
			     dma_addr_t blocksz)
{
	dma_addr_t leading_gap = entry->dma_address & (blocksz - 1);

	/*
	 * Each dmalist entry is adjusted to start at a block and end at a
	 * block, then we trivially iterate splitting it down.
	 */
	entry->length =
		ALIGN((u64)(entry->length + leading_gap), blocksz) - blocksz;
	entry->dma_address -= leading_gap;
}

bool rlsdma_block_iter_reset(struct rlist_dma_state *rls,
			     struct rlist_dma_entry *entry, dma_addr_t blocksz)
{
	if (!rlsdma_reset(rls, entry))
		return false;
	set_first_block(entry, blocksz);
	return true;
}
EXPORT_SYMBOL_GPL(rlsdma_block_iter_reset);

bool rlsdma_block_iter_next(struct rlist_dma_state *rls,
			    struct rlist_dma_entry *entry, dma_addr_t blocksz)
{
	if (entry->length) {
		entry->dma_address += blocksz;
		entry->length -= blocksz;
		return true;
	}
	if (!rlsdma_next(rls, entry))
		return false;
	set_first_block(entry, blocksz);
	return true;
}
EXPORT_SYMBOL_GPL(rlsdma_block_iter_next);

static void pad_iova(struct rlist_dma_state_iova *rsiova, phys_addr_t phys,
		     const struct rlist_cpu_entry *entry)
{
	dma_addr_t segment_boundary_mask =
		rsiova->segment->segment_boundary_mask;

	WARN_ON(rsiova->cur_iova % rsiova->pgsize);

	/* The lower bits of phys and iova must be the same for all iommu HW */
	rsiova->cur_iova += (phys & (rsiova->pgsize - 1));

	/*
	 * Add padding to ensure the number of post-segmentation elements does
	 * not increase. This is a simple inefficient conservative algorithm.
	 *
	 * If we don't cross a segment boundary then there is no need to add
	 * padding.
	 */
	if ((rsiova->cur_iova & segment_boundary_mask) !=
	    ((rsiova->cur_iova + entry->length) & segment_boundary_mask)) {
		struct rlist_dma_entry dma_entry = {
			.dma_address = rsiova->cur_iova, .length = entry->length
		};

		/*
		 * If we are going to make the number of entries bigger by using
		 * this IOVA then align it to the DMA segment size which will
		 * fix it.
		 */
		if (rlist_cpu_num_segments(rsiova->segment, entry) <
		    rlist_dma_num_segments(rsiova->segment, &dma_entry)) {
			rsiova->cur_iova = ALIGN(rsiova->cur_iova,
						 segment_boundary_mask + 1);
			rsiova->cur_iova += (phys & (rsiova->pgsize - 1));
		}
	}
}

static void fill_iova_map(struct rlist_dma_state_iova *rsiova,
			  struct rlist_dma_iova_map *map, phys_addr_t phys,
			  u64 length)
{
	unsigned long offset;

	/*
	 * The IOMMU only works in pgsize units, so if the map request doesn't
	 * exactly fit we map more than requested. Round the start down and the
	 * end up.
	 */
	offset = phys & (rsiova->pgsize - 1);
	map->phys = phys - offset;
	map->iova = rsiova->cur_iova - offset;
	map->length = ALIGN(length + offset, rsiova->pgsize);
	rsiova->cur_iova += map->length;
}

static int fill_p2p(struct rlist_dma_state_iova *rsiova,
		    struct rlist_dma_iova_map *map,
		    const struct rlist_cpu_entry *entry)
{
	int ret;

	switch (entry->type) {
	case RLIST_CPU_MEM_FOLIO:
		map->phys = rlist_cpu_entry_physical(entry);
		return 0;

	case RLIST_CPU_MEM_PHYSICAL: {
		struct p2pdma_provider *provider =
			p2pdma_provider_from_id(entry->provider_index);

		ret = p2pdma_provider_map(rsiova->dev, provider, entry->phys,
					  &map->iova, &rsiova->p2pdma_cache);
		if (ret == P2P_MAP_FILLED_DMA) {
			/* Addresses that are bus mapped do not allocate IOVA */
			map->phys = PHYS_ADDR_MAX;
			return 0;
		}
		map->phys = rlist_cpu_entry_physical(entry);
		return ret;
	}
	default:
		WARN(true, "Corrupt rlist_cpu");
		return -EINVAL;
	}
}

enum {
	/* 0 means it is normal allocated IOVA */
	RSIOVA_PRIV_P2P = 1,
};

static int rsiova_init_slow(struct rlist_dma_state_iova *rsiova,
			    struct rlist_cpu *rcpu, struct rlist_dma *rdma,
			    gfp_t gfp)
{
	RLIST_DMA_STATE_APPEND(rlsa, rdma);
	struct rlist_dma_iova_map map;
	struct rlist_cpu_entry entry;
	RLIST_CPU_STATE(rls, rcpu);
	int ret;

	rsiova->cur_iova = 0;

	ret = rlsdma_append_begin(&rlsa);
	if (ret)
		return ret;

	rlist_cpu_for_each_entry(&rls, &entry) {
		ret = fill_p2p(rsiova, &map, &entry);
		if (ret)
			goto err_destroy;

		if (map.phys == PHYS_ADDR_MAX) {
			ret = rlsdma_append_no_base(&rlsa, map.iova,
						    entry.length,
						    RSIOVA_PRIV_P2P, gfp);
			if (ret)
				goto err_destroy;
		} else {
			pad_iova(rsiova, map.phys, &entry);
			ret = rlsdma_append(&rlsa, rsiova->cur_iova,
					    entry.length, 0, gfp);
			if (ret)
				goto err_destroy;
			fill_iova_map(rsiova, &map, map.phys, entry.length);
		}
	}
	/* FIXME we need to keep track of how many segments because the next
	 * step will be to allocate memory */
	/*  dma_get_seg_boundary_nr_pages */

	/* Store the total iova length in cur_iova */
	rsiova->cur_iova = ALIGN(map.iova + map.length, rsiova->pgsize);
	return 0;

err_destroy:
	rlsdma_append_destroy_rlist(&rlsa);
	return ret;
}

int rsiova_init(struct rlist_dma_state_iova *rsiova, struct rlist_cpu *rcpu,
		struct rlist_dma *rdma,
		const struct rlist_dma_segmentation *segment,
		struct device *dev, dma_addr_t min_iova_pgsize, gfp_t gfp)
{
	struct rlist_cpu_entry entry;

	/*
	 * Since IOMMUs always require
	 *     phys % min_iova_pgsize == iova % min_iova_pgsize
	 * We can relate this to min_align_mask thusly:
	 *     phys & min_align_mask == iova & min_align_mask
	 *       when min_align_mask <= min_iova_pgsize
	 *
	 * Currently all uses of min_align_mask set a 4K-1 and all IOMMUs
	 * support at least 4K page size so this should never trigger.
	 */
	if (min_iova_pgsize & segment->min_align_mask)
		return -EINVAL;

	rlscpu_init(&rsiova->rls, rcpu);
	rsiova->pgsize = min_iova_pgsize;
	rsiova->segment = segment;
	rsiova->dev = dev;
	memset(&rsiova->p2pdma_cache, 0, sizeof(rsiova->p2pdma_cache));

	/*
	 * All the rlist_dma IOVA mapped entries are relative to a global base
	 * IOVA so we can adjust them all once we learn the allocated IOVA. Thus
	 * we can setup the rlist_dma before the IOMMU does any mapping.
	 */
	rdma->base = 0;

	if (!rlist_cpu_is_pagelist(rcpu) || rlist_cpu_has_p2pdma(rcpu) ||
	    !rlist_cpu_first(rcpu, &entry))
		return rsiova_init_slow(rsiova, rcpu, rdma, gfp);

	/*
	 * In the fast path we know the rcpu is layed out as a page list which
	 * means we can always linearly map it without any gaps. Thus the
	 * rlist_dma is always a single entry and we don't need to iterate.
	 */
	rsiova->cur_iova =
		ALIGN(rlist_cpu_length(rcpu) +
			      (rlist_cpu_entry_physical(&entry) % PAGE_SIZE),
		      entry.length);
	return rlist_dma_init_single(rdma, 0, rlist_cpu_length(rcpu), 0, gfp);
}

static void rsiova_fill_map(struct rlist_dma_state_iova *rsiova,
			    struct rlist_dma_iova_map *map,
			    struct rlist_cpu_entry *entry)
{
	int ret;

	/* Skip P2P entries that are set to RSIOVA_PRIV_P2P by rsiova_init() */
	while (true) {
		/* rsiova_init() already checked every entry. */
		ret = fill_p2p(rsiova, map, entry);
		if (WARN_ON(ret))
			goto end_out;
		if (map->phys != PHYS_ADDR_MAX)
			break;
		if (!rlscpu_next(&rsiova->rls, entry))
			goto end_out;
	}

	pad_iova(rsiova, map->phys, entry);
	fill_iova_map(rsiova, map, map->phys, entry->length);
	return;

end_out:
	map->length = 0;
}

void rsiova_first_map(struct rlist_dma_state_iova *rsiova,
		      struct rlist_dma_iova_map *map, dma_addr_t first_iova)
{
	struct rlist_cpu_entry entry;

	if (!rlscpu_reset(&rsiova->rls, &entry)) {
		map->length = 0;
		return;
	}
	rsiova->cur_iova = first_iova;
	rsiova_fill_map(rsiova, map, &entry);
}

void rsiova_next_map(struct rlist_dma_state_iova *rsiova,
		      struct rlist_dma_iova_map *map)
{
	struct rlist_cpu_entry entry;

	if (!rlscpu_next(&rsiova->rls, &entry)) {
		map->length = 0;
		return;
	}
	rsiova_fill_map(rsiova, map, &entry);
}

void rlsdma_first_unmap(struct rlist_dma_state *rls,
			struct rlist_dma_iova_map *map, dma_addr_t pgsize)
{
	struct rlist_dma_entry entry;

	if (!rlsdma_reset(rls, &entry)) {
		map->length = 0;
		return;
	}

	while (entry.dma_map_ops_priv == RSIOVA_PRIV_P2P) {
		if (!rlsdma_next(rls, &entry)) {
			map->length = 0;
			return;
		}
	}

	/* FIXME: this is too simple, the DMA list has IOVA that is non pgsize
	 * aligned so it has gaps, but once it is re-aligned it will usually
	 * become contiguous again. It would be good to gather it here. */
	map->iova = ALIGN_DOWN(entry.dma_address, pgsize);
	map->length = ALIGN(entry.length + entry.dma_address, pgsize) -
		      entry.dma_address;
}

void rlsdma_next_unmap(struct rlist_dma_state *rls,
		       struct rlist_dma_iova_map *map, dma_addr_t pgsize)
{
	struct rlist_dma_entry entry;

	do {
		if (!rlsdma_next(rls, &entry)) {
			map->length = 0;
			return;
		}
	} while (entry.dma_map_ops_priv == RSIOVA_PRIV_P2P);

	map->iova = ALIGN_DOWN(entry.dma_address, pgsize);
	map->length = ALIGN(entry.length + entry.dma_address, pgsize) -
		      entry.dma_address;
}
