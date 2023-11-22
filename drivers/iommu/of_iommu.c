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
		.of_master_np = master_np,
		.of_map_id = id,
		.is_dma_configure = true,
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

struct parse_info {
	struct iommu_probe_info *pinf;
	const struct iommu_ops *ops;
	int num_cells;
	iommu_of_xlate_fn xlate_fn;
	void *priv;
};

static struct iommu_device *parse_iommu(struct parse_info *info,
					struct of_phandle_args *iommu_spec)
{
	if (!of_device_is_available(iommu_spec->np))
		return ERR_PTR(-ENODEV);

	if (info->num_cells != -1 && iommu_spec->args_count != info->num_cells) {
		dev_err(info->pinf->dev,
			FW_BUG
			"Driver %ps expects number of cells %u but DT has %u\n",
			info->ops, info->num_cells, iommu_spec->args_count);
		return ERR_PTR(-EINVAL);
	}
	return iommu_device_from_fwnode_pinf(info->pinf, info->ops,
					     &iommu_spec->np->fwnode);
}

static int parse_single_iommu(struct of_phandle_args *iommu_spec, void *_info)
{
	struct parse_info *info = _info;
	struct iommu_device *iommu;

	iommu = parse_iommu(info, iommu_spec);
	if (IS_ERR(iommu))
		return PTR_ERR(iommu);
	info->pinf->num_ids++;
	return 0;
}

struct iommu_device *__iommu_of_get_single_iommu(struct iommu_probe_info *pinf,
						 const struct iommu_ops *ops,
						 int num_cells)
{
	struct parse_info info = { .pinf = pinf,
				   .ops = ops,
				   .num_cells = num_cells };
	int err;

	if (!pinf->is_dma_configure || !pinf->of_master_np)
		return ERR_PTR(-ENODEV);

	iommu_fw_clear_cache(pinf);
	err = of_iommu_for_each_id(pinf->dev, pinf->of_master_np,
				   pinf->of_map_id, parse_single_iommu, &info);
	if (err)
		return ERR_PTR(err);
	return iommu_fw_finish_get_single(pinf);
}
EXPORT_SYMBOL_GPL(__iommu_of_get_single_iommu);

static int parse_of_xlate(struct of_phandle_args *iommu_spec, void *_info)
{
	struct parse_info *info = _info;
	struct iommu_device *iommu;

	iommu = parse_iommu(info, iommu_spec);
	if (IS_ERR(iommu))
		return PTR_ERR(iommu);
	info->pinf->num_ids++;
	return info->xlate_fn(iommu, iommu_spec, info->priv);
}

/**
 * iommu_of_xlate - Parse all OF ids for an IOMMU
 * @pinf: The iommu_probe_info
 * @ops: The ops the iommu instance must have
 * @num_cells: #iommu-cells value to enforce, -1 is no check
 * @fn: Call for each Instance and ID
 * @priv: Opaque cookie for fn
 *
 * Drivers that support multiple iommu instances must call this function to
 * parse each instance from the OF table. fn will be called with the driver's
 * iommu_driver instance and the raw of_phandle_args that contains the ID.
 *
 * Drivers that need to parse a complex ID format should also use this function.
 */
int iommu_of_xlate(struct iommu_probe_info *pinf, const struct iommu_ops *ops,
		   int num_cells, iommu_of_xlate_fn fn, void *priv)
{
	struct parse_info info = { .pinf = pinf,
				   .ops = ops,
				   .num_cells = num_cells,
				   .xlate_fn = fn,
				   .priv = priv };

	pinf->num_ids = 0;
	return of_iommu_for_each_id(pinf->dev, pinf->of_master_np,
				    pinf->of_map_id, parse_of_xlate, &info);
}
EXPORT_SYMBOL_GPL(iommu_of_xlate);

/*
 * Temporary approach to allow drivers to opt into the bus probe. It configures
 * the iommu_probe_info to probe the dev->of_node. This is a bit hacky because
 * it mutates the iommu_probe_info and thus assumes there is only one op in the
 * system. Remove when we call probe from the bus always anyhow.
 */
void iommu_of_allow_bus_probe(struct iommu_probe_info *pinf)
{
	if (pinf->is_dma_configure)
		return;
	pinf->of_master_np = pinf->dev->of_node;
	pinf->is_dma_configure = true;
}
EXPORT_SYMBOL_GPL(iommu_of_allow_bus_probe);
