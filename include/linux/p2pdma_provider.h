// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 */
#ifndef LINUX_P2PDMA_PROVIDER_H
#define LINUX_P2PDMA_PROVIDER_H
#include <linux/types.h>

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

static inline int p2pdma_provider_register(struct p2pdma_provider *provider,
					   struct device *owner)
{
	provider->owner = owner;
	return 0;
}

static inline void p2pdma_provider_unregister(struct p2pdma_provider *provider)
{
}
#endif
