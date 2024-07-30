/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 *
 */
#ifndef __GENERIC_PT_FMT_DEFS_DART_H
#define __GENERIC_PT_FMT_DEFS_DART_H

#include <linux/generic_pt/common.h>
#include <linux/types.h>

typedef u64 pt_vaddr_t;
typedef u64 pt_oaddr_t;

struct dart_pt_write_attrs {
	u64 descriptor_bits;
	gfp_t gfp;
};
#define pt_write_attrs dart_pt_write_attrs

#endif
