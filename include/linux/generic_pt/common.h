/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
#ifndef __GENERIC_PT_COMMON_H
#define __GENERIC_PT_COMMON_H

#include <linux/types.h>
#include <linux/build_bug.h>
#include <linux/bits.h>

/**
 * DOC: Generic Radix Page Table
 *
 * Generic Radix Page Table is a set of functions and helpers to efficiently
 * parse radix style page tables typically seen in HW implementations. The
 * interface is built to deliver similar code generation as the mm's pte/pmd/etc
 * system by fully inlining the exact code required to handle each level.
 *
 * Like the MM each format contributes its parsing implementation under common
 * names and the common code implements an algorithm.
 *
 * The system is divided into three logical levels:
 *  - The page table format and its accessors
 *  - Generic helpers to make
 *  - An implementation (eg IOMMU/DRM/KVM/MM)
 *
 * Multiple implementations are supported, the intention is to have the generic
 * format code be re-usable for whatever specalized implementation is required.
 */
struct pt_common {
	/**
	 * @top_of_table: Encodes the table top pointer and the top level in a
	 * single value. Must use READ_ONCE/WRITE_ONCE to access it. The lower
	 * bits of the aligned table pointer are used for the level.
	 */
	uintptr_t top_of_table;
	/**
	 * @oasz_lg2: Maximum number of bits the OA can contain. Upper bits must
	 * be zero. This may be less than what the page table format supports,
	 * but must not be more.
	 */
	u8 max_oasz_lg2;
	/**
	 * @vasz_lg2: Maximum number of bits the VA can contain. Upper bits are
	 * 0 or 1 depending on pt_full_va_prefix(). This may be less than what
	 * the page table format supports, but must not be more. When
	 * PT_FEAT_DYNAMIC_TOP this reflects the maximum VA capability.
	 */
	u8 max_vasz_lg2;
	unsigned int features;
};

enum {
	PT_TOP_LEVEL_BITS = 3,
	PT_TOP_LEVEL_MASK = GENMASK(PT_TOP_LEVEL_BITS - 1, 0),
};

enum {
	/*
	 * Cache flush page table memory before assuming the HW can read it.
	 * Otherwise a SMP release is sufficient for HW to read it.
	 */
	PT_FEAT_DMA_INCOHERENT,
	/*
	 * An OA entry can change size while still present. For instance an item
	 * can be up-sized to a contiguous entry, a contiguous entry down-sized
	 * to single items, or the size of a contiguous entry changed. Changes
	 * are hitless to ongoing translation. Otherwise an OA has to be made
	 * non present and flushed before it can be re-established with a new
	 * size.
	 */
	PT_FEAT_OA_SIZE_CHANGE,
	/*
	 * A non-contiguous OA entry can be converted to a populated table and
	 * vice versa while still present. For instance a OA with a high size
	 * can be replaced with a table mapping the same OA using a lower size.
	 * Assuming the table has the same translation as the OA then it is
	 * hitless to ongoing translation. Otherwise an OA or populated table
	 * can only be stored over a non-present item.
	 *
	 * Note this does not apply to tables which have entirely non present
	 * items. A non present table can be replaced with an OA or vice versa
	 * freely so long as nothing is made present without flushing.
	 */
	PT_FEAT_OA_TABLE_XCHG,
	/*
	 * The table can span the full VA range from 0 to PT_VADDR_MAX.
	 */
	PT_FEAT_FULL_VA,
	/*
	 * The table's top level can be increased dynamically during map. This
	 * requires HW support for atomically setting both the table top pointer
	 * and the starting table level.
	 */
	PT_FEAT_DYNAMIC_TOP,
	PT_FEAT_FMT_START,
};

struct pt_amdv1 {
	struct pt_common common;
};

struct pt_armv8 {
	struct pt_common common;
};

enum {
	/* Use the upper address space instead of lower */
	PT_FEAT_ARMV8_TTBR1 = PT_FEAT_FMT_START,
	/*
	 * Large Physical Address extension allows larger page sizes on 64k.
	 * Larger physical addresess are always supported
	 */
	PT_FEAT_ARMV8_LPA,
	/* Use the Stage 2 format instead of Stage 1 */
	PT_FEAT_ARMV8_S2,
	/* Use Dirty Bit Modifier, necessary for IOMMU dirty tracking */
	PT_FEAT_ARMV8_DBM,
	/* For S2 uses the Force Write Back coding of the S2MEMATTR */
	PT_FEAT_ARMV8_S2FWB,
	/* Set the NS and NSTable bits in all entries */
	PT_FEAT_ARMV8_NS,
};

#endif
