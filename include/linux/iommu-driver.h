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

struct iommu_probe_info {
	struct device *dev;
	struct list_head *deferred_group_list;
	bool defer_setup : 1;
};

int iommu_probe_device_pinf(struct iommu_probe_info *pinf);

#endif
