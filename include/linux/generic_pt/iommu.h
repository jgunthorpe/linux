/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
#ifndef __GENERIC_PT_IOMMU_H
#define __GENERIC_PT_IOMMU_H

#include <linux/generic_pt/common.h>
#include <linux/mm_types.h>

struct iommu_iotlb_gather;
struct pt_iommu_ops;
struct pt_iommu_flush_ops;
struct iommu_dirty_bitmap;

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
	 * map_range() - Install translation for an IOVA range
	 * @iommu_table: Table to manipulate
	 * @iova: IO virtual address to start
	 * @paddr: Physical/Output address to start
	 * @len: Length of the range starting from @iova
	 * @prot: A bitmap of IOMMU_READ/WRITE/CACHE/NOEXEC/MMIO
	 * @gfp: GFP flags for any memory allocations
	 * @gather: Gather struct that must be flushed on return
	 *
	 * The range starting at IOVA will have paddr installed into it. The
	 * rage is automatically segmented into optimally sized table entries,
	 * and can have any valid alignment.
	 *
	 * On error the caller will probably want to invoke unmap on the range
	 * from iova up to the amount indicated by @mapped to return the table
	 * back to an unchanged state.
	 *
	 * Context: The caller must hold a write range lock that includes
	 * the whole range.
	 *
	 * Returns: -ERRNO on failure, 0 on success. The number of bytes of VA
	 * that were mapped are added to @mapped, @mapped is not zerod first.
	 */
	int (*map_range)(struct pt_iommu *iommu_table, dma_addr_t iova,
			 phys_addr_t paddr, dma_addr_t len, unsigned int prot,
			 gfp_t gfp, size_t *mapped,
			 struct iommu_iotlb_gather *iotlb_gather);

	/**
	 * unmap_range() - Make a range of IOVA empty/not present
	 * @iommu_table: Table to manipulate
	 * @iova: IO virtual address to start
	 * @len: Length of the range starting from @iova
	 * @gather: Gather struct that must be flushed on return
	 *
	 * unmap_range() will remove translation created by map_range(). It
	 * cannot subdivide a mapping created by map_range(), so it should be
	 * called with IOVA ranges that match those passed to map_pages. The
	 * IOVA range can aggregate contiguous map_range() calls so long as no
	 * individual range is split.
	 *
	 * Context: The caller must hold a write range lock that includes
	 * the whole range.
	 *
	 * Returns: Number of bytes of VA unmapped. iova + res will be the
	 * point unmapping stopped.
	 */
	size_t (*unmap_range)(struct pt_iommu *iommu_table, dma_addr_t iova,
			      dma_addr_t len,
			      struct iommu_iotlb_gather *iotlb_gather);

	/**
	 * cut_mapping() - Split a mapping
	 * @iommu_table: Table to manipulate
	 * @iova: IO virtual address to cut at
	 * @gfp: GFP flags for any memory allocations
	 *
	 * If map was used on [iova_a, iova_b] then unmap must be used on the
	 * same interval. When called twice this is useful to unmap a portion of
	 * a larger mapping.
	 *
	 * cut_mapping() changes the page table so that umap of both:
	 *    [iova_a, iova_c - 1]
	 *    [iova_c, iova_b]
	 * will work.
	 *
	 * In practice this is done by breaking up large pages into smaller
	 * pages so that no large page crosses iova_c.
	 *
	 * cut_mapping() works to ensure all page sizes that don't cross the cut
	 * remain at the optimal sizes.
	 *
	 * Context: The caller must hold a write range lock that includes the
	 * entire range used with the map that contains iova.
	 *
	 * Returns: -ERRNO on failure, 0 on success.
	 */
	int (*cut_mapping)(struct pt_iommu *iommu_table, dma_addr_t cut_iova,
			   gfp_t gfp);

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
	 * read_and_clear_dirty() - Manipulate the HW set write dirty state
	 * @iommu_table: Table to manipulate
	 * @iova: IO virtual address to start
	 * @size: Length of the IOVA
	 * @flags: A bitmap of IOMMU_DIRTY_NO_CLEAR
	 *
	 * Iterate over all the entries in the mapped range and record their
	 * write dirty status in iommu_dirty_bitmap. If IOMMU_DIRTY_NO_CLEAR is
	 * not specified then the entries will be left dirty, otherwise they are
	 * returned to being not write dirty.
	 *
	 * Context: The caller must hold a read range lock that includes @iova.
	 *
	 * Returns: -ERRNO on failure, 0 on success.
	 */
	int (*read_and_clear_dirty)(struct pt_iommu *iommu_table,
				    dma_addr_t iova, dma_addr_t len,
				    unsigned long flags,
				    struct iommu_dirty_bitmap *dirty_bitmap);

	/**
	 * set_dirty() - Make the iova write dirty
	 * @iommu_table: Table to manipulate
	 * @iova: IO virtual address to start
	 *
	 * This is only used by iommufd testing. It makes the iova dirty so that
	 * read_and_clear_dirty() will see it as dirty. Unlike all the other ops
	 * this one is safe to call without holding any locking. It may return
	 * -EAGAIN if there is a race.
	 */
	int (*set_dirty)(struct pt_iommu *iommu_table, dma_addr_t iova);

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

	/**
	 * change_top() - Update the top of table pointer
	 * @iommu_table: Table to operate on
	 * @top_paddr: New CPU physical address of the top pointer
	 * @top_level: IOMMU PT level of the new top
	 *
	 * Called under the get_top_lock() spinlock. The driver must update all
	 * HW references to this domain with a new top address and
	 * configuration. On return mappings placed in the new top must be
	 * reachable by the HW.
	 *
	 * top_level encodes the level in IOMMU PT format, level 0 is the
	 * smallest page size increasing from there. This has to be translated
	 * to any HW specific format. During this call the new top will not be
	 * visible to any other API.
	 *
	 * This op is only used by PT_FEAT_DYNAMIC_TOP, and is required if
	 * enabled.
	 */
	void (*change_top)(struct pt_iommu *iommu_table, phys_addr_t top_paddr,
			   unsigned int top_level);
	/**
	 * get_top_lock() - Return a lock to hold when changing the table top
	 * @iommu_table: Table to operate on
	 *
	 * page table from being stored in HW. The lock will be held prior
	 * to calling change_top() and released once the top is fully visible.
	 *
	 * Typically this would be a lock that protects the iommu_domain's
	 * attachment list.
	 *
	 * This op is only used by PT_FEAT_DYNAMIC_TOP, and is required if
	 * enabled.
	 */
	spinlock_t *(*get_top_lock)(struct pt_iommu *iommu_table);
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

struct pt_iommu_amdv1 {
	struct pt_iommu iommu;
	struct pt_amdv1 amdpt;
};

struct pt_iommu_amdv1_cfg {
	struct pt_iommu_cfg common;
	unsigned int starting_level;
};

struct pt_iommu_amdv1_hw_info {
	u64 host_pt_root;
	u8 mode;
};

int pt_iommu_amdv1_init(struct pt_iommu_amdv1 *table,
			const struct pt_iommu_amdv1_cfg *cfg, gfp_t gfp);
void pt_iommu_amdv1_hw_info(struct pt_iommu_amdv1 *table,
			    struct pt_iommu_amdv1_hw_info *info);
int pt_iommu_amdv1_mock_init(struct pt_iommu_amdv1 *table,
			     const struct pt_iommu_amdv1_cfg *cfg, gfp_t gfp);
void pt_iommu_amdv1_mock_hw_info(struct pt_iommu_amdv1 *table,
				 struct pt_iommu_amdv1_hw_info *info);

struct pt_iommu_armv7s {
	struct pt_iommu iommu;
	struct pt_armv7s armpt;
};

struct pt_iommu_armv7s_cfg {
	struct pt_iommu_cfg common;
};

int pt_iommu_armv7s_init(struct pt_iommu_armv7s *table,
			 const struct pt_iommu_armv7s_cfg *cfg, gfp_t gfp);

struct pt_iommu_armv8 {
	struct pt_iommu iommu;
	struct pt_armv8 armpt;
};

struct pt_iommu_armv8_cfg {
	struct pt_iommu_cfg common;
};

int pt_iommu_armv8_4k_init(struct pt_iommu_armv8 *table,
			   const struct pt_iommu_armv8_cfg *cfg, gfp_t gfp);
int pt_iommu_armv8_16k_init(struct pt_iommu_armv8 *table,
			    const struct pt_iommu_armv8_cfg *cfg, gfp_t gfp);
int pt_iommu_armv8_64k_init(struct pt_iommu_armv8 *table,
			    const struct pt_iommu_armv8_cfg *cfg, gfp_t gfp);

static size_t __pt_iommu_armv8_granuals_to_lg2(size_t granual_sizes)
{
	size_t supported_granuals = 0;

	if (IS_ENABLED(CONFIG_IOMMU_PT_ARMV8_4K))
		supported_granuals |= BIT(12);
	if (IS_ENABLED(CONFIG_IOMMU_PT_ARMV8_16K))
		supported_granuals |= BIT(14);
	if (IS_ENABLED(CONFIG_IOMMU_PT_ARMV8_64K))
		supported_granuals |= BIT(16);

	granual_sizes &= supported_granuals;
	if (!granual_sizes)
		return 0;

	/* Prefer the CPU page size if possible */
	if (granual_sizes & PAGE_SIZE)
		return PAGE_SHIFT;

	/*
	 * Otherwise prefer the largest page size smaller than the CPU page
	 * size
	 */
	if (granual_sizes % PAGE_SIZE)
		return ilog2(rounddown_pow_of_two(granual_sizes % PAGE_SIZE));

	/* Otherwise use the smallest page size available */
	return __ffs(granual_sizes);
}

static inline int pt_iommu_armv8_init(struct pt_iommu_armv8 *table,
				      const struct pt_iommu_armv8_cfg *cfg,
				      size_t granual_sizes, gfp_t gfp)
{
	switch (__pt_iommu_armv8_granuals_to_lg2(granual_sizes)) {
	case 12:
		if (!IS_ENABLED(CONFIG_IOMMU_PT_ARMV8_4K))
			return -EOPNOTSUPP;
		return pt_iommu_armv8_4k_init(table, cfg, gfp);
	case 14:
		if (!IS_ENABLED(CONFIG_IOMMU_PT_ARMV8_16K))
			return -EOPNOTSUPP;
		return pt_iommu_armv8_16k_init(table, cfg, gfp);
	case 16:
		if (!IS_ENABLED(CONFIG_IOMMU_PT_ARMV8_64K))
			return -EOPNOTSUPP;
		return pt_iommu_armv8_64k_init(table, cfg, gfp);
	default:
		return -EOPNOTSUPP;
	}
}

struct pt_iommu_dart {
	struct pt_iommu iommu;
	struct pt_dart dartpt;
};

struct pt_iommu_dart_cfg {
	struct pt_iommu_cfg common;
	u64 pgsize_bitmap;
};
int pt_iommu_dart_init(struct pt_iommu_dart *table,
		       const struct pt_iommu_dart_cfg *cfg, gfp_t gfp);

struct pt_iommu_x86pae {
	struct pt_iommu iommu;
	struct pt_x86pae x86pae_pt;
};

struct pt_iommu_x86pae_cfg {
	struct pt_iommu_cfg common;
};

struct pt_iommu_x86pae_hw_info {
	u64 gcr3_pt;
	u8 levels;
};

int pt_iommu_x86pae_init(struct pt_iommu_x86pae *table,
			 const struct pt_iommu_x86pae_cfg *cfg, gfp_t gfp);
void pt_iommu_x86pae_hw_info(struct pt_iommu_x86pae *table,
			     struct pt_iommu_x86pae_hw_info *info);

#endif
