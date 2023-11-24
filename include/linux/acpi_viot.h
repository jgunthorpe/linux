/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ACPI_VIOT_H__
#define __ACPI_VIOT_H__

#include <linux/acpi.h>

struct viot_iommu {
	/* Node offset within the table */
	unsigned int			offset;
	struct fwnode_handle		*fwnode;
	struct list_head		list;
};

typedef int (*viot_for_each_fn)(struct viot_iommu *viommu, u32 epid,
				void *info);
int viot_iommu_for_each_id(struct device *dev, viot_for_each_fn fn, void *info);

#ifdef CONFIG_ACPI_VIOT
void __init acpi_viot_early_init(void);
void __init acpi_viot_init(void);
int viot_iommu_configure(struct device *dev);
#else
static inline void acpi_viot_early_init(void) {}
static inline void acpi_viot_init(void) {}
static inline int viot_iommu_configure(struct device *dev)
{
	return -ENODEV;
}
#endif

#endif /* __ACPI_VIOT_H__ */
