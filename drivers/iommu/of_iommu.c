// SPDX-License-Identifier: GPL-2.0-only
/*
 * OF helpers for IOMMU
 *
 * Copyright (c) 2012, NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/export.h>
#include <linux/iommu.h>
#include <linux/iommu-driver.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_iommu.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/fsl/mc.h>

static int of_iommu_xlate(struct of_phandle_args *iommu_spec, void *info)
{
	struct device *dev = info;
	struct iommu_device *iommu;
	struct fwnode_handle *fwnode = &iommu_spec->np->fwnode;
	int ret;

	iommu = iommu_device_from_fwnode(fwnode);
	if ((iommu && !iommu->ops->of_xlate) ||
	    !of_device_is_available(iommu_spec->np))
		return -ENODEV;

	ret = iommu_fwspec_init(dev, &iommu_spec->np->fwnode, iommu->ops);
	if (ret)
		return ret;
	/*
	 * The otherwise-empty fwspec handily serves to indicate the specific
	 * IOMMU device we're waiting for, which will be useful if we ever get
	 * a proper probe-ordering dependency mechanism in future.
	 */
	if (!iommu)
		return driver_deferred_probe_check_state(dev);

	ret = iommu->ops->of_xlate(dev, iommu_spec);
	return ret;
}

typedef int (*of_for_each_fn)(struct of_phandle_args *args, void *info);

static int __for_each_map_id(struct device_node *master_np, u32 id,
			     of_for_each_fn fn, void *info)
{
	struct of_phandle_args iommu_spec = { .args_count = 1 };
	int err;

	err = of_map_id(master_np, id, "iommu-map",
			 "iommu-map-mask", &iommu_spec.np,
			 iommu_spec.args);
	if (err)
		return err;

	err = fn(&iommu_spec, info);
	of_node_put(iommu_spec.np);
	return err;
}

static int __for_each_iommus(struct device_node *master_np, of_for_each_fn fn,
			     void *info)
{
	struct of_phandle_args iommu_spec;
	int err = -ENODEV, idx = 0;

	while (!of_parse_phandle_with_args(master_np, "iommus",
					   "#iommu-cells",
					   idx, &iommu_spec)) {
		err = fn(&iommu_spec, info);
		of_node_put(iommu_spec.np);
		idx++;
		if (err)
			break;
	}

	return err;
}

struct of_pci_iommu_alias_info {
	struct device_node *np;
	of_for_each_fn fn;
	void *info;
};

static int __for_each_map_pci(struct pci_dev *pdev, u16 alias, void *data)
{
	struct of_pci_iommu_alias_info *pci_info = data;

	return __for_each_map_id(pci_info->np, alias, pci_info->fn,
				 pci_info->info);
}

static int of_iommu_for_each_id(struct device *dev,
				struct device_node *master_np, const u32 *id,
				of_for_each_fn fn, void *info)
{
	/*
	 * We don't currently walk up the tree looking for a parent IOMMU.
	 * See the `Notes:' section of
	 * Documentation/devicetree/bindings/iommu/iommu.txt
	 */
	if (dev_is_pci(dev)) {
		struct of_pci_iommu_alias_info pci_info = {
			.np = master_np,
			.fn = fn,
			.info = info,
		};

		/* In PCI mode the ID comes from the RID */
		if (WARN_ON(id))
			return -EINVAL;

		return pci_for_each_dma_alias(to_pci_dev(dev),
					     __for_each_map_pci, &pci_info);
	}

	if (id)
		return __for_each_map_id(master_np, *id, fn, info);
	return __for_each_iommus(master_np, fn, info);
}

/*
 * Returns:
 *  0 on success, an iommu was configured
 *  -ENODEV if the device does not have any IOMMU
 *  -EPROBEDEFER if probing should be tried again
 *  -errno fatal errors
 */
int of_iommu_configure(struct device *dev, struct device_node *master_np,
		       const u32 *id)
{
	struct iommu_probe_info pinf = {
		.dev = dev,
	};
	struct iommu_fwspec *fwspec;
	int err;

	if (!master_np)
		return -ENODEV;

	/* Serialise to make dev->iommu stable under our potential fwspec */
	mutex_lock(&iommu_probe_device_lock);
	fwspec = dev_iommu_fwspec_get(dev);
	if (fwspec) {
		if (fwspec->ops) {
			mutex_unlock(&iommu_probe_device_lock);
			return 0;
		}
		/* In the deferred case, start again from scratch */
		iommu_fwspec_free(dev);
	}

	if (dev_is_pci(dev))
		pci_request_acs();

	err = of_iommu_for_each_id(dev, master_np, id, of_iommu_xlate, dev);
	mutex_unlock(&iommu_probe_device_lock);
	if (err == -ENODEV || err == -EPROBE_DEFER)
		return err;
	if (err)
		goto err_log;

	err = iommu_probe_device_pinf(&pinf);
	if (err)
		goto err_log;
	return 0;

err_log:
	dev_dbg(dev, "Adding to IOMMU failed: %pe\n", ERR_PTR(err));
	return err;
}

static enum iommu_resv_type __maybe_unused
iommu_resv_region_get_type(struct device *dev,
			   struct resource *phys,
			   phys_addr_t start, size_t length)
{
	phys_addr_t end = start + length - 1;

	/*
	 * IOMMU regions without an associated physical region cannot be
	 * mapped and are simply reservations.
	 */
	if (phys->start >= phys->end)
		return IOMMU_RESV_RESERVED;

	/* may be IOMMU_RESV_DIRECT_RELAXABLE for certain cases */
	if (start == phys->start && end == phys->end)
		return IOMMU_RESV_DIRECT;

	dev_warn(dev, "treating non-direct mapping [%pr] -> [%pap-%pap] as reservation\n", &phys,
		 &start, &end);
	return IOMMU_RESV_RESERVED;
}

/**
 * of_iommu_get_resv_regions - reserved region driver helper for device tree
 * @dev: device for which to get reserved regions
 * @list: reserved region list
 *
 * IOMMU drivers can use this to implement their .get_resv_regions() callback
 * for memory regions attached to a device tree node. See the reserved-memory
 * device tree bindings on how to use these:
 *
 *   Documentation/devicetree/bindings/reserved-memory/reserved-memory.txt
 */
void of_iommu_get_resv_regions(struct device *dev, struct list_head *list)
{
#if IS_ENABLED(CONFIG_OF_ADDRESS)
	struct of_phandle_iterator it;
	int err;

	if (!dev->of_node)
		return;

	of_for_each_phandle(&it, err, dev->of_node, "memory-region", NULL, 0) {
		const __be32 *maps, *end;
		struct resource phys;
		int size;

		memset(&phys, 0, sizeof(phys));

		/*
		 * The "reg" property is optional and can be omitted by reserved-memory regions
		 * that represent reservations in the IOVA space, which are regions that should
		 * not be mapped.
		 */
		if (of_find_property(it.node, "reg", NULL)) {
			err = of_address_to_resource(it.node, 0, &phys);
			if (err < 0) {
				dev_err(dev, "failed to parse memory region %pOF: %d\n",
					it.node, err);
				continue;
			}
		}

		maps = of_get_property(it.node, "iommu-addresses", &size);
		if (!maps)
			continue;

		end = maps + size / sizeof(__be32);

		while (maps < end) {
			struct device_node *np;
			u32 phandle;

			phandle = be32_to_cpup(maps++);
			np = of_find_node_by_phandle(phandle);

			if (np == dev->of_node) {
				int prot = IOMMU_READ | IOMMU_WRITE;
				struct iommu_resv_region *region;
				enum iommu_resv_type type;
				phys_addr_t iova;
				size_t length;

				if (of_dma_is_coherent(dev->of_node))
					prot |= IOMMU_CACHE;

				maps = of_translate_dma_region(np, maps, &iova, &length);
				type = iommu_resv_region_get_type(dev, &phys, iova, length);

				region = iommu_alloc_resv_region(iova, length, prot, type,
								 GFP_KERNEL);
				if (region)
					list_add_tail(&region->list, list);
			}
		}
	}
#endif
}
EXPORT_SYMBOL(of_iommu_get_resv_regions);
