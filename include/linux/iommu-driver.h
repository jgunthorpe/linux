/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES
 *
 * This file should ONLY be included by iommu drivers. These API
 * calls are NOT to be used generally.
 */
#ifndef __LINUX_IOMMU_DRIVER_H
#define __LINUX_IOMMU_DRIVER_H

#ifndef CONFIG_IOMMU_API
#error "CONFIG_IOMMU_API is not set, should this header be included?"
#endif

#include <linux/types.h>
#include <linux/err.h>
#include <linux/slab.h>

struct of_phandle_args;
struct fwnode_handle;
struct iommu_device;
struct iommu_ops;

/*
 * FIXME this is sort of like container_of_safe() that was removed, do we want
 * to put it in the common header?
 */
#define container_of_err(ptr, type, member)                       \
	({                                                        \
		void *__mptr = (void *)(ptr);                     \
								  \
		(offsetof(type, member) != 0 && IS_ERR(__mptr)) ? \
			(type *)ERR_CAST(__mptr) :                \
			container_of(ptr, type, member);          \
	})

struct iommu_probe_info {
	struct device *dev;
	struct list_head *deferred_group_list;
	struct iommu_device *cached_iommu;
	struct device_node *of_master_np;
	const u32 *of_map_id;
	int (*get_u32_ids)(struct iommu_probe_info *pinf, u32 *ids);
	unsigned int num_ids;
	u32 cached_ids[8];
	bool defer_setup : 1;
	bool is_dma_configure : 1;
	bool is_acpi : 1;
	bool cached_single_iommu : 1;
};

static inline void iommu_fw_clear_cache(struct iommu_probe_info *pinf)
{
	pinf->num_ids = 0;
	pinf->cached_single_iommu = true;
}

int iommu_probe_device_pinf(struct iommu_probe_info *pinf);
struct iommu_device *iommu_device_from_fwnode(struct fwnode_handle *fwnode);
struct iommu_device *
iommu_device_from_fwnode_pinf(struct iommu_probe_info *pinf,
			      const struct iommu_ops *ops,
			      struct fwnode_handle *fwnode);
struct iommu_device *iommu_fw_finish_get_single(struct iommu_probe_info *pinf);

typedef int (*iommu_of_xlate_fn)(struct iommu_device *iommu,
				struct of_phandle_args *args, void *priv);
void iommu_of_allow_bus_probe(struct iommu_probe_info *pinf);

#if IS_ENABLED(CONFIG_OF_IOMMU)
void of_iommu_get_resv_regions(struct device *dev, struct list_head *list);

int iommu_of_xlate(struct iommu_probe_info *pinf, const struct iommu_ops *ops,
		   int num_cells, iommu_of_xlate_fn fn, void *priv);

struct iommu_device *__iommu_of_get_single_iommu(struct iommu_probe_info *pinf,
						 const struct iommu_ops *ops,
						 int num_cells);
#else
static inline void of_iommu_get_resv_regions(struct device *dev,
					     struct list_head *list)
{
}
static inline int iommu_of_xlate(struct iommu_probe_info *pinf,
				 const struct iommu_ops *ops, int num_cells,
				 iommu_of_xlate_fn fn, void *priv)
{
	return -ENODEV;
}
static inline
struct iommu_device *__iommu_of_get_single_iommu(struct iommu_probe_info *pinf,
						 const struct iommu_ops *ops,
						 int num_cells)
{
	return ERR_PTR(-ENODEV);
}
#endif

/**
 * iommu_of_get_single_iommu - Return the driver's iommu instance
 * @pinf: The iommu_probe_info
 * @ops: The ops the iommu instance must have
 * @num_cells: #iommu-cells value to enforce, -1 is no check
 * @drv_struct: The driver struct containing the struct iommu_device
 * @member: The name of the iommu_device member
 *
 * Parse the OF table describing the iommus and return a pointer to the driver's
 * iommu_device struct that the OF table points to. Check that the OF table is
 * well formed with a single iommu for all the entries and that the table refers
 * to this iommu driver. Integrates a container_of() to simplify all users.
 */
#define iommu_of_get_single_iommu(pinf, ops, num_cells, drv_struct, member)  \
	container_of_err(__iommu_of_get_single_iommu(pinf, ops, num_cells), \
			  drv_struct, member)

/**
 * iommu_of_num_ids - Return the number of iommu associations the FW has
 * @pinf: The iommu_probe_info
 *
 * For drivers using iommu_of_get_single_iommu() this will return the number
 * of ids associated with the iommu instance. For other cases this will return
 * the sum of all ids across all instances. Returns >= 1.
 */
static inline unsigned int iommu_of_num_ids(struct iommu_probe_info *pinf)
{
	return pinf->num_ids;
}

unsigned int iommu_of_num_ids(struct iommu_probe_info *pinf);
int iommu_fw_get_u32_ids(struct iommu_probe_info *pinf, u32 *ids);

static inline void iommu_fw_cache_id(struct iommu_probe_info *pinf, u32 id)
{
	if (pinf->num_ids < ARRAY_SIZE(pinf->cached_ids))
		pinf->cached_ids[pinf->num_ids] = id;
	pinf->num_ids++;
}

static inline void *
__iommu_fw_alloc_per_device_ids(struct iommu_probe_info *pinf, void *mem,
				unsigned int num_ids, unsigned int *num_ids_p,
				u32 *ids_p)
{
	int ret;

	if (!mem)
		return ERR_PTR(-ENOMEM);

	ret = iommu_fw_get_u32_ids(pinf, ids_p);
	if (ret) {
		kfree(mem);
		return ERR_PTR(ret);
	}

	*num_ids_p = num_ids;
	return mem;
}

/**
 * iommu_fw_alloc_per_device_ids - Allocate a per-device struct with ids
 * @pinf: The iommu_probe_info
 * @drv_struct: Name of a variable to a pointer of the driver structure
 *
 * Called by a driver during probe this helper allocates and initializes the
 * driver struct that embeds the ids array with the trailing members:
 *
 *	unsigned int num_ids;
 *	u32 ids[] __counted_by(num_ids);
 *
 * The helper allocates the driver struct with the right size flex array,
 * and initializes both members. Returns the driver struct or ERR_PTR.
 */
#define iommu_fw_alloc_per_device_ids(pinf, drv_struct)                    \
	({                                                                 \
		unsigned int num_ids = iommu_of_num_ids(pinf);             \
		typeof(drv_struct) drv;                                    \
                                                                           \
		drv = kzalloc(struct_size(drv, ids, num_ids), GFP_KERNEL); \
		drv = __iommu_fw_alloc_per_device_ids(                     \
			pinf, drv, num_ids, &drv->num_ids, drv->ids);      \
	})

/*
 * Used temporarily to indicate drivers that have moved to the new probe method.
 */
static inline int iommu_dummy_of_xlate(struct device *dev,
				       struct of_phandle_args *args)
{
	return 0;
}

#define __iommu_first(a, b)                              \
	({                                               \
		struct iommu_device *a_dev = a;          \
		a_dev != ERR_PTR(-ENODEV) ? a_dev : (b); \
	})

#if IS_ENABLED(CONFIG_ACPI_VIOT)
struct iommu_device *
__iommu_viot_get_single_iommu(struct iommu_probe_info *pinf,
			      const struct iommu_ops *ops);
#else
static inline struct iommu_device *
__iommu_viot_get_single_iommu(struct iommu_probe_info *pinf,
			      const struct iommu_ops *ops)
{
	return ERR_PTR(-ENODEV);
}
#endif
#define iommu_viot_get_single_iommu(pinf, ops, drv_struct, member)         \
	container_of_err(                                                  \
		__iommu_first(__iommu_viot_get_single_iommu(pinf, ops),    \
			      __iommu_of_get_single_iommu(pinf, ops, -1)), \
		drv_struct, member)

#endif
