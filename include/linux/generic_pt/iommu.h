/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
#ifndef __GENERIC_PT_IOMMU_H
#define __GENERIC_PT_IOMMU_H

#include <linux/generic_pt/common.h>
#include <linux/mm_types.h>

struct pt_iommu_ops;
struct pt_iommu_flush_ops;

/**
 * DOC: IOMMU Radix Page Table
 *
 * The iommu implementation of the Generic Page Table provides an ops struct
 * that is useful to go with an iommu_domain to serve the DMA API, IOMMUFD and
 * the generic map/unmap interface.
 *
 * This interface uses a caller provided locking approach. The caller must have
 * a VA range lock concept that prevents concurrent threads from calling ops on
 * the same VA. Generally the range lock must be at least as large as a single
 * map call.
 */

/**
 * struct pt_iommu - Base structure for iommu page tables
 *
 * The format specific struct will include this as the first member.
 */
struct pt_iommu {
	/**
	 * @ops - Function pointers to access the API
	 */
	const struct pt_iommu_ops *ops;
	/**
	 * @hw_flush_ops - Function pointers provided by the HW driver to flush
	 * HW caches after changes to the page table.
	 */
	const struct pt_iommu_flush_ops *hw_flush_ops;

	/**
	 * @nid - Node ID to use for table memory allocations. The iommu driver
	 * may want to set the NID to the device's NID, if there are multiple
	 * table walkers.
	 */
	int nid;

	/**
	 * @iommu_device - Device pointer used for any DMA cache flushing when
	 * PT_FEAT_DMA_INCOHERENT.
	 */
	struct device *iommu_device;
};

/**
 * struct pt_iommu_info - Details about the iommu page table
 *
 * Returned from pt_iommu_ops->get_info()
 */
struct pt_iommu_info {
	/**
	 * @pgsize_bitmap - A bitmask where each set bit indicates
	 * a page size that can be natively stored in the page table.
	 */
	u64 pgsize_bitmap;
};

struct pt_iommu_ops {
	/**
	 * iova_to_phys() - Return the output address for the given IOVA
	 * @iommu_table: Table to query
	 * @iova: IO virtual address to query
	 *
	 * Determine the output address from the given IOVA. @iova may have any
	 * alignment, the returned physical will be adjusted with any sub page
	 * offset.
	 *
	 * Context: The caller must hold a read range lock that includes @iova.
	 *
	 * Return: 0 if there is no translation for the given iova.
	 */
	phys_addr_t (*iova_to_phys)(struct pt_iommu *iommu_table,
				    dma_addr_t iova);

	/**
	 * get_info() - Return the pt_iommu_info structure
	 * @iommu_table: Table to query
	 *
	 * Return some basic static information about the page table.
	 */
	void (*get_info)(struct pt_iommu *iommu_table,
			 struct pt_iommu_info *info);

	/**
	 * deinit() - Undo a format specific init operation
	 * @iommu_table: Table to destroy
	 *
	 * Release all of the memory. The caller must have already removed the
	 * table from all HW access and all caches.
	 */
	void (*deinit)(struct pt_iommu *iommu_table);
};

/**
 * struct pt_iommu_flush_ops - HW IOTLB cache flushing operations
 *
 * The IOMMU driver should implement these using container_of(iommu_table) to
 * get to it's iommu_domain dervied structure. All ops can be called in atomic
 * contexts as they are buried under DMA API calls.
 */
struct pt_iommu_flush_ops {
	/**
	 * flush_all() - Clear all caches related to this table
	 * @iommu_table: Table to flush
	 *
	 * Any gather can be concluded by calling flush_all.
	 */
	void (*flush_all)(struct pt_iommu *iommu_table);
};


static inline void pt_iommu_deinit(struct pt_iommu *iommu_table)
{
	iommu_table->ops->deinit(iommu_table);
}

/**
  * struct pt_iommu_cfg - Common configuration values for all formats
  */
struct pt_iommu_cfg {
	/**
	 * @domain - Initialize the page table related members for this domain
	 * pointer.
	 */
	struct iommu_domain *domain;
	/**
	 * @features - Features required. Only these features will be turned on.
	 * The feature list should reflect what the IOMMU HW is capable of.
	 */
	unsigned int features;
	/**
	 * @hw_max_vasz_lg2 - Maximum VA the IOMMU HW can support. This will
	 * imply the top level of the table.
	 */
	u8 hw_max_vasz_lg2;
	/**
	 * @hw_max_oasz_lg2 - Maximum OA the IOMMU HW can support. The format
	 * might select a lower maximum OA.
	 */
	u8 hw_max_oasz_lg2;
};

#endif
