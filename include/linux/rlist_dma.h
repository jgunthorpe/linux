// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 */
#ifndef __LINUX_RLIST_DMA_H
#define __LINUX_RLIST_DMA_H
#include <linux/rlist_cpu.h>
#include <linux/xarray.h>
#include <linux/p2pdma_provider.h>

/**
 * DOC: DMA Range List
 *
 * Provide a general data structure for storing dma_addr_t ranges without
 * exposing details of the storage layer to the caller.
 *
 * This is intended to be an alternative to the dma list embedded in the
 * scatterlist as part of the DMA API. Keeping the DMA addresses type-seperated
 * from CPU addresses is a desirable design.
 *
 * The ranges provide an opaque 32 bit "dma_map_ops_priv" which can be used by
 * the dma_map_ops implementation to store data between map and unmap calls.
 *
 * As a special case the DMA Range List can be pointed at a rlist_cpu, in this
 * mode the iterators extract a phys_addr_t from the rlist_cpu entries and
 * return it 1:1 as a dma_addr_t. This allows a common case of the direct
 * mapping to avoid allocations and work.
 */

enum rlist_dma_types {
	/* Use rlist_dma->base as an offset for every dma_address */
	RLIST_DMA_RELATIVE = 0,
	RLIST_DMA_ABSOLUTE = 1,
};

struct rlist_dma_entry {
	union {
		struct rlist_entry _entry;
		struct rlist_cpu_entry _cpu;
		struct {
			u64 _reserved1 : 2;
			u64 length : 60;
			union {
				dma_addr_t dma_address;
				u64 _base;
			};
			u32 _reserved2;
			u32 dma_map_ops_priv;
		};
	};
};

/**
 * struct rlist_dma - Container for DMA memory ranges
 *
 * This is the struct that HW would use to refer to DMA mapped memory ranges
 * prior to programming them into HW. It is output from the DMA API.
 * Each entry is a struct rlist_dma_entry, with dma_addrss and length visible to
 * drivers.
 */
struct rlist_dma {
	/* private */

	/*
	 * If rlist is empty then the DMA list is "identity mapped" and the DMA
	 * addresses are derived from the cpu list. Otherwise rlist contains the
	 * unique addresses required.
	 */
	struct rlist rlist;
	struct rlist_cpu *cpu;
	dma_addr_t base;
};

void rlist_dma_init(struct rlist_dma *rdma);
void rlist_dma_init_identity_cpu(struct rlist_dma *rdma,
				 struct rlist_cpu *rcpu);
void rlist_dma_destroy(struct rlist_dma *rdma);

static inline bool rlist_dma_empty(struct rlist_dma *rdma)
{
	if (rdma->cpu)
		return rlist_cpu_empty(rdma->cpu);
	return rlist_empty(&rdma->rlist);
}

/* Sum of length from every entry in the list */
static inline u64 rlist_dma_length(struct rlist_dma *rdma)
{
	return rlist_cpu_length(rdma->cpu);
}

/*
 * Return the rlist_cpu that was associated with the rlist_dma when it was DMA
 * mapped.
 */
static inline struct rlist_cpu *rdma_get_source_rcpu(struct rlist_dma *rdma)
{
	return rdma->cpu;
}

/*
 * A segmentation limit is a HW restriction that limits the kind of entry that
 * HW can program. For instance a HW device may be able to do arbitary
 * scatter/gather lists but may not be able to program more than 16 bits of
 * length.
 *
 * Combined with a total limit on the number of HW S/G entries per operation it
 * is necessary to manage the total number of HW S/G entries, possibly starting
 * from the moment the rlist_cpu is formed.
 *
 * rlist handles segmentation limits differently than scatterlist. We do not
 * force segmentation into the storage, so raw rlist_cpu_entry should always be
 * assumed to be maximually coalesced.
 *
 * Intead the rlist_dma_segmentation struct specifies the requirement and we can
 * keep track of it when:
 *   - The rlist_cpu is filled, count how many segments each entry uses and
 *     limit the total number of segments
 *   - The DMA API does mapping, ensure that the number of segments does not
 *     increase.
 *   - During HW programming, break up the raw entries into HW segments,
 *     similar to how we break up into folios/pages
 *
 * This struct defines all the segmentation limitations for rlist.
 *
 * FIXME: Very similar to struct device_dma_parameters ??
 */
/*
 * Describe HW requirements for segmenting the rlist_dma for HW programming.
 * rlist_dma supports the two main forms of HW programming: scatter/gather lists
 * and page lists.
 *
 * In rlist_dma segmentation is something that happens during iteration. The
 * segment boundaries are not stored in the storage. The desired segmentation
 * parameters are pushed into various places to increase efficiency by:
 *   - Pre-computing segmentation related information while populating
 *   - Allowing the DMA API to assign dma_addr to increase segmentation
 *     efficiency
 *   - Retain the DMA API SG approach of not increasing the number of segments
 *     during mapping
 *
 * scatter/gather segmentation fits HW that has a byte granular starting/length
 * pair to describe each segment.
 *
 * page (block) list segmentation fits HW that works in uniform fixed size and
 * aligned segments. The HW input is usually only an address with the length
 * implied.
 */
struct rlist_dma_segmentation
{
	/* cpu_addr & min_align_mask == dma_addr & min_align_mask */
	unsigned long min_align_mask;
	/* addr & segment_boundary_mask == (addr + length) & segment_boundary_mask */
	unsigned long segment_boundary_mask;
	/* length <= max_segment_size */
	unsigned long max_segment_size;

	/*
	 * If non-zero the HW wants to do a 'block list' map not a 'scatter
	 * list' map. Above parameters are ignored. Each bit indicates a page
	 * size supported by HW. PAGE_SHIFT or lower must be set.
	 */
	u64 block_list_supported : 62;
	u64 has_block_list_hwva : 1;

	/*
	 * For page table HW specify the HW's virtual address base that must
	 * pass through to the dma_addr_t. This allows the IOVA assingment to
	 * adjust the initial IOVA offset to allow higher page sizes.
	 */
	dma_addr_t block_list_hwva;
};

/* HW can work with any rlist_dma_entry */
extern const struct rlist_dma_segmentation rlist_no_segmentation;

size_t rlist_dma_num_segments(const struct rlist_dma_segmentation *segment,
			      const struct rlist_dma_entry *entry);
static inline size_t
rlist_cpu_num_segments(const struct rlist_dma_segmentation *segment,
		       const struct rlist_cpu_entry *entry)
{
	/* FIXME assuming dma_addr_t >  phys_addr_t? */
	struct rlist_dma_entry dma_entry = {
		.dma_address = rlist_cpu_entry_physical(entry),
		.length = entry->length
	};

	return rlist_dma_num_segments(segment, &dma_entry);
}

/*
 * Use in the DMA API implementations to check the segmentation logic. This
 * checks that the number of segments for the CPU entry is >= the number of
 * segments for the corresponding DMA entry.
 */
static inline bool
rlist_dma_segmentation_ok(const struct rlist_dma_segmentation *segment,
			  const struct rlist_cpu_entry *entry, dma_addr_t dma)
{
	phys_addr_t phys = rlist_cpu_entry_physical(entry);
	struct rlist_dma_entry dma_entry = {
		.dma_address = dma,
		.length = entry->length
	};

	if ((phys & segment->min_align_mask) != (dma & segment->min_align_mask))
		return false;

	/* Max segment size can not fail since both lengths are the same */

	if ((dma & segment->segment_boundary_mask) ==
	    ((dma + entry->length) & segment->segment_boundary_mask))
		return true;
	return rlist_cpu_num_segments(segment, entry) >=
	       rlist_dma_num_segments(segment, &dma_entry);
}

struct rlist_dma_state
{
	struct rlist_dma *rlist_dma;
	union {
		struct rlist_state rls;
		struct rlist_cpu_state rlscpu;
	};
	u8 valid; // FIXME layout into the union
};

static inline void rlsdma_init(struct rlist_dma_state *rls,
			       struct rlist_dma *rdma)
{
	rls->rlist_dma = rdma;
	if (rdma->cpu)
		rlscpu_init(&rls->rlscpu, rdma->cpu);
	else
		rls_init(&rls->rls, &rdma->rlist);
}

static inline struct rlist_dma_state __rlist_state_init(struct rlist_dma *rdma)
{
	struct rlist_dma_state res;

	rlsdma_init(&res, rdma);
	return res;
}

/**
 * RLIST_DMA_STATE() - Declare a rlist operation state.
 * @name: Name of this operation state (usually rls).
 * @rlist: rlist to operate on.
 *
 * Declare and initialise an rlist_dma_state on the stack.
 */
#define RLIST_DMA_STATE(name, rlist_dma) \
	struct rlist_dma_state name = __rlist_state_init(rlist_dma)

bool rlsdma_reset(struct rlist_dma_state *rls, struct rlist_dma_entry *entry);
bool rlsdma_next(struct rlist_dma_state *rls, struct rlist_dma_entry *entry);

#define rlist_dma_for_each_entry(rls, entry)                       \
	for ((rls)->valid = rlsdma_reset(rls, entry); (rls)->valid; \
	     (rls)->valid = rlsdma_next(rls, entry))

/* Return the first entry in the list or return false */
static inline bool rlist_dma_first(struct rlist_dma *rdma,
				   struct rlist_dma_entry *entry)
{
	RLIST_DMA_STATE(rls, rdma);

	return rlsdma_reset(&rls, entry);
}

/*
 * Like we can split rlist_cpu up by folio/page split up the rlist_dma
 * based on segmentation limits.
 */
// #define rlist_dma_for_each_segment(rdma, rls, entry, segmentation)

/*
 * page list segmentation iterators
 *
 * Page list HW takes in a list of fixed size, and fixed alignment "blocks" to
 * cover the memory. The blocks must be contiguous and interior gaps are not
 * allowed. The HW may represent this as a list of aligned fixed size DMA
 * addresses or something like a multi-level page table.
 *
 * The block size used in the HW and the page size used in the CPU are not
 * related, we inspect the rlist_dma's content and determine the HW block size
 * that can represent the content. The CPU may have a larger page size than the
 * HW's block size.
 *
 * HW may support page lists with a starting offset and a may have an unaligned
 * length. This potentially creates a gap at the start and end where the HW
 * promises not to DMA to. These algorithms take advantage of that gap to
 * increase the HW block size.
 *
 * HW may treat the page list as a page table, in which case HW must have the
 * invariant of:
 *   hwva % block_size == dma_addr % block_size
 * For evey block.
 */

unsigned long
rlist_dma_find_best_blocksz(struct rlist_dma *rdma,
			    const struct rlist_dma_segmentation *segment);

bool rlsdma_block_iter_reset(struct rlist_dma_state *rls,
			     struct rlist_dma_entry *entry,
			     dma_addr_t blocksz);
bool rlsdma_block_iter_next(struct rlist_dma_state *rls,
			    struct rlist_dma_entry *entry, dma_addr_t blocksz);

dma_addr_t rlist_dma_block_offset(struct rlist_dma *rdma,
				  dma_addr_t blocksz);

dma_addr_t rlist_dma_num_blocks(struct rlist_dma *rdma, dma_addr_t blocksz);

/*
 * Iterate over a block size determined by rlist_dma_find_best_blocksz()
 * Only entry->dma_address is valid. Performs exactly rlist_dma_num_blocks()
 * iterations.
 */
#define rlist_dma_for_each_block(rls, entry, blocksz)                     \
	for ((rls)->valid = rlsdma_block_iter_reset(rls, entry, blocksz); \
	     (rls)->valid;                                                \
	     (rls)->valid = rlsdma_block_iter_next(rls, entry, blocksz))

/*
 * Tools to populate the rlist_dma
 */
struct rlist_dma_state_append
{
	union {
		struct rlist_dma *rlist_dma;
		struct rlist_state_append rlsa;
	};
};
#define RLIST_DMA_STATE_APPEND(name, _rlist) \
	struct rlist_dma_state_append name = { .rlist_dma = _rlist }

static inline int rlsdma_append_begin(struct rlist_dma_state_append *rlsa)
{
	return rls_append_begin(&rlsa->rlsa);
}

static inline void rlsdma_append_end(struct rlist_dma_state_append *rlsa)
{
	rls_append_end(&rlsa->rlsa);
}

static inline void
rlsdma_append_destroy_rlist(struct rlist_dma_state_append *rlsa)
{
	rls_append_destroy_rlist(&rlsa->rlsa);
}

/* Address is relative to rsiova_set_iova() */
static inline int rlsdma_append(struct rlist_dma_state_append *rlsa,
				dma_addr_t dma_address, u64 length,
				u32 dma_map_ops_priv, gfp_t gfp)
{
	struct rlist_entry entry = {
		.type = RLIST_DMA_RELATIVE,
		.base = dma_address - rlsa->rlist_dma->base,
		.length = length,
		.extra = dma_map_ops_priv,
	};

	/* FIXME This has to combine? */
	return rls_append(&rlsa->rlsa, &entry, gfp);
}

/* For this version rsiova_set_iova(0 does not change the address */
static inline int rlsdma_append_no_base(struct rlist_dma_state_append *rlsa,
					dma_addr_t dma_address, u64 length,
					u32 dma_map_ops_priv, gfp_t gfp)
{
	struct rlist_entry entry = {
		.type = RLIST_DMA_ABSOLUTE,
		.base = dma_address,
		.length = length,
		.extra = dma_map_ops_priv,
	};

	return rls_append(&rlsa->rlsa, &entry, gfp);
}

/*
 * IOVA assignment helpers for rlist
 *
 * Many dma_map_ops implementaions are driving an IOMMU, this provides a common
 * flow for them all to use.
 *
 * We compute the required IOVA size and alignment, the driver allocates it,
 * then the driver uses an iterator to get each each physically contiguous chunk
 * to maps it to the iommu, finally we build the rlist_dma.
 */
struct rlist_dma_state_iova
{
	const struct rlist_dma_segmentation *segment;
	struct rlist_cpu_state rls;
	struct p2pdma_provider_map_cache p2pdma_cache;
	struct device *dev;
	dma_addr_t cur_iova;
	dma_addr_t pgsize;
};

int rsiova_init(struct rlist_dma_state_iova *rsiova, struct rlist_cpu *rcpu,
		struct rlist_dma *rdma,
		const struct rlist_dma_segmentation *segment,
		struct device *dev, dma_addr_t min_iova_pgsize, gfp_t gfp);

static inline dma_addr_t rsiova_length(struct rlist_dma_state_iova *rsiova)
{
	/* rsiova_init() leaves the length here */
	return rsiova->cur_iova;
}

/*
 * For these routines to work the iommu must allocate rsiova_length() bytes and
 * align it to at least rsiova_alignment(). This is because we make assumptions
 * about where segment_boundary_mask effects things based on a 0 IOVA.
 *
 * The IOVA should be allocated such that:
 *   starting_iova % rsiova_alignment(rsiova) == 0
 */
static inline dma_addr_t rsiova_alignment(struct rlist_dma_state_iova *rsiova)
{
	if (rsiova->segment->segment_boundary_mask == ULONG_MAX)
		return rsiova->pgsize;
	return min_t(dma_addr_t,
		     max_t(dma_addr_t, rsiova->pgsize,
			   roundup_pow_of_two(rsiova_length(rsiova))),
		     rsiova->segment->segment_boundary_mask + 1);
}

/* Set the IOVA that was allocated */
static inline void rsiova_set_iova(struct rlist_dma_state_iova *rsiova,
				   struct rlist_dma *rdma, dma_addr_t iova)
{
	rdma->base = iova;
	WARN_ON(iova % rsiova_alignment(rsiova) == 0);
}

struct rlist_dma_iova_map
{
	dma_addr_t iova;
	phys_addr_t phys;
	size_t length;
};

void rsiova_first_map(struct rlist_dma_state_iova *rsiova,
		      struct rlist_dma_iova_map *map, dma_addr_t first_iova);
void rsiova_next_map(struct rlist_dma_state_iova *rsiova,
		     struct rlist_dma_iova_map *map);

/*
 * Iterate over all the IOVA mappings that need to be programmed. Each map is
 * the iova to map phys to. length, phys and iova are all multiples of
 * min_iova_pgsize.
 */
#define rsiova_for_each_map(rsiova, first_iova, map)                   \
	for (rsiova_first_map(rsiova, map, first_iova); (map)->length; \
	     rsiova_next_map(rsiova, map))

void rlsdma_first_unmap(struct rlist_dma_state *rls,
			struct rlist_dma_iova_map *map, dma_addr_t pgsize);
void rlsdma_next_unmap(struct rlist_dma_state *rls,
		       struct rlist_dma_iova_map *map, dma_addr_t pgsize);

/*
 * Iterate over all the IOVA mappings that need to be unmapped. This will not be
 * the exact set of IOVAs that rsiova_for_each_map() produces, but it will span
 * the same ranges without crossing gaps.
 */
#define rlsdma_for_each_unmap(rls, map, iova_pgsize)                   \
	for (rlsdma_first_unmap(rls, map, iova_pgsize); (map)->length; \
	     rlsdma_next_unmap(rls, map, iova_pgsize))

#endif
