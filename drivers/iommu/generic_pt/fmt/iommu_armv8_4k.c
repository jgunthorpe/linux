// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
#define PT_FMT armv8
#define PT_FMT_VARIANT 4k
#define PT_SUPPORTED_FEATURES                                   \
	(BIT(PT_FEAT_DMA_INCOHERENT) | BIT(PT_FEAT_ARMV8_LPA) | \
	 BIT(PT_FEAT_ARMV8_S2) | BIT(PT_FEAT_ARMV8_DBM) |       \
	 BIT(PT_FEAT_ARMV8_S2FWB))
#define ARMV8_GRANULE_SIZE 4096

#include "iommu_template.h"
