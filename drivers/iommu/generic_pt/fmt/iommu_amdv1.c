// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
#define PT_FMT amdv1
#define PT_SUPPORTED_FEATURES (BIT(PT_FEAT_FULL_VA) | BIT(PT_FEAT_DYNAMIC_TOP))
#define PT_FORCE_FEATURES BIT(PT_FEAT_DYNAMIC_TOP)

#include "iommu_template.h"
