// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 */
#ifndef __LINUX_RLIST_H
#define __LINUX_RLIST_H
#include <linux/types.h>
#include <linux/string.h>
#include <linux/limits.h>

/**
 * DOC: Range List Summary
 *
 * The Range List is a linear list of (usually memory) ranges. It is similar in
 * spirit to the scatterlist or bio_vec, but with a different design.
 *
 * This is a helper datastructure, most likely little will use it directly. The
 * rlist_cpu and rlist_dma use this in their implementations to create dedicated
 * objects to store memory ranges of CPU folio/physical or memory ranges of
 * dma_addr_t's.
 *
 * The data structure is general purpose, it holds a logical list of of struct
 * rlist_entry. rlist_entry provides 96 bits to describe the "base/offset" a 60
 * bit length and 34 bits of extra data.
 *
 * rlist is seekable in O(N) time. Since each entry contains a variable length,
 * the entire list must be iterated to find a "position" from the start.
 * Next entry iteration is O(1).
 *
 * Internally rlist is a linked list of memory chunks each holding an array of
 * entries. The rlist entry size is variable, each  entry size may be 8, 16 or
 * 24 bytes. The smallest size is selected based on the providated data to
 * store. This automatic operation means there is no special version of rlist
 * for 32 bit systems, they will naturally use the smaller entry sizes. Things
 * are organized so that 64 bit systems also have a higher chance of using the
 * smaller entry sizes to help with cache and memory utilization. This choice
 * costs CPU cycles at benifit of reducing memory usage and not duplicating the
 * code for 32/64 configurations.
 *
 * The API design is intended to follow the pattern of xarray and maple tree,
 * with familiar things like a state object, lower level 'rls' API to build
 * iterators and higher level APIs built from the rls. Aside from being familiar
 * it lends itself to an understandable modularity.
 *
 * Compared to scatter list:
 * - Entry size is 8/16/24 while scatterlist is fixed at 32 bytes
 * - A header is placed on each chunk instead of using the last entry to chain
 *   This means freeing doesn't have to walk the entries
 * - rlist cannot concurrently store a CPU list and a DMA list in the same
 *   memory. This has caused alot of type safety problems.
 * - rlist has 'extra' scatterlist has 'dma_flags'
 * - rlist is not restricted to struct page memory
 * - rlist has one top-of-type 'struct rlist', vs 'struct scatterlist',
 *   'struct sg_table', and 'struct sg_table_append'
 *
 * Compared to bio:
 * - Entry size is 8/16/24 while bio_vec is fixed at 16 bytes
 * - bi_io_vec is a linear list of chunks vs a linked list
 * - rlist has "extra" bio has no equivilant
 * - rlist is not restricted to struct page memory
 * - rlist can grow dynamically and does not need to pre-predict the required
 *   entries
 * - Splitting rlist at entry boundaries would be slower than bio_vec
 * - Probably everything about rlist is more CPU due to the abstraction overhead
 */

struct rlist_chunk;
struct rlist_append;
struct rlist_hdr;

/**
 * struct rlist_entry - Each entry stored in the list
 * @type: Identify what type of range it is
 * @length: The length of the range
 * @base: The start of the range
 * @offset: A bias from the start
 * @extra: Additional data
 *
 * The container is largely opaque to how the data above is used, however it
 * does have some built-in assumptions:
 *  - "append with merge" operation will look at the base, length offset and
 *     type to perform adjacent range merging
 *  - "position" is the sum of length fields
 */
struct rlist_entry {
	u64 type : 2;
	u64 length : 60;
	u64 base;
	u32 offset;
	u32 extra;
};

/**
 * struct rlist - Top level structure for the range list
 *
 * There are no user accessible members of this struct.
 */
struct rlist {
	/* private */
	struct rlist_chunk *head;
};

static inline void rlist_init(struct rlist *rlist)
{
	memset(rlist, 0, sizeof(*rlist));
}
void rlist_destroy(struct rlist *rlist);

bool __rlist_empty(struct rlist *rlist);
static inline bool rlist_empty(struct rlist *rlist)
{
	if (!rlist->head)
		return true;
	return __rlist_empty(rlist);
}

/**
 * struct rlist_state - State for iteration of rlist elements
 */
struct rlist_state
{
	/* private */
	struct rlist *rlist;
	struct rlist_chunk *chunk;
	/* Sum of lengths in entries prior to this */
	u64 position;
	unsigned char valid;
	unsigned char cur_entry_size;
	unsigned short cur_entry_idx;
};

/**
 * RLIST_STATE() - Declare a rlist operation state.
 * @name: Name of this operation state (usually rls).
 * @rlist: rlist to operate on.
 *
 * Declare and initialise an rlist_state on the stack.
 * A state can be used for iterating over the rlist, or it can be used
 * for appending new entries.
 *
 * If the state was used for appending then rls_unload() or rls_destroy_rlist()
 * must be called.
 */
#define RLIST_STATE(name, _rlist) struct rlist_state name = { .rlist = _rlist }

static inline void rls_init(struct rlist_state *rls, struct rlist *rlist)
{
	rls->rlist = rlist;
}
int rlist_init_single(struct rlist *rlist, struct rlist_entry *entry,
		      gfp_t gfp);

bool rls_reset(struct rlist_state *rls, struct rlist_entry *entry);
bool rls_next(struct rlist_state *rls, struct rlist_entry *entry);
bool rls_seek(struct rlist_state *rls, struct rlist_entry *entry, u64 position);

#define rlist_for_each_entry(rls, entry)                         \
	for ((rls)->valid = rls_reset(rls, entry); (rls)->valid; \
	     (rls)->valid = rls_next(rls, entry))

#ifdef cutting_room_floor
/* slice is a linear chunk of the rlist that can be iterated on */
struct rlist_slice {
	struct rlist_chunk *chunk;
	u64 position;
	u64 end_position;
	unsigned short start_entry_idx;
};

static inline void rls_slice_start(struct rlist_state *rls,
				   struct rlist_slice *slice)
{
	slice->chunk = rls->chunk;
	slice->start_entry_idx = rls->cur_entry_idx;
	slice->position = rls->position;
}

static inline void rls_slice_end(struct rlist_state *rls,
				 struct rlist_slice *slice)
{
	slice->end_position = rls->position;
}

bool rls_seek_slice(struct rlist_state *rls, struct rlist_entry *entry,
		    const struct rlist_slice *slice);

static inline bool rls_next_slice(struct rlist_state *rls,
				  struct rlist_entry *entry,
				  const struct rlist_slice *slice)
{
	if (rls->position >= slice->end_position)
		return false;
	return rls_next(rls, entry);
}
#define rlist_for_each_slice_element(rls, entry, slice)                  \
	for ((rls).valid = rls_seek_slice(rls, entry, slice); rls.valid; \
	     (rls).valid = rls_next_slice(rls, entry, slice))
#endif

/**
 * struct rlist_state_append - State for appending new elements
 *
 * Appending requires enough more memory than normal iteration it is given its
 * own state, also switching between iteration and appending is not allowed.
 */
struct rlist_state_append
{
	struct rlist_state rls;
	struct rlist_chunk *preload_head;
	unsigned char no_alloc : 1;
};

#define RLIST_STATE_APPEND(name, _rlist) \
	struct rlist_state_append name = { .rls = { .rlist = _rlist } }

int rls_append_begin(struct rlist_state_append *rlsa);
void rls_append_end(struct rlist_state_append *rlsa);
void rls_append_destroy_rlist(struct rlist_state_append *rlsa);
int rls_preload(struct rlist_state_append *rlsa, size_t estimated_num_entries,
		gfp_t gfp);
int rls_append(struct rlist_state_append *rlsa, const struct rlist_entry *entry,
	       gfp_t gfp);

#endif
