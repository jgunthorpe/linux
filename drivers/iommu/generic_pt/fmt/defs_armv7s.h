/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 *
 */
#ifndef __GENERIC_PT_FMT_DEFS_ARMV7S_H
#define __GENERIC_PT_FMT_DEFS_ARMV7S_H

#include <linux/generic_pt/common.h>
#include <linux/types.h>

typedef u32 pt_vaddr_t;
typedef u64 pt_oaddr_t;

struct armv7s_pt_write_attrs {
	u32 pte1;
	u32 pte2;
	u32 pte2l;
	gfp_t gfp;
};
#define pt_write_attrs armv7s_pt_write_attrs

#endif
