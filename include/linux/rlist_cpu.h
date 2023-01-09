// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 */
#ifndef __LINUX_RLIST_CPU_H
#define __LINUX_RLIST_CPU_H
#include <linux/rlist.h>
#include <linux/mm.h>
#include <linux/bio.h>

struct p2pdma_provider;

/**
 * DOC: CPU Range Iterator
 *
 * Provide a general API to iterate over CPU memory ranges without exposing
 * details of the storage layer to the caller.
 *
 * This is intended to map 1:1 to the 'struct rlist' as a storage layer, however
 * it will operate on other storage layers too.
 *
 * The iterator returns entries using one of these formats:
 *
 * Struct page backed memory:
 *      u8 type:2;
 *	u64 length:60;
 *	struct folio *folio;
 *	u32 folio_offset;
 *
 * Pageless CPU memory:
 *      u8 type:2;
 *	u64 length:60;
 *      phys_addr_t base;
 *      u32 provider;
 *
 * Type is used as an enum rlist_cpu_types and provider is a struct
 * p2pdma_provider identifier.
 *
 * When 'struct rlist' is used as the storage layer then this is intended to be
 * the easiest to use "kitchen sink" implementation that can do everything,
 * including dynamic sizing, mixing physical memory, struct page backed memory
 * and peer to peer requirements.
 */

enum rlist_cpu_types {
	RLIST_CPU_MEM_FOLIO = 0,
	RLIST_CPU_MEM_PHYSICAL = 1,
};

struct rlist_cpu_entry {
	union {
		struct rlist_entry _entry;
		struct {
			u64 type : 2;
			u64 length : 60;
			union {
				struct folio *folio;
				phys_addr_t phys;
				u64 _base;
			};
			union {
				u32 folio_offset;
				/* Only used by rlist_cpu_for_each_page() */
				u32 page_offset;
			};
			u32 provider_index;
		};
	};
};

static inline phys_addr_t
rlist_cpu_entry_physical(const struct rlist_cpu_entry *entry)
{
	switch (entry->type) {
	case RLIST_CPU_MEM_FOLIO:
		return ((phys_addr_t)folio_pfn(entry->folio)) * PAGE_SIZE +
		       entry->folio_offset;
	case RLIST_CPU_MEM_PHYSICAL:
		return entry->phys;
	default:
		WARN(true, "Corrupt rlist cpu");
		return 0;
	}
}

/* Backing storage choice */
enum rlist_cpu_type {
	RLIST_CPU,
	RLIST_CPU_PAGES,
	RLIST_CPU_BIO,
};

/* Backing storage is a linear array of struct pages */
struct _rlist_cpu_pages {
	struct page **pages;
	size_t size;
	size_t available;
};

/* Backing storage is a bio */
struct _rlist_cpu_bio {
	struct bio *bio;
};

/* Bits summarizing the content of the list */
enum {
	RLIST_SUM_HAS_P2PDMA_PAGE = 1 << 0,
	RLIST_SUM_NOT_PAGELIST = 1 << 1,
};

/**
 * struct rlist_cpu - Range List of CPU memory
 */
struct rlist_cpu {
	/* private */
	union {
		struct _rlist_cpu_pages pages;
		struct _rlist_cpu_bio bio;
		struct rlist rlist;
	};
	enum rlist_cpu_type type;

	u64 summary_flags : 3;
	u64 max_position : 61;
};

void rlist_cpu_init(struct rlist_cpu *rcpu);
void rlist_cpu_init_pages(struct rlist_cpu *rcpu, struct page **pages,
			  size_t npages_used, size_t npages_available);
void rlist_cpu_init_bio(struct rlist_cpu *rcpu, struct bio *bio,
			unsigned int length);
int rlist_cpu_init_single_page(struct rlist_cpu *rcpu, struct page *page,
			       unsigned int offset, size_t length, gfp_t gfp);
void rlist_cpu_destroy(struct rlist_cpu *rcpu, bool dirty);

/* Sum of length from every entry in the list */
static inline u64 rlist_cpu_length(const struct rlist_cpu *rcpu)
{
	return rcpu->max_position;
}

/* True if a page in the rlist might be is_pci_p2pdma_page() */
static inline bool rlist_cpu_has_p2pdma(struct rlist_cpu *rcpu)
{
	return rcpu->summary_flags & RLIST_SUM_HAS_P2PDMA_PAGE;
}

/*
 * All pages but the last end on a PAGE_SIZE boundary, all pages but the first
 * start on a PAGE_SIZE boundary.
 */
static inline bool rlist_cpu_is_pagelist(struct rlist_cpu *rcpu)
{
	return !(rcpu->summary_flags & RLIST_SUM_NOT_PAGELIST);
}

struct _rlist_cpu_pages_state {
	struct rlist_cpu *rlist_cpu;
	void *_pad;
	u64 position;
	unsigned char valid;
	struct page **cur_page;
};

struct _rlist_cpu_bio_state {
	struct rlist_cpu *rlist_cpu;
	struct bio *cur_bio;
	u64 position;
	unsigned char valid;
	/* FIXME: bvec_iter is a bit big, and we don't use most of it. */
	struct bvec_iter iter;
};

struct rlist_cpu_state
{
	union {
		struct rlist_state rls;
		struct _rlist_cpu_pages_state rls_pages;
		struct _rlist_cpu_bio_state rls_bio;
		struct {
			struct rlist_cpu *rlist_cpu;
			void *_pad;
			u64 position;
			unsigned char valid;
		};
	};
	u64 remaining_length;
};

/**
 * RLIST_CPU_STATE() - Declare a rlist operation state.
 * @name: Name of this operation state (usually rls).
 * @rlist: rlist to operate on.
 *
 * Declare and initialise an rlist_cpu_state on the stack.
 */
#define RLIST_CPU_STATE(name, rlist_cpu) \
	struct rlist_cpu_state name = { .rls = { .rlist = &(rlist_cpu)->rlist } }

static inline void rlscpu_init(struct rlist_cpu_state *rls,
			       struct rlist_cpu *rlist_cpu)
{
	rls->rlist_cpu = rlist_cpu;
}

bool rlist_cpu_empty(struct rlist_cpu *rcpu);

/* This is the position of the last entry the iterator returned */
static inline u64 rlscpu_position(struct rlist_cpu_state *rlscpu)
{
	return rlscpu->position;
}

bool rlscpu_reset(struct rlist_cpu_state *rlscpu, struct rlist_cpu_entry *entry);
bool rlscpu_next(struct rlist_cpu_state *rlscpu, struct rlist_cpu_entry *entry);

#define rlist_cpu_for_each_entry(rlscpu, entry)                              \
	for ((rlscpu)->valid = rlscpu_reset(rlscpu, entry); (rlscpu)->valid; \
	     (rlscpu)->valid = rlscpu_next(rlscpu, entry))

bool rlscpu_read_folio(struct rlist_cpu_state *rlscpu,
		       struct rlist_cpu_entry *entry);
bool rlscpu_next_folio(struct rlist_cpu_state *rlscpu,
		       struct rlist_cpu_entry *entry);

/*
 * Break up multi-folio ranges and return single folios. entry->folio,
 * entry->folio_offset and entry->length are valid
 */
#define rlist_cpu_for_each_folio(rlscpu, entry)                  \
	for ((rlscpu)->valid = rlscpu_reset(rlscpu, entry) &&    \
			       rlscpu_read_folio(rlscpu, entry); \
	     (rlscpu)->valid;                                    \
	     (rlscpu)->valid = rlscpu_next_folio(rlscpu, entry))

bool rlscpu_read_page(struct rlist_cpu_state *rlscpu, struct page **page,
		      struct rlist_cpu_entry *entry);
bool rlscpu_next_page(struct rlist_cpu_state *rlscpu, struct page **page,
		      struct rlist_cpu_entry *entry);

/*
 * Break up multi-page ranges and return single pages. entry->page_offset and
 * entry->length are valid
 */
#define rlist_cpu_for_each_page(rlscpu, page, entry)                  \
	for ((rlscpu)->valid = rlscpu_reset(rlscpu, entry) &&         \
			       rlscpu_read_page(rlscpu, page, entry); \
	     (rlscpu)->valid;                                         \
	     (rlscpu)->valid = rlscpu_next_page(rlscpu, page, entry))

/* Return the first entry in the list or return false */
static inline bool rlist_cpu_first(struct rlist_cpu *rcpu,
				   struct rlist_cpu_entry *entry)
{
	RLIST_CPU_STATE(rls, rcpu);

	return rlscpu_reset(&rls, entry);
}

int rlist_cpu_copy_from(void *dst, struct rlist_cpu *rcpu, size_t offset,
			size_t length);
int rlist_cpu_copy_to(struct rlist_cpu *rcpu, const void *src, size_t offset,
		      size_t length);

struct rlist_cpu_state_append
{
	union {
		struct rlist_cpu *rlist_cpu;
		struct rlist_state_append rlsa;
		struct _rlist_cpu_pages_state rls_pages;
	};
	struct rlist_cpu_entry cur;
	unsigned int last_summary_flags;
};

int rlscpu_append_begin(struct rlist_cpu_state_append *rlsa);
int rlscpu_append_end(struct rlist_cpu_state_append *rlsa, gfp_t gfp);
void rlscpu_append_destroy_rlist(struct rlist_cpu_state_append *rlsa);

int rlscpu_append_folio(struct rlist_cpu_state_append *rlsa,
			struct folio *folio, unsigned int offset, size_t length,
			gfp_t gfp);
#endif
