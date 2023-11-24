// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES
 */
#include <linux/acpi_viot.h>
#include <linux/iommu.h>
#include <linux/iommu-driver.h>

struct parse_info {
	struct iommu_probe_info *pinf;
	const struct iommu_ops *ops;
	u32 *ids;
};

static int parse_single_iommu(struct viot_iommu *viommu, u32 epid, void *_info)
{
	struct fwnode_handle *fwnode = viommu->fwnode;
	struct parse_info *info = _info;
	struct iommu_probe_info *pinf = info->pinf;
	struct iommu_device *iommu;

	/* We're not translating ourself */
	if (device_match_fwnode(pinf->dev, fwnode))
		return -ENODEV;

	iommu = iommu_device_from_fwnode_pinf(pinf, info->ops, fwnode);
	if (IS_ERR(iommu)) {
		if (!IS_ENABLED(CONFIG_VIRTIO_IOMMU) &&
		    iommu == ERR_PTR(-EPROBE_DEFER))
			return -ENODEV;
		return PTR_ERR(iommu);
	}
	iommu_fw_cache_id(pinf, epid);
	return 0;
}

static int parse_read_ids(struct viot_iommu *viommu, u32 epid, void *_info)
{
	struct parse_info *info = _info;

	*info->ids = epid;
	(*info->ids)++;
	return 0;
}

static int viot_get_u32_ids(struct iommu_probe_info *pinf, u32 *ids)
{
	struct parse_info info = { .pinf = pinf, .ids = ids };

	return viot_iommu_for_each_id(pinf->dev, parse_read_ids, &info);
}

struct iommu_device *
__iommu_viot_get_single_iommu(struct iommu_probe_info *pinf,
			      const struct iommu_ops *ops)
{
	struct parse_info info = { .pinf = pinf, .ops = ops };
	int err;

	if (!pinf->is_dma_configure || !pinf->is_acpi)
		return ERR_PTR(-ENODEV);

	iommu_fw_clear_cache(pinf);
	err = viot_iommu_for_each_id(pinf->dev, parse_single_iommu, &info);
	if (err)
		return ERR_PTR(err);
	pinf->get_u32_ids = viot_get_u32_ids;
	return iommu_fw_finish_get_single(pinf);
}
EXPORT_SYMBOL(__iommu_viot_get_single_iommu);
