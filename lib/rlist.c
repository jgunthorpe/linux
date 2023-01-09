// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 */
#include <linux/rlist.h>
#include <linux/slab.h>

static struct kmem_cache *rlist_chunk_cache;

enum {
	RLIST_ENCODING_8 = 0,
	RLIST_ENCODING_16 = 1,
	RLIST_ENCODING_24 = 2,

	/*
	 * To avoid storing the length of the used chunk the last entry has this
	 * type to mark the end of the used chunk.
	 */
	RLIST_ENCODING_END_PAD = 3,
};

enum {
	RLIST_HDR_LENGTH_BITS = 28,
	RLIST_HDR_MAX_LENGTH = (1 << RLIST_HDR_LENGTH_BITS) - 1,
};
struct rlist_hdr {
	u32 encoding : 2;
	u32 type : 2;
	u32 length : RLIST_HDR_LENGTH_BITS;
};
static_assert(sizeof(struct rlist_hdr) == sizeof(u32));

#ifdef cutting_room_floor
/* FIXME: for page lists on 32 bit systems ? Would have to replace rlist_8 at compile time */
/* 20 bit base, 8 bit length (4 bytes) */
struct rlist_4 {
	u32 encoding : 2;
	u32 type : 2;
	u32 base : 20;
	u32 length : 8;
};
#endif

/*
 * 32 bit base, 28 bit length, no offset (8 bytes)
 *
 * Intended to describe a folio (by pfn). This is enough for up to 17TB of
 * memory, but we could adjust more bits from the length.
 *
 * Also good for almost all IOMMU addresses.
 */
enum {
	RLIST_8_MAX_LENGTH = (1ULL << RLIST_HDR_LENGTH_BITS) - 1,
	RLIST_8_MAX_BASE = U32_MAX,
};
struct rlist_8 {
	struct rlist_hdr hdr;
	u32 base;
} __packed __aligned(__alignof__(u64));
static_assert(sizeof(struct rlist_8) == sizeof(u64));

/*
 * 64 bit base, 24 bit offset, 36 bit length (16 bytes)
 *
 * Intended to describe any slice of up to a 16M folio, or any physical range.
 */
enum {
	RLIST_16_MAX_LENGTH = (1ULL << (RLIST_HDR_LENGTH_BITS + 8)) - 1,
	RLIST_16_MAX_BASE = U64_MAX,
	RLIST_16_MAX_OFFSET = (1 << 24) - 1,
};
struct rlist_16 {
	struct rlist_hdr hdr;
	u32 length : 8;
	u32 offset : 24;
	u64 base;
} __packed __aligned(__alignof__(u64));
static_assert(sizeof(struct rlist_16) == sizeof(u64) * 2);

/* 64 bit base, 32 bit offset, 60 bit length (24 bytes) */
struct rlist_24 {
	struct rlist_hdr hdr;
	u32 offset;
	u64 base;
	u32 length;
	u32 extra;
} __packed __aligned(__alignof__(u64));
static_assert(sizeof(struct rlist_24) == sizeof(u64) * 3);

/*
 * This follows a similar logic to xarray. We have an overhead of 8 bytes per
 * chunk and we try to make this struct a power of two so it packs into a
 * PAGE_SIZE and thus kmem_cache without additional overhead. Like xarray we
 * accept the overhead costs of partially populated chunks.
 *
 * At 512 bytes this gives a memory efficiency for each entry size of:
 *   full chunk usage: 63 entries = 8.1 bytes/entry 98.4% (8)
 *                     31 entries = 16.5 bytes/entry 96.9% (16)
 *                     21 entries = 24.38 bytes/entry 98.4% (24)
 *   half chunk usage: 32 entries = 16 bytes/entry 50% (8)
 *                     16 entries = 32 bytes/entry 50% (16)
 *                     11 entries = 46.55 bytes/entry 51.6% (24)
 *
 * Compared to other data structures:
 *   struct page array: 8 bytes/entry
 *   struct page array 2M contiguous pages: 4096 bytes/entry
 *   struct scatterlist (4k chunks) : 32.25 bytes/entry 99.2%
 *
 * FIXME: If we do do an optional 4k chunk size then efficiency improves to
 * 99.8%/99.6%
 */
enum {
	RLIST_CHUNK_SIZE = CONFIG_BASE_SMALL ? 128 - sizeof(u64) :
					       512 - sizeof(u64),
};

/* FIXME: we could support three chunk types, encode the type
 * in the low two bits of the pointer
 *  - This 512 byte chunk
 *  - A PAGE_SIZE chunk, if we know the list is very big
 *  - A small kmalloc chunk if we know the list is small
 */

struct rlist_chunk {
	struct rlist_chunk *next;
	struct rlist_hdr __aligned(sizeof(u64))
		entries[RLIST_CHUNK_SIZE / sizeof(struct rlist_hdr)];
};

/* rls iteration  ---------------------------------------------- */

static struct rlist_hdr *rls_cur_entry(struct rlist_state *rls)
{
	return &rls->chunk->entries[rls->cur_entry_idx];
}

static void rls_set_chunk(struct rlist_state *rls, struct rlist_chunk *chunk)
{
	rls->chunk = chunk;
	rls->cur_entry_idx = 0;
	rls->cur_entry_size = 0;
}

static size_t rls_chunk_num_idx(struct rlist_state *rls)
{
	return ARRAY_SIZE(rls->chunk->entries);
}

static bool rls_is_end(struct rlist_state *rls)
{
	if (rls->cur_entry_idx == rls_chunk_num_idx(rls))
		return true;
	if (WARN_ON(rls->cur_entry_idx > rls_chunk_num_idx(rls)))
		return true;
	return rls->chunk->entries[rls->cur_entry_idx].type ==
	       RLIST_ENCODING_END_PAD;
}

static size_t rlist_decode(struct rlist_hdr *hdr, struct rlist_entry *entry)
{
	switch (hdr->encoding) {
		case RLIST_ENCODING_8: {
			struct rlist_8 *elm =
				container_of(hdr, struct rlist_8, hdr);

			entry->type = elm->hdr.type;
			entry->length = elm->hdr.length;
			entry->base = elm->base;
			entry->offset = 0;
			entry->extra = 0;
			return sizeof(*elm) / sizeof(struct rlist_hdr);
		}
		case RLIST_ENCODING_16: {
			struct rlist_16 *elm =
				container_of(hdr, struct rlist_16, hdr);

			entry->type = elm->hdr.type;
			entry->length =
				elm->hdr.length |
				((u64)elm->length << RLIST_HDR_LENGTH_BITS);
			entry->base = elm->base;
			entry->offset = elm->offset;
			entry->extra = 0;
			return sizeof(*elm) / sizeof(struct rlist_hdr);
		}
		case RLIST_ENCODING_24: {
			struct rlist_24 *elm =
				container_of(hdr, struct rlist_24, hdr);

			entry->type = elm->hdr.type;
			entry->length =
				elm->hdr.length |
				((u64)elm->length << RLIST_HDR_LENGTH_BITS);
			entry->base = elm->base;
			entry->offset = elm->offset;
			entry->extra = elm->extra;
			return sizeof(*elm) / sizeof(struct rlist_hdr);
		}
		default:
		WARN_ON(1);
		memset(entry, 0, sizeof(*entry));
		return 0;
	}
}

bool rls_next(struct rlist_state *rls, struct rlist_entry *entry)
{
	rls->position += entry->length;
	rls->cur_entry_idx += rls->cur_entry_size;
	if (rls_is_end(rls)) {
		rls_set_chunk(rls, rls->chunk->next);
		if (!rls->chunk)
			return false;
	}

	rls->cur_entry_size = rlist_decode(rls_cur_entry(rls), entry);
	return rls->cur_entry_size;
}

bool rls_reset(struct rlist_state *rls, struct rlist_entry *entry)
{
	rls->position = 0;
	rls_set_chunk(rls, rls->rlist->head);
	if (!rls->chunk)
		return false;
	rls->cur_entry_size = rlist_decode(rls_cur_entry(rls), entry);
	return rls->cur_entry_size;
}

bool rls_seek(struct rlist_state *rls, struct rlist_entry *entry, u64 position)
{
	if (!rls_reset(rls, entry))
		return false;

	while (!(rls->position >= position &&
	       rls->position + entry->length <= position))
		if (!rls_next(rls, entry))
			return false;
	return true;
}

#ifdef cutting_room_floor
bool rls_seek_slice(struct rlist_state *rls, struct rlist_entry *entry,
		    const struct rlist_slice *slice)
{
	rls_set_chunk(rls, slice->chunk);
	rls->position = slice->position;
	rls->cur_entry_idx = slice->start_entry_idx;
	rls->cur_entry_size = rlist_decode(rls_cur_entry(rls), entry);
	return rls->cur_entry_size && slice->position < slice->end_position;
}
#endif

/* rlist api  ---------------------------------------------- */

static struct rlist_chunk *do_free_chunk_list(struct rlist_chunk *head,
					      void **free_list, size_t len)
{
	size_t cur = 0;

	while (head && cur != len) {
		free_list[cur++] = head;
		head = head->next;
	}
	kmem_cache_free_bulk(rlist_chunk_cache, cur, free_list);
	return head;
}

static void free_chunk_list(struct rlist_chunk *head)
{
	void **free_list;

	if (!head)
		return;

	/* Use the first chunk as temporary memory to build a free list */
	free_list = (void **)head;
	head = head->next;
	while (head)
		head = do_free_chunk_list(head, free_list,
					  sizeof(struct rlist_chunk) /
						  sizeof(*free_list));
	kmem_cache_free(rlist_chunk_cache, free_list);
}

void rlist_destroy(struct rlist *rlist)
{
	free_chunk_list(rlist->head);
	rlist->head = NULL;
}

bool __rlist_empty(struct rlist *rlist)
{
	RLIST_STATE(rls, rlist);
	struct rlist_entry entry;

	/* FIXME better to disallow 0 length chunks and store NULL instead */
	return rls_reset(&rls, &entry);
}

/* rls append  ---------------------------------------------- */

static struct rlist_chunk *rls_alloc_chunk(struct rlist_state_append *rls,
					   gfp_t gfp)
{
	struct rlist_chunk *chunk;

	if (rls->preload_head) {
		chunk = rls->preload_head;
		rls->preload_head = chunk->next;
		return chunk;
	}
	if (rls->no_alloc)
		return ERR_PTR(-ENOSPC);

	chunk = kmem_cache_alloc(rlist_chunk_cache, gfp);
	if (!chunk)
		return ERR_PTR(-ENOMEM);
	return chunk;
}

static struct rlist_hdr *rls_alloc_entry(struct rlist_state_append *rlsa,
					 size_t entry_size, gfp_t gfp)
{
	struct rlist_chunk *chunk;
	struct rlist_state *rls = &rlsa->rls;

	if (rls->chunk) {
		size_t num = rls_chunk_num_idx(rls);

		if (rls->cur_entry_idx + entry_size <= num) {
			struct rlist_hdr *entry =
				&rls->chunk->entries[rls->cur_entry_idx];

			rls->cur_entry_idx += entry_size;
			rls->cur_entry_size = entry_size;
			return entry;
		}

		if (rls->cur_entry_idx != num)
			rls->chunk->entries[rls->cur_entry_idx].type =
				RLIST_ENCODING_END_PAD;
	}

	chunk = rls_alloc_chunk(rlsa, gfp);
	if (IS_ERR(chunk))
		return ERR_CAST(chunk);

	if (rls->chunk) {
		rls->chunk->next = chunk;
		rls->chunk = chunk;
	} else {
		if (WARN_ON(rls->rlist->head))
			return NULL;
		rls->chunk = rls->rlist->head = chunk;
	}
	chunk->next = NULL;
	rls->cur_entry_idx = entry_size;
	rls->cur_entry_size = entry_size;
	return rls->chunk->entries;
}

static void rls_mark_end(struct rlist_state *rls)
{
	if (!rls->chunk)
		return;
	if (rls->cur_entry_idx != rls_chunk_num_idx(rls))
		rls->chunk->entries[rls->cur_entry_idx].type =
			RLIST_ENCODING_END_PAD;
}

int rls_append(struct rlist_state_append *rls, const struct rlist_entry *entry,
	       gfp_t gfp)
{
#define alloc_elm(rls, elm, gfp)                                              \
	container_of(rls_alloc_entry(rls,                                     \
				     sizeof(*elm) / sizeof(struct rlist_hdr), \
				     gfp),                                    \
		     typeof(*elm), hdr)

	if (entry->length <= RLIST_8_MAX_LENGTH &&
	    entry->base <= RLIST_8_MAX_BASE && !entry->offset &&
	    !entry->extra) {
		struct rlist_8 *elm;

		elm = alloc_elm(rls, elm, gfp);
		if (IS_ERR(elm))
			return PTR_ERR(elm);
		elm->hdr.encoding = RLIST_ENCODING_8;
		elm->hdr.type = entry->type;
		elm->hdr.length = entry->length;
		elm->base = entry->base;
	} else if (entry->length <= RLIST_16_MAX_LENGTH &&
		   entry->base <= RLIST_16_MAX_BASE &&
		   entry->offset <= RLIST_16_MAX_OFFSET && !entry->extra) {
		struct rlist_16 *elm;

		elm = alloc_elm(rls, elm, gfp);
		if (IS_ERR(elm))
			return PTR_ERR(elm);
		elm->hdr.encoding = RLIST_ENCODING_16;
		elm->hdr.type = entry->type;
		elm->hdr.length = entry->length;
		elm->length = entry->length >> RLIST_HDR_LENGTH_BITS;
		elm->offset = entry->offset;
		elm->base = entry->base;
	} else {
		struct rlist_24 *elm;

		elm = alloc_elm(rls, elm, gfp);
		if (IS_ERR(elm))
			return PTR_ERR(elm);
		elm->hdr.encoding = RLIST_ENCODING_24;
		elm->hdr.type = entry->type;
		elm->hdr.length = entry->length;
		elm->length = entry->length >> RLIST_HDR_LENGTH_BITS;
		elm->offset = entry->offset;
		elm->base = entry->base;
		elm->extra = entry->extra;
	}
	rls->rls.position += entry->length;
	return 0;
#undef alloc_elm
}

int rls_append_begin(struct rlist_state_append *rlsa)
{
	struct rlist_state *rls = &rlsa->rls;

	/*
	 * This should be avoided, but if the list is already populated then
	 * seek to the end
	 */
	if (rls->rlist->head) {
		struct rlist_entry entry;

		rlist_for_each_entry(rls, &entry)
			;
	} else {
		rls->chunk = NULL;
	}

	rls->position = 0;
	rlsa->no_alloc = false;
	return 0;
}

static int do_alloc_chunk_list(struct rlist_state *prls, void **list,
			       size_t num, gfp_t gfp)
{
	int ret;

	ret = kmem_cache_alloc_bulk(rlist_chunk_cache, gfp, num, list);
	if (ret <= 0)
		return -ENOMEM;
	if (WARN_ON(ret != num))
		return -ENOMEM;

	prls->position += num * ARRAY_SIZE(prls->chunk->entries);
	while (num){
		prls->chunk->next = *list;
		prls->chunk = *list;
		list++;
		num--;
	}
	prls->chunk->next = NULL;
	return 0;
}

/*
 * So we may append entries in atomic contexts, eg for GUP, allow memory
 * allocation to be done before entering the atomic context and store the
 * allocations in the state.
 */
int rls_preload(struct rlist_state_append *rls, size_t estimated_num_entries,
		gfp_t gfp)
{
	RLIST_STATE(prls, rls->rls.rlist);
	unsigned long desired_idx = estimated_num_entries *
				    sizeof(struct rlist_24) /
				    sizeof(struct rlist_hdr);
	unsigned long mem_list_entries;
	void **mem_list;

	/*
	 * Figure out how much we already have, normally caller will only call
	 * this if preload_head is NULL
	 */
	rls_set_chunk(&prls, rls->preload_head);
	while (prls.chunk) {
		prls.position += rls_chunk_num_idx(&prls);
		if (!prls.chunk->next)
			break;
		rls_set_chunk(&prls, prls.chunk->next);
	}

	if (!prls.chunk) {
		struct rlist_chunk *chunk;

		chunk = kmem_cache_alloc(rlist_chunk_cache, gfp);
		if (!chunk)
			return -ENOMEM;

		prls.chunk = rls->preload_head = chunk;
		prls.position += rls_chunk_num_idx(&prls);
	}

	/*
	 * Use an already allocated chunk as temporary memory to do bulk
	 * allocations
	 */
	mem_list = (void **)prls.chunk->entries;
	mem_list_entries = rls_chunk_num_idx(&prls) * sizeof(struct rlist_hdr) /
			   sizeof(void *);
	while (desired_idx < prls.position) {
		unsigned long todo = (desired_idx - prls.position) *
				     sizeof(struct rlist_hdr) /
				     sizeof(prls.chunk->entries);
		int ret;

		todo = min(todo, mem_list_entries);
		ret = do_alloc_chunk_list(&prls, mem_list, mem_list_entries,
					  gfp);
		if (ret)
			return ret;
	}
	return 0;
}

void rls_append_end(struct rlist_state_append *rls)
{
	rls_mark_end(&rls->rls);

	free_chunk_list(rls->preload_head);
	rls->preload_head = NULL;
}

/* Used when rlist appending failed, return it to empty */
void rls_append_destroy_rlist(struct rlist_state_append *rls)
{
	rls_append_end(rls);
	rlist_destroy(rls->rls.rlist);
}

int rlist_init_single(struct rlist *rlist, struct rlist_entry *entry, gfp_t gfp)
{
	RLIST_STATE_APPEND(rlsa, rlist);
	int ret;

	/*
	 * FIXME since we know this is a single entry we could allocate
	 * much less memory.
	 */
	ret = rls_append_begin(&rlsa);
	if (ret)
		return ret;
	ret = rls_append(&rlsa, entry, gfp);
	if (ret) {
		rls_append_destroy_rlist(&rlsa);
		return ret;
	}
	rls_append_end(&rlsa);
	return 0;
}

static int __init rlist_initcall(void)
{
	rlist_chunk_cache = KMEM_CACHE(rlist_chunk, SLAB_PANIC);
	return 0;
}
// FIXME: this may not be early enough in the long run?
early_initcall(rlist_initcall);
