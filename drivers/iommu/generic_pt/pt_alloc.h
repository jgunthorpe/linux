/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
#ifndef __GENERIC_PT_PT_ALLOC_H
#define __GENERIC_PT_PT_ALLOC_H

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/device.h>

/*
 * Per radix table level allocation meta data. This is very similar in purpose
 * to the struct ptdesc.
 *
 * radix levels have special properties:
 *   - Always a power of two size
 *   - Can be threaded on a list without a memory allocation
 *   - Can be RCU freed without a memory allocation
 */
struct pt_radix_meta {
	unsigned long __page_flags;

	struct rcu_head rcu_head;
	union {
		struct {
			u8 zero; /* Lower bits of page mapping must be zero */
			u8 lg2sz;
			u8 incoherent;
			u8 still_flushing;
		};
		unsigned long __page_mapping;
	};
	struct pt_common *owner;
	struct pt_radix_meta *free_next;

	unsigned int __page_type;
	atomic_t __page_refcount;
#ifdef CONFIG_MEMCG
	unsigned long memcg_data;
#endif
};

static inline struct pt_radix_meta *folio_to_meta(struct folio *folio)
{
	return (struct pt_radix_meta *)folio;
}

static inline struct pt_radix_meta *virt_to_meta(const void *addr)
{
	return folio_to_meta(virt_to_folio(addr));
}

// FIXME remove head from the name
struct pt_radix_list_head {
	unsigned long num_items;
	struct pt_radix_meta *head;
	struct pt_radix_meta *tail;
};
#define PT_RADIX_LIST_INIT ((struct pt_radix_list_head){})

void *pt_radix_alloc(struct pt_common *owner, int nid, size_t log2size,
		     gfp_t gfp);
void pt_radix_free(void *radix);
void pt_radix_free_list(struct pt_radix_list_head *list);
void pt_radix_free_list_rcu(struct pt_radix_list_head *list);

static inline void pt_radix_add_list(struct pt_radix_list_head *list,
				     void *radix)
{
	struct pt_radix_meta *meta = virt_to_meta(radix);

	list->num_items++;
	meta->free_next = NULL;
	if (list->tail)
		list->tail->free_next = meta;
	else
		list->head = meta;
	list->tail = meta;
}

static inline void pt_radix_list_splice(struct pt_radix_list_head *to,
					const struct pt_radix_list_head *from)
{
	if (!from->head)
		return;

	to->num_items += from->num_items;
	if (to->head) {
		to->tail->free_next = from->head;
		to->tail = from->tail;
	} else {
		to->head = from->head;
		to->tail = from->tail;
	}
}

int pt_radix_start_incoherent(void *radix, struct device *dma_dev,
			      bool still_flushing);
int pt_radix_start_incoherent_list(struct pt_radix_list_head *list,
				   struct device *dma_dev);
void pt_radix_stop_incoherent_list(struct pt_radix_list_head *list,
				   struct device *dma_dev);

static inline void pt_radix_done_incoherent_flush(void *radix)
{
	struct pt_radix_meta *meta = virt_to_meta(radix);

	/*
	 * Release/acquire is against the cache flush,
	 * pt_radix_still_incoherent() must not return 0 until the HW observes
	 * the flush.
	 */
	smp_store_release(&meta->still_flushing, 0);
}

static inline bool pt_radix_incoherent_still_flushing(void *radix)
{
	struct pt_radix_meta *meta = virt_to_meta(radix);

	return smp_load_acquire(&meta->still_flushing);
}

#endif
