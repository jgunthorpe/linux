/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2016, Semihalf
 *	Author: Tomasz Nowicki <tn@semihalf.com>
 */

#ifndef __ACPI_IORT_H__
#define __ACPI_IORT_H__

#include <linux/acpi.h>
#include <linux/fwnode.h>
#include <linux/irqdomain.h>

#define IORT_IRQ_MASK(irq)		(irq & 0xffffffffULL)
#define IORT_IRQ_TRIGGER_MASK(irq)	((irq >> 32) & 0xffffffffULL)

/*
 * PMCG model identifiers for use in smmu pmu driver. Please note
 * that this is purely for the use of software and has nothing to
 * do with hardware or with IORT specification.
 */
#define IORT_SMMU_V3_PMCG_GENERIC        0x00000000 /* Generic SMMUv3 PMCG */
#define IORT_SMMU_V3_PMCG_HISI_HIP08     0x00000001 /* HiSilicon HIP08 PMCG */
#define IORT_SMMU_V3_PMCG_HISI_HIP09     0x00000002 /* HiSilicon HIP09 PMCG */

int iort_register_domain_token(int trans_id, phys_addr_t base,
			       struct fwnode_handle *fw_node);
void iort_deregister_domain_token(int trans_id);
struct fwnode_handle *iort_find_domain_token(int trans_id);
int iort_pmsi_get_dev_id(struct device *dev, u32 *dev_id);

struct iort_params {
	unsigned int pasid_num_bits;
	bool dma_can_stall : 1;
	bool pci_rc_ats : 1;
};

typedef int (*iort_for_each_fn)(struct acpi_iort_node *iommu, u32 streamid,
				void *info);
int iort_iommu_for_each_id(struct device *dev, const u32 *id_in,
			   struct iort_params *params, iort_for_each_fn fn,
			   void *info);
struct fwnode_handle *iort_get_fwnode(struct acpi_iort_node *node);

#ifdef CONFIG_ACPI_IORT
u32 iort_msi_map_id(struct device *dev, u32 id);
struct irq_domain *iort_get_device_domain(struct device *dev, u32 id,
					  enum irq_domain_bus_token bus_token);
void acpi_configure_pmsi_domain(struct device *dev);
void iort_get_rmr_sids(struct fwnode_handle *iommu_fwnode,
		       struct list_head *head);
void iort_put_rmr_sids(struct fwnode_handle *iommu_fwnode,
		       struct list_head *head);
/* IOMMU interface */
int iort_dma_get_ranges(struct device *dev, u64 *size);
int iort_iommu_configure_id(struct device *dev, const u32 *id_in);
void iort_iommu_get_resv_regions(struct device *dev, struct list_head *head,
				 struct fwnode_handle *iommu_fwnode,
				 const u32 *fw_ids, unsigned int fw_num_ids);
phys_addr_t acpi_iort_dma_get_max_cpu_address(void);
#else
static inline u32 iort_msi_map_id(struct device *dev, u32 id)
{ return id; }
static inline struct irq_domain *iort_get_device_domain(
	struct device *dev, u32 id, enum irq_domain_bus_token bus_token)
{ return NULL; }
static inline void acpi_configure_pmsi_domain(struct device *dev) { }
static inline
void iort_get_rmr_sids(struct fwnode_handle *iommu_fwnode, struct list_head *head) { }
static inline
void iort_put_rmr_sids(struct fwnode_handle *iommu_fwnode, struct list_head *head) { }
/* IOMMU interface */
static inline int iort_dma_get_ranges(struct device *dev, u64 *size)
{ return -ENODEV; }
static inline int iort_iommu_configure_id(struct device *dev, const u32 *id_in)
{ return -ENODEV; }
static inline
void iort_iommu_get_resv_regions(struct device *dev, struct list_head *head,
				 struct fwnode_handle *iommu_fwnode,
				 const u32 *fw_ids, unsigned int fw_num_ids)
{ }

static inline phys_addr_t acpi_iort_dma_get_max_cpu_address(void)
{ return PHYS_ADDR_MAX; }
#endif

#endif /* __ACPI_IORT_H__ */
