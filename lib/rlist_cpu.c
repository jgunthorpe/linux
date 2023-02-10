// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 */
#include <linux/rlist_cpu.h>
#include <linux/highmem.h>
#include <linux/bio.h>
#include <linux/p2pdma_provider.h>

#define same_memory(struct_a, member_a, struct_b, member_b)              \
	(offsetof(struct_a, member_a) == offsetof(struct_b, member_b) && \
	 __same_type(&((struct_a *)NULL)->member_a,                      \
		     &((struct_b *)NULL)->member_b))

static_assert(same_memory(struct rlist_entry, base, struct rlist_cpu_entry,
			  _base) &&
	      same_memory(struct rlist_entry, offset, struct rlist_cpu_entry,
			  folio_offset) &&
	      same_memory(struct rlist_entry, extra, struct rlist_cpu_entry,
			  provider_index));

static_assert(same_memory(struct _rlist_cpu_pages_state, position,
			  struct rlist_state, position));

static_assert(same_memory(struct _rlist_cpu_bio_state, position,
			  struct rlist_state, position));

static_assert(same_memory(struct rlist_cpu_state, rlist_cpu,
			  struct rlist_cpu_state, rls_pages.rlist_cpu) &&
	      same_memory(struct rlist_cpu_state, position,
			  struct rlist_cpu_state, rls_pages.position)&&
	      same_memory(struct rlist_cpu_state, valid,
			  struct rlist_cpu_state, rls_pages.valid));

/* rlist_cpu and rls.rlist are the same pointer, but not the same type */
static_assert(offsetof(struct rlist_cpu_state, rlist_cpu) ==
		      offsetof(struct rlist_cpu_state, rls.rlist) &&
	      offsetof(struct rlist_cpu, rlist) == 0);

static_assert(same_memory(struct rlist_cpu_state, position,
			  struct rlist_cpu_state, rls.position) &&
	      same_memory(struct rlist_cpu_state, valid, struct rlist_cpu_state,
			  rls.valid));

enum {
	/* This internal type means the length is / PAGE_SIZE and base is a PFN */
	RLIST_CPU_MEM_FOLIO_PFN = 3,
};

/*
 * The encoding scheme stores the folio pointer directly with an offset rather
 * than the phys_addr_t. This is an effort to avoid memory references while
 * decoding the rlist_cpu_entry as we don't have to do compound_head()
 * The offset basically comes for free due to keeping a 8 byte alignment
 * in the rlist entries. However it means some cases would be pushed out
 * of a 16 byte entry size.
 * FIXME: If it is worth the extra trouble is unclear at this point.
 */
static void rlscpu_decode_entry(struct rlist_cpu_entry *entry)
{
	switch (entry->_entry.type) {
	case RLIST_CPU_MEM_FOLIO:
		entry->folio = (void *)(uintptr_t)entry->_entry.base;
		return;
	case RLIST_CPU_MEM_PHYSICAL:
		entry->phys = entry->_entry.base;
		return;
	case RLIST_CPU_MEM_FOLIO_PFN:
		/* FIXME: pfn_folio is overkill, we know this pfn does not
		 * point to a tail page because it came from folio_pfn() */
		entry->folio = pfn_folio(entry->_entry.base);
		entry->length = entry->_entry.length * PAGE_SIZE;
		entry->type = RLIST_CPU_MEM_FOLIO;
		return;
	default:
		WARN(true, "Corrupt rlist_cpu");
	}
}

static void rlscpu_encode_entry(struct rlist_cpu_entry *entry)
{
	switch (entry->_entry.type) {
	case RLIST_CPU_MEM_FOLIO:
		if (entry->folio_offset == 0 && (entry->length % PAGE_SIZE) == 0) {
			entry->_entry.type = RLIST_CPU_MEM_FOLIO_PFN;
			entry->_entry.base = folio_pfn(entry->folio);
			entry->_entry.length = entry->length / PAGE_SIZE;
		} else
			entry->_entry.base = (uintptr_t)entry->folio;
		return;
	case RLIST_CPU_MEM_PHYSICAL:
		entry->_entry.base = entry->phys;
		return;
	default:
		WARN(true, "Corrupt rlist_cpu");
	}
}

static struct page **rlist_pages_end(struct rlist_cpu *rcpu)
{
	return rcpu->pages.pages + rcpu->pages.size;
}

static bool rlscpu_decode_pages(struct rlist_cpu_state *rlscpu,
				struct rlist_cpu_entry *entry)
{
	struct page **end = rlist_pages_end(rlscpu->rlist_cpu);
	struct page **first = rlscpu->rls_pages.cur_page;
	struct page **cur;
	size_t npages = 1;

	rlscpu->rls_pages.position = (rlscpu->rls_pages.cur_page -
				      rlscpu->rlist_cpu->pages.pages) *
				     PAGE_SIZE;

	if (!first || first >= end)
		return false;

	/* Combine consecutive pages */
	for (cur = first; cur != end - 1; cur++) {
		if (nth_page(cur[0], 1) == cur[1])
			npages++;
		else
			break;
	}

	entry->type = RLIST_CPU_MEM_FOLIO;
	entry->length = PAGE_SIZE * npages;
	entry->folio = page_folio(*first);
	entry->folio_offset = folio_page_idx(entry->folio, *first) * PAGE_SIZE;
	entry->provider_index = 0;
	rlscpu->rls_pages.cur_page = cur;
	return true;
}

static bool rlscpu_decode_bio(struct _rlist_cpu_bio_state *rlsbio,
			      struct rlist_cpu_entry *entry)
{
	struct bio_vec bvec;

	bvec = mp_bvec_iter_bvec(rlsbio->cur_bio->bi_io_vec, rlsbio->iter);
	entry->type = RLIST_CPU_MEM_FOLIO;
	entry->length = bvec.bv_len;
	rlsbio->position += entry->length;

	/*
	 * rlist_cpu_entry should point to the leading folio enclosing the first
	 * byte. FIXME: some comments suggested this was necessary for bio?
	 */
	if (bvec.bv_offset >= PAGE_SIZE) {
		bvec.bv_page =
			nth_page(bvec.bv_page, bvec.bv_offset / PAGE_SIZE);
		bvec.bv_offset %= PAGE_SIZE;
	}
	entry->folio = page_folio(bvec.bv_page);
	entry->folio_offset =
		folio_page_idx(entry->folio, bvec.bv_page) * PAGE_SIZE +
		bvec.bv_offset;
	entry->provider_index = 0;

	/*
	 * FIXME: we should agressively merge consecutive bio_vecs. rlist_cpu
	 * presents a fully merged very as this is what the DMA layers want.
	 */
	bio_advance_iter_single(rlsbio->cur_bio, &rlsbio->iter, bvec.bv_len);
	return true;
}

bool rlscpu_reset(struct rlist_cpu_state *rlscpu, struct rlist_cpu_entry *entry)
{
	switch (rlscpu->rlist_cpu->type) {
	case RLIST_CPU:
		if (!rls_reset(&rlscpu->rls, &entry->_entry))
			return false;
		rlscpu_decode_entry(entry);
		return true;
	case RLIST_CPU_PAGES:
		rlscpu->rls_pages.cur_page = rlscpu->rlist_cpu->pages.pages;
		rlscpu->rls_pages.position = 0;
		return rlscpu_decode_pages(rlscpu, entry);
	case RLIST_CPU_BIO:
		rlscpu->rls_bio.cur_bio = rlscpu->rlist_cpu->bio.bio;
		rlscpu->rls_bio.position = 0;
		rlscpu->rls_bio.iter = rlscpu->rls_bio.cur_bio->bi_iter;
		return rlscpu_decode_bio(&rlscpu->rls_bio, entry);
	default:
		WARN(true, "Corrupt rlist_cpu");
		return false;
	}
}
EXPORT_SYMBOL_GPL(rlscpu_reset);

static bool rlscpu_seek_bio(struct _rlist_cpu_bio_state *rlsbio,
			    struct rlist_cpu_entry *entry, u64 position)
{
	struct bvec_iter iter;

	/* Find the bio in the list that contains position */
	rlsbio->position = 0;
	rlsbio->cur_bio = rlsbio->rlist_cpu->bio.bio;
	while (rlsbio->position + rlsbio->cur_bio->bi_iter.bi_size < position) {
		rlsbio->position += rlsbio->cur_bio->bi_iter.bi_size;
		rlsbio->cur_bio = rlsbio->cur_bio->bi_next;
		if (!rlsbio->cur_bio)
			return false;
	}

	/* FIXME this could be a lot faster */
	iter = rlsbio->cur_bio->bi_iter;
	do {
		if (!rlscpu_decode_bio(rlsbio, entry))
			return false;
		if (rlsbio->position >= position)
			return true;
	} while (true);
}

/*
 * Note that seeking doesn't always give the exact same entry as normal
 * iteration would give, it may start later as we don't reproduce merging
 * perfectly.
 */
bool rlscpu_seek(struct rlist_cpu_state *rlscpu, struct rlist_cpu_entry *entry,
		 u64 position)
{
	switch (rlscpu->rlist_cpu->type) {
	case RLIST_CPU:
		if (!rls_seek(&rlscpu->rls, &entry->_entry, position))
			return false;
		rlscpu_decode_entry(entry);
		return true;
	case RLIST_CPU_PAGES:
		if (position / PAGE_SIZE >= rlscpu->rlist_cpu->pages.size)
			return false;
		rlscpu->rls_pages.cur_page =
			rlscpu->rlist_cpu->pages.pages + position / PAGE_SIZE;
		return rlscpu_decode_pages(rlscpu, entry);
	case RLIST_CPU_BIO:
		return rlscpu_seek_bio(&rlscpu->rls_bio, entry, position);
	default:
		WARN(true, "Corrupt rlist_cpu");
		return false;
	}
}
EXPORT_SYMBOL_GPL(rlscpu_seek);

static bool rlscpu_next_bio(struct _rlist_cpu_bio_state *rlsbio,
			    struct rlist_cpu_entry *entry)
{
	if (!rlsbio->iter.bi_size) {
		rlsbio->cur_bio = rlsbio->cur_bio->bi_next;
		if (!rlsbio->cur_bio)
			return false;
		rlsbio->iter = rlsbio->cur_bio->bi_iter;
	}
	return rlscpu_decode_bio(rlsbio, entry);
}

bool rlscpu_next(struct rlist_cpu_state *rlscpu, struct rlist_cpu_entry *entry)
{
	switch (rlscpu->rlist_cpu->type) {
	case RLIST_CPU:
		if (!rls_next(&rlscpu->rls, &entry->_entry))
			return false;
		rlscpu_decode_entry(entry);
		return true;
	case RLIST_CPU_PAGES:
		rlscpu->rls_pages.cur_page++;
		return rlscpu_decode_pages(rlscpu, entry);
	case RLIST_CPU_BIO:
		return rlscpu_next_bio(&rlscpu->rls_bio, entry);
	default:
		WARN(true, "Corrupt rlist_cpu");
		return false;
	}
}
EXPORT_SYMBOL_GPL(rlscpu_next);

bool rlscpu_read_folio(struct rlist_cpu_state *rlscpu,
		       struct rlist_cpu_entry *entry)
{
	u64 length;

	if (WARN_ON_ONCE(entry->type != RLIST_CPU_MEM_FOLIO))
		return false;
	/* The stored folio is always the first folio */
	length = entry->length;
	entry->length = min_t(u64, length,
			      folio_size(entry->folio) - entry->folio_offset);
	rlscpu->remaining_length = length - entry->length;
	return true;
}
EXPORT_SYMBOL_GPL(rlscpu_read_folio);

bool rlscpu_next_folio(struct rlist_cpu_state *rlscpu,
		       struct rlist_cpu_entry *entry)
{
	if (rlscpu->remaining_length) {
		rlscpu->position += entry->length;
		entry->folio_offset = 0;
		entry->folio = folio_next(entry->folio);
		entry->length = min_t(u64, rlscpu->remaining_length,
				      folio_size(entry->folio));
		rlscpu->remaining_length -= entry->length;
		return true;
	}

	return rlscpu_next(rlscpu, entry) && rlscpu_read_folio(rlscpu, entry);
}
EXPORT_SYMBOL_GPL(rlscpu_next_folio);

bool rlscpu_read_page(struct rlist_cpu_state *rlscpu, struct page **page,
		      struct rlist_cpu_entry *entry)
{
	u64 length;

	if (WARN_ON_ONCE(entry->type != RLIST_CPU_MEM_FOLIO))
		return false;

	length = entry->length;
	*page = folio_page(entry->folio, entry->folio_offset / PAGE_SIZE);
	entry->page_offset = entry->folio_offset % PAGE_SIZE;
	entry->length = min_t(u64, length,
			      PAGE_SIZE - entry->page_offset);
	rlscpu->remaining_length = length - entry->length;
	return true;
}
EXPORT_SYMBOL_GPL(rlscpu_read_page);

bool rlscpu_next_page(struct rlist_cpu_state *rlscpu, struct page **page,
		      struct rlist_cpu_entry *entry)
{
	if (rlscpu->remaining_length) {
		rlscpu->position += entry->length;
		entry->page_offset = 0;
		*page = nth_page(*page, 1);
		entry->length = min_t(u64, rlscpu->remaining_length, PAGE_SIZE);
		rlscpu->remaining_length -= entry->length;
		return true;
	}

	return rlscpu_next(rlscpu, entry) &&
	       rlscpu_read_page(rlscpu, page, entry);
}
EXPORT_SYMBOL_GPL(rlscpu_next_page);

/* Iterate over the internal rlist */
void rlist_cpu_init(struct rlist_cpu *rcpu)
{
	rcpu->type = RLIST_CPU;
	rcpu->summary_flags = 0;
	rcpu->max_position = 0;
	rlist_init(&rcpu->rlist);
}
EXPORT_SYMBOL_GPL(rlist_cpu_init);

/* Iterate over an external pages array */
void rlist_cpu_init_pages(struct rlist_cpu *rcpu, struct page **pages,
			  size_t npages_used, size_t npages_available)
{
	rcpu->type = RLIST_CPU_PAGES;
	rcpu->pages.pages = pages;
	rcpu->pages.size = npages_used;
	rcpu->pages.available = npages_available;
	rcpu->max_position = npages_used * PAGE_SIZE;
	rcpu->summary_flags = 0;
	/* Assume a P2PDMA page is present */
	if (npages_used && IS_ENABLED(CONFIG_PCI_P2PDMA))
		rcpu->summary_flags |= RLIST_SUM_HAS_P2PDMA_PAGE;
}
EXPORT_SYMBOL_GPL(rlist_cpu_init_pages);

/* Iterate over an externa bio */
void rlist_cpu_init_bio(struct rlist_cpu *rcpu, struct bio *bio,
			unsigned int length)
{
	rcpu->type = RLIST_CPU_BIO;
	rcpu->bio.bio = bio;
	rcpu->max_position = length;

	/*
	 * FIXME would be helpful if the request could keep track of this
	 * summary information when it builds the bios
	 */
	rcpu->summary_flags = RLIST_SUM_NOT_PAGELIST;
	/* Assume a P2PDMA page is present */
	if (length && IS_ENABLED(CONFIG_PCI_P2PDMA))
		rcpu->summary_flags |= RLIST_SUM_HAS_P2PDMA_PAGE;
}
EXPORT_SYMBOL_GPL(rlist_cpu_init_bio);

int rlist_cpu_init_single_page(struct rlist_cpu *rcpu, struct page *page,
			       unsigned int offset, size_t length, gfp_t gfp)
{
	struct rlist_cpu_entry entry = {};

	entry.type = RLIST_CPU_MEM_FOLIO;
	entry.length = length;

	if (offset >= PAGE_SIZE) {
		page = nth_page(page, offset / PAGE_SIZE);
		offset %= PAGE_SIZE;
	}
	entry.folio = page_folio(page),
	entry.folio_offset =
		folio_page_idx(entry.folio, page) * PAGE_SIZE + offset;

	rlscpu_encode_entry(&entry);
	return rlist_init_single(&rcpu->rlist, &entry._entry, gfp);
}
EXPORT_SYMBOL_GPL(rlist_cpu_init_single_page);

static void rlscpu_unpin_entry(struct rlist_cpu_entry *entry, bool make_dirty)
{
	if (!entry->length)
		return;
	if (entry->type == RLIST_CPU_MEM_FOLIO)
		unpin_user_page_range_dirty_lock(
			folio_page(entry->folio,
				   entry->folio_offset / PAGE_SIZE),
			(entry->length + entry->folio_offset) / PAGE_SIZE -
				entry->folio_offset / PAGE_SIZE,
			make_dirty);
}

/* Unpin any pages held inside */
void rlist_cpu_destroy(struct rlist_cpu *rcpu, bool make_dirty)
{
	switch (rcpu->type) {
	case RLIST_CPU: {
		RLIST_CPU_STATE(rlscpu, rcpu);
		struct rlist_cpu_entry entry;

		/*
		 * For now rlist continuse the GUP protocol of refing every
		 * tail page in the folio.
		 * FIXME: This would be nicer with a folio function
		 */
		rlist_cpu_for_each_entry(&rlscpu, &entry)
			rlscpu_unpin_entry(&entry, make_dirty);
		return;
	}
	case RLIST_CPU_PAGES:
		unpin_user_pages_dirty_lock(rcpu->pages.pages, rcpu->pages.size,
					    make_dirty);
		return;
	case RLIST_CPU_BIO:
		/* rlist_cpu is just a reference, bio owns the page refs. */
		return;
	default:
		WARN(true, "Corrupt rlist_cpu");
		return;
	}
}
EXPORT_SYMBOL_GPL(rlist_cpu_destroy);

bool rlist_cpu_empty(struct rlist_cpu *rcpu)
{
	switch (rcpu->type) {
	case RLIST_CPU:
		return rlist_empty(&rcpu->rlist);
	case RLIST_CPU_PAGES:
		return !(rcpu->pages.pages && rcpu->pages.size);
	case RLIST_CPU_BIO:
		return rcpu->bio.bio->bi_iter.bi_size == 0;
	default:
		WARN(true, "Corrupt rlist_cpu");
		return true;
	}
}
EXPORT_SYMBOL_GPL(rlist_cpu_empty);

static __always_inline int rlist_cpu_copy(void *ptr, struct rlist_cpu *rcpu,
					  size_t offset, size_t length,
					  bool from)
{
	struct rlist_cpu_entry entry;
	RLIST_CPU_STATE(rls, rcpu);

	if (!rlscpu_seek(&rls, &entry, offset))
		return -EINVAL;
	while (true) {
		struct page *page;
		size_t position;
		size_t chunk;
		void *va;

		if (!rlscpu_read_page(&rls, &page, &entry))
			return -EINVAL;

		chunk = min(length, PAGE_SIZE - entry.page_offset);
		position = rlscpu_position(&rls);
		if (WARN_ON(offset < position + entry.page_offset ||
			    offset - position >= PAGE_SIZE))
			return -EINVAL;

		va = kmap_local_page(page);
		if (from)
			memcpy(ptr, va + offset - position, chunk);
		else
			memcpy(va + offset - position, ptr, chunk);
		kunmap_local(va);

		length -= chunk;
		ptr += chunk;
		offset += chunk;

		if (!rlscpu_next_page(&rls, &page, &entry))
			break;
	}
	if (length != 0)
		return -EINVAL;
	return 0;
}

int rlist_cpu_copy_from(void *dst, struct rlist_cpu *rcpu, size_t offset,
			size_t length)
{
	return rlist_cpu_copy(dst, rcpu, offset, length, true);
}
EXPORT_SYMBOL_GPL(rlist_cpu_copy_from);

int rlist_cpu_copy_to(struct rlist_cpu *rcpu, const void *src, size_t offset,
		      size_t length)
{
	return rlist_cpu_copy((void *)src, rcpu, offset, length, false);
}
EXPORT_SYMBOL_GPL(rlist_cpu_copy_to);

int rlscpu_append_begin(struct rlist_cpu_state_append *rlsa)
{
	rlsa->rlist_cpu->summary_flags = 0;
	rlsa->rlist_cpu->max_position = 0;

	switch (rlsa->rlist_cpu->type) {
	case RLIST_CPU:
		memset(&rlsa->cur, 0, sizeof(rlsa->cur));
		return rls_append_begin(&rlsa->rlsa);
	case RLIST_CPU_PAGES:
		rlsa->rls_pages.cur_page = rlsa->rlist_cpu->pages.pages;
		return true;
	default:
		WARN(true, "Corrupt rlist_cpu");
		return false;
	}
}
EXPORT_SYMBOL_GPL(rlscpu_append_begin);

/* rlsa->cur is garbage on return */
static int rlscpu_append_push_cur_rlist(struct rlist_cpu_state_append *rlsa,
					gfp_t gfp)
{
	bool first = rlsa->rlsa.rls.position != 0;
	unsigned int start;
	u64 length;
	int ret;

	length = rlsa->cur.length;
	switch (rlsa->cur.type) {
	case RLIST_CPU_MEM_FOLIO:
		start = rlsa->cur.folio_offset;
		break;
	case RLIST_CPU_MEM_PHYSICAL:
		start = rlsa->cur.phys;
		break;
	default:
		WARN(true, "Corrupt rlist_cpu");
		return -EINVAL;
	}

	rlscpu_encode_entry(&rlsa->cur);
	ret = rls_append(&rlsa->rlsa, &rlsa->cur._entry, gfp);
	if (ret) {
		rlscpu_decode_entry(&rlsa->cur);
		return ret;
	}

	/*
	 * PAGELIST means all pages but the first start on a page boundary, and
	 * all but the last end on a page boundary
	 */
	rlsa->rlist_cpu->summary_flags |= rlsa->last_summary_flags;
	rlsa->last_summary_flags = 0;
	if (!first && (start % PAGE_SIZE))
		rlsa->rlist_cpu->summary_flags |= RLIST_SUM_NOT_PAGELIST;
	if ((start + length) % PAGE_SIZE)
		rlsa->last_summary_flags = RLIST_SUM_NOT_PAGELIST;
	return 0;
}

int rlscpu_append_end(struct rlist_cpu_state_append *rlsa, gfp_t gfp)
{
	switch (rlsa->rlist_cpu->type) {
	case RLIST_CPU: {
		int ret = 0;

		if (rlsa->cur.length) {
			rlsa->last_summary_flags = 0;
			ret = rlscpu_append_push_cur_rlist(rlsa, gfp);
			if (ret)
				rlscpu_unpin_entry(&rlsa->cur, false);
			memset(&rlsa->cur, 0, sizeof(rlsa->cur));
		}
		if (!ret)
			rlsa->rlist_cpu->max_position = rlsa->rlsa.rls.position;
		rls_append_end(&rlsa->rlsa);
		return ret;
	}
	case RLIST_CPU_PAGES:
		rlsa->rlist_cpu->pages.size =
			rlsa->rls_pages.cur_page - rlsa->rlist_cpu->pages.pages;
		rlsa->rlist_cpu->max_position =
			rlsa->rlist_cpu->pages.size * PAGE_SIZE;
		return 0;
	default:
		WARN(true, "Corrupt rlist_cpu");
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(rlscpu_append_end);

void rlscpu_append_destroy_rlist(struct rlist_cpu_state_append *rlsa)
{
	if (rlsa->cur.length) {
		rlscpu_unpin_entry(&rlsa->cur, false);
		memset(&rlsa->cur, 0, sizeof(rlsa->cur));
	}
	rlscpu_append_end(rlsa, 0);
	rlist_cpu_destroy(rlsa->rlist_cpu, false);
}
EXPORT_SYMBOL_GPL(rlscpu_append_destroy_rlist);

int rlscpu_append_folio_pages(struct _rlist_cpu_pages_state *rlspages,
			      struct folio *folio, unsigned int offset,
			      size_t length)
{
	struct page *page;

	if (offset % PAGE_SIZE || length % PAGE_SIZE)
		return -EINVAL;

	if (rlspages->rlist_cpu->pages.pages +
		    rlspages->rlist_cpu->pages.available - rlspages->cur_page <
	    length / PAGE_SIZE)
		return -ENOSPC;

	page = folio_page(folio, offset / PAGE_SIZE);
	for (;length; length -= PAGE_SIZE) {
		*rlspages->cur_page = page;
		page = nth_page(page, 1);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(rlscpu_append_folio_pages);

static bool is_contiguous_folio(struct folio *cur_folio,
				unsigned int cur_folio_end, struct folio *folio,
				unsigned int offset)
{
	if (!cur_folio)
		return false;
	if (cur_folio == folio && cur_folio_end == offset)
		return true;
	if ((cur_folio_end % PAGE_SIZE) || (offset % PAGE_SIZE))
		return false;
	/*
	 * FIXME: folio accessor
	 */
	if (!zone_device_pages_have_same_pgmap(&cur_folio->page, &folio->page))
		return false;

	/*
	 * FIXME: Make a helper - this could be less instructions with knowledge
	 * of the memory model.
	 */
	if (folio_pfn(cur_folio) + cur_folio_end / PAGE_SIZE !=
	    folio_pfn(folio) + offset / PAGE_SIZE)
		return false;
	return true;
}

static int rlscpu_append_folio_rlist(struct rlist_cpu_state_append *rlsa,
				     struct folio *folio, unsigned int offset,
				     size_t length, gfp_t gfp)
{
	if (rlsa->cur.type == RLIST_CPU_MEM_FOLIO &&
	    is_contiguous_folio(rlsa->cur.folio,
				rlsa->cur.folio_offset + rlsa->cur.length,
				folio, offset)) {
		rlsa->cur.length += length;
		return 0;
	}

	if (rlsa->cur.length) {
		int ret;

		ret = rlscpu_append_push_cur_rlist(rlsa, gfp);
		if (ret)
			return ret;
	}

	/* No alloc mode will waste the last 24 bytes of the last chunk
	if (rlsa->rlsa.no_alloc && rls_is_full(&rlsa->rlsa)
	return -ENOSPC;
	*/

	rlsa->cur.type = RLIST_CPU_MEM_FOLIO;
	rlsa->cur.length = length;
	rlsa->cur.folio = folio;
	rlsa->cur.folio_offset = offset;
	rlsa->cur.provider_index = 0;
	/* FIXME: the refcounts assume we hold a refcount on every tail page
	 * in the span */
	return 0;
}

/*
 * "moves" the folio ref from the caller into the datastructure.
 * rlist_cpu_destory() will perform the put. FIXME: caller must have got a ref
 * on every tail page of the folio range. FIXME: length should be within the
 * same folio, if not within the same folio then the group of folios must share
 * the same pgmap/etc.
 */
int rlscpu_append_folio(struct rlist_cpu_state_append *rlsa,
			struct folio *folio, unsigned int offset, size_t length,
			gfp_t gfp)
{
	int ret;

	switch (rlsa->rlist_cpu->type) {
	case RLIST_CPU:
		ret = rlscpu_append_folio_rlist(rlsa, folio, offset, length,
						 gfp);
		break;
	case RLIST_CPU_PAGES:
		ret =  rlscpu_append_folio_pages(&rlsa->rls_pages, folio,
						 offset, length);
		break;
	default:
		WARN(true, "Corrupt rlist_cpu");
		return -EINVAL;
	}
	if (ret)
		return ret;

	if (is_pci_p2pdma_page(&folio->page))
		rlsa->rlist_cpu->summary_flags |= RLIST_SUM_HAS_P2PDMA_PAGE;
	return 0;
}
EXPORT_SYMBOL_GPL(rlscpu_append_folio);

static int rlscpu_append_phys_rlist(struct rlist_cpu_state_append *rlsa,
				    phys_addr_t base, u64 length,
				    struct p2pdma_provider *provider, gfp_t gfp)
{
	if (rlsa->cur.length) {
		int ret;

		ret = rlscpu_append_push_cur_rlist(rlsa, gfp);
		if (ret)
			return ret;
		rlsa->cur.length = 0;
	}

	rlsa->cur = (struct rlist_cpu_entry){
		.type = RLIST_CPU_MEM_PHYSICAL,
		.length = length,
		.phys = base,
		.provider_index = provider->provider_id,
	};
	return rlscpu_append_push_cur_rlist(rlsa, gfp);
}

/*
 * The caller must ensure the provider continues to exist as long as the
 * rlist_cpu exists.
 */
int rlscpu_append_physical(struct rlist_cpu_state_append *rlsa,
			   phys_addr_t base, u64 length,
			   struct p2pdma_provider *provider, gfp_t gfp)
{
	int ret;

	switch (rlsa->rlist_cpu->type) {
	case RLIST_CPU:
		ret = rlscpu_append_phys_rlist(rlsa, base, length, provider,
					       gfp);
		break;
	case RLIST_CPU_PAGES:
		return -EOPNOTSUPP;
	default:
		WARN(true, "Corrupt rlist_cpu");
		return -EINVAL;
	}
	if (ret)
		return ret;

	rlsa->rlist_cpu->summary_flags |= RLIST_SUM_HAS_P2PDMA_PAGE;
	return 0;
}
EXPORT_SYMBOL_GPL(rlscpu_append_physical);
