// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 */
#ifndef LINUX_P2PDMA_PROVIDER_H
#define LINUX_P2PDMA_PROVIDER_H
#include <linux/types.h>
#include <linux/scatterlist.h>

/**
 * struct p2pdma_provider
 *
 * A P2DMa provider is a range of MMIO address space available to the CPU. This
 * struct describes how this address space maps to the struct device and bus
 * world so that the DMA API can understand how peer to peer memory DMA will
 * work with those MMIOs.
 */
struct p2pdma_provider {
	struct device *owner;
	u64 bus_offset;
};

/* Return code from p2pdma_provider_map*() functions */
enum {
	P2P_MAP_NORMALLY = 0,
	/* The dma_out was filled in with the IOVA to use */
	P2P_MAP_FILLED_DMA = 1,
};

struct p2pdma_provider_map_cache {
	struct p2pdma_provider *mem;
	unsigned int map;
};

static inline int p2pdma_provider_register(struct p2pdma_provider *provider,
					   struct device *owner)
{
	provider->owner = owner;
	return 0;
}

static inline void p2pdma_provider_unregister(struct p2pdma_provider *provider)
{
}

#ifdef CONFIG_PCI_P2PDMA
int p2pdma_provider_map(struct device *consumer,
			struct p2pdma_provider *provider, phys_addr_t base,
			dma_addr_t *dma_out,
			struct p2pdma_provider_map_cache *cache);
#else
static inline int p2pdma_provider_map(struct device *consumer,
				      struct p2pdma_provider *provider,
				      phys_addr_t base, dma_addr_t *dma_out,
				      struct p2pdma_provider_map_cache *cache)
{
	return -EREMOTEIO;
}
#endif

int __p2pdma_provider_map_page(struct device *consumer, struct page *page,
			       dma_addr_t *dma_out,
			       struct p2pdma_provider_map_cache *cache);

/**
 * p2pdma_provider_map - Help to map a physical address to a DMA address
 * @consumer: Device that will do DMA
 * @page: Starting page
 * @dma_out: DMA Address to use if P2P_MAP_FILLED_DMA is returned
 * @cache: Inter-call storage to reduce computation
 *
 * See p2pdma_provider_map(). This wrapper obtains the provider from a struct
 * page.
 */
static inline int
p2pdma_provider_map_page(struct device *consumer, struct page *page,
			 dma_addr_t *dma_out,
			 struct p2pdma_provider_map_cache *cache)
{
	if (!is_pci_p2pdma_page(page))
		return P2P_MAP_NORMALLY;

	return __p2pdma_provider_map_page(consumer, page, dma_out, cache);
}

/**
 * p2pdma_provider_map - Help to map a physical address to a DMA address
 * @consumer: Device that will do DMA
 * @sg: SG to map
 * @cache: Inter-call storage to reduce computation
 *
 * See p2pdma_provider_map(). This wrapper uses the struct page from the
 * scatterlist element and populates sg_dma/address/len/mark if
 * P2P_MAP_FILLED_DMA.
 */
static inline int
p2pdma_provider_map_sg(struct device *consumer, struct scatterlist *sg,
		       struct p2pdma_provider_map_cache *cache)
{
	int ret;

	ret = p2pdma_provider_map_page(consumer, sg_page(sg), &sg->dma_address,
				       cache);
	if (ret) {
		if (ret == P2P_MAP_FILLED_DMA) {
			sg_dma_len(sg) = sg->length;
			sg_dma_mark_bus_address(sg);
			sg->dma_address += sg->offset;
			return P2P_MAP_FILLED_DMA;
		}
		return ret;
	}
	return P2P_MAP_NORMALLY;
}

#endif
