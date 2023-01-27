// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 *
 * Generic rlist support exists to allow old architectures that have not been
 * updated to modern frameworks, like dma-iommu.c, to work with rlists.
 *
 * Ignoring segmentation this is conceptually straightforward. Call the
 * map_page() for each entry in the rlist_cpu and return it as a DMA address.
 *
 * When mapping a dmap_map_ops implementation must not increase the number of
 * segments required from CPU to DMA. Meaning, each of the segmentation rules
 * has to be considered when DMA mapping and the resulting IOVA must not cause
 * additional segmentation.
 *
 * In the rlist system rlist_dma will perform segmentation for the driver when
 * the driver does the final iteration of the DMA addresses. This means we at
 * the dma_map_ops level we simply want to have maximally combined entries.
 *
 * This is trivial for all the rules except for segment_boundary_mask. Although
 * segment_boundary_mask is rarely used in the dma_map_ops algorithms there are
 * a few subtle implementation details that do make an impact. For the purposes
 * of rlist the only variable that matters is what IOVA was selected during
 * mapping. The ops tend to implement one of these strategies:
 *
 *  - Add large offsets to the physical address, eg for DAC windows or something,
 *    and assume these are larger than segment_boundary_mask. Ignore the
 *    value of segment_boundary_mask.
 *
 *  - Use a high enough order alignment for IOVA of each chunk, for instance
 *    align to the size when allocating IOVA. This ensures no additional
 *    segmentations. Ignore the value of segment_boundary_mask.
 *
 *  - Use iommu_area_alloc() which selects the right alignment for the IOVA,
 *    considering segment_boundary_mask as part of the alignment.
 *
 *  - Any 4k aligned IOVA and ignore segment_boundary_mask completely.
 *    Presumably the arch doesn't support any devices that require it, or maybe
 *    it is just broken.
 *
 * Figuring out which approach the dma_map_ops implementation is using is
 * necessary to comfirm these generic routines are going to work.
 */
#include <linux/rlist_dma.h>
#include <linux/dma-map-ops.h>

void generic_dma_unmap_rlist(struct device *dev, struct rlist_dma *rdma,
			     enum dma_data_direction dir, unsigned long attrs)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);
	struct rlist_dma_entry entry;
	RLIST_DMA_STATE(rls, rdma);

	rlist_dma_for_each_entry(&rls, &entry) {
		ops->unmap_page(dev, entry.dma_address, entry.length, dir,
				attrs);
	}
}

int generic_dma_map_rlist(struct device *dev, struct rlist_cpu *rcpu,
			  struct rlist_dma *rdma,
			  const struct rlist_dma_segmentation *segment,
			  enum dma_data_direction dir, unsigned long attrs,
			  gfp_t gfp)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);
	struct rlist_cpu_entry entry;
	RLIST_DMA_STATE_APPEND(rlsa, rdma);
	RLIST_CPU_STATE(rls, rcpu);
	dma_addr_t dma;
	int ret;

	ret = rlsdma_append_begin(&rlsa);
	if (ret)
		return ret;

	rlist_cpu_for_each_entry(&rls, &entry) {
		/* Old architectures do not get support for fancy stuff */
		if (entry.type != RLIST_CPU_MEM_FOLIO) {
			ret = -EREMOTEIO;
			goto err_unmap;
		}

		dma = ops->map_page(dev,
				    folio_page(entry.folio,
					       entry.folio_offset / PAGE_SIZE),
				    entry.folio_offset % PAGE_SIZE,
				    entry.length, dir, attrs);

		if (dma == DMA_MAPPING_ERROR) {
			ret = -EIO;
			goto err_unmap;
		}

		if (WARN_ON(!rlist_dma_segmentation_ok(segment, &entry, dma))) {
			ret = -EIO;
			goto err_unmap;
		}

		ret = rlsdma_append(&rlsa, dma, entry.length, 0, gfp);
		if (ret) {
			ops->unmap_page(dev, dma, entry.length, dir, attrs);
			goto err_unmap;
		}
	}
	rlsdma_append_end(&rlsa);
	return 0;

err_unmap:
	rlsdma_append_end(&rlsa);
	generic_dma_unmap_rlist(dev, rdma, dir, attrs);
	rlist_dma_destroy(rdma);
	return ret;
}

void generic_dma_sync_rlist_for_cpu(struct device *dev, struct rlist_dma *rdma,
				    enum dma_data_direction dir)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);
	struct rlist_dma_entry entry;
	RLIST_DMA_STATE(rls, rdma);

	rlist_dma_for_each_entry(&rls, &entry) {
		ops->sync_single_for_cpu(dev, entry.dma_address, entry.length,
					 dir);
	}
}

void generic_dma_sync_rlist_for_device(struct device *dev,
				       struct rlist_dma *rdma,
				       enum dma_data_direction dir)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);
	struct rlist_dma_entry entry;
	RLIST_DMA_STATE(rls, rdma);

	rlist_dma_for_each_entry(&rls, &entry) {
		ops->sync_single_for_device(dev, entry.dma_address,
					    entry.length, dir);
	}
}
