// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES
 */
#include <linux/acpi_iort.h>
#include <acpi/actbl2.h>

#include <linux/iommu.h>
#include <linux/iommu-driver.h>

struct parse_info {
	struct iommu_probe_info *pinf;
	const struct iommu_ops *ops;
	u32 *ids;
};

static bool iort_iommu_driver_enabled(struct iommu_probe_info *pinf, u8 type)
{
	switch (type) {
	case ACPI_IORT_NODE_SMMU_V3:
		return IS_ENABLED(CONFIG_ARM_SMMU_V3);
	case ACPI_IORT_NODE_SMMU:
		return IS_ENABLED(CONFIG_ARM_SMMU);
	default:
		dev_warn(pinf->dev,
			 FW_WARN
			 "IORT node type %u does not describe an SMMU\n",
			 type);
		return false;
	}
}

static int parse_single_iommu(struct acpi_iort_node *iort_iommu, u32 streamid,
			      void *_info)
{
	struct parse_info *info = _info;
	struct iommu_probe_info *pinf = info->pinf;
	struct fwnode_handle *fwnode;
	struct iommu_device *iommu;

	fwnode = iort_get_fwnode(iort_iommu);
	if (!fwnode)
		return -ENODEV;

	iommu = iommu_device_from_fwnode_pinf(pinf, info->ops, fwnode);
	if (IS_ERR(iommu)) {
		if (iommu == ERR_PTR(-EPROBE_DEFER) &&
		    !iort_iommu_driver_enabled(pinf, iort_iommu->type))
			return -ENODEV;
		return PTR_ERR(iommu);
	}
	iommu_fw_cache_id(pinf, streamid);
	return 0;
}

static int parse_read_ids(struct acpi_iort_node *iommu, u32 streamid,
			  void *_info)
{
	struct parse_info *info = _info;

	*info->ids = streamid;
	(*info->ids)++;
	return 0;
}

static int iort_get_u32_ids(struct iommu_probe_info *pinf, u32 *ids)
{
	struct parse_info info = { .pinf = pinf, .ids = ids };
	struct iort_params params;

	return iort_iommu_for_each_id(pinf->dev, pinf->acpi_map_id, &params,
				      parse_read_ids, &info);
}

struct iommu_device *
__iommu_iort_get_single_iommu(struct iommu_probe_info *pinf,
			      const struct iommu_ops *ops,
			      struct iort_params *params)
{
	struct parse_info info = { .pinf = pinf, .ops = ops };
	struct iort_params unused_params;
	int err;

	if (!pinf->is_dma_configure || !pinf->is_acpi)
		return ERR_PTR(-ENODEV);

	if (!params)
		params = &unused_params;

	iommu_fw_clear_cache(pinf);
	err = iort_iommu_for_each_id(pinf->dev, pinf->acpi_map_id, params,
				     parse_single_iommu, &info);
	if (err)
		return ERR_PTR(err);
	pinf->get_u32_ids = iort_get_u32_ids;
	return iommu_fw_finish_get_single(pinf);
}
EXPORT_SYMBOL(__iommu_iort_get_single_iommu);
