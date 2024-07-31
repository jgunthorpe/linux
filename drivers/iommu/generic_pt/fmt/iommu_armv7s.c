// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */

#define PT_FMT armv7s
/* FIXME BIT(PT_FEAT_OA_SIZE_CHANGE) | BIT(PT_FEAT_OA_TABLE_XCHG) */
#define PT_SUPPORTED_FEATURES (BIT(PT_FEAT_FULL_VA))

#include "iommu_template.h"
