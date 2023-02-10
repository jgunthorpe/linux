/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/*
 * Copyright (c) 2007 Cisco Systems.  All rights reserved.
 * Copyright (c) 2020 Intel Corporation.  All rights reserved.
 */

#ifndef IB_UMEM_H
#define IB_UMEM_H

#include <linux/list.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>
#include <rdma/ib_verbs.h>
#include <linux/rlist_dma.h>

struct ib_ucontext;
struct ib_umem_odp;
struct dma_buf_attach_ops;

struct ib_umem {
	struct ib_device       *ibdev;
	struct mm_struct       *owning_mm;
	u64 iova;
	unsigned long		address;
	u32 writable : 1;
	u32 is_odp : 1;
	u32 is_dmabuf : 1;
	struct rlist_cpu rcpu;
	struct rlist_dma rdma;
};

struct ib_umem_dmabuf {
	struct ib_umem umem;
	struct dma_buf_attachment *attach;
	void *private;
	size_t length;
	u8 pinned : 1;
};

static inline struct ib_umem_dmabuf *to_ib_umem_dmabuf(struct ib_umem *umem)
{
	return container_of(umem, struct ib_umem_dmabuf, umem);
}

/* Returns the offset of the umem start relative to the first page. */
static inline int ib_umem_offset(struct ib_umem *umem)
{
	return umem->address & ~PAGE_MASK;
}

static inline unsigned long ib_umem_dma_offset(struct ib_umem *umem,
					       unsigned long pgsz)
{
	return rlist_dma_block_offset(&umem->rdma, pgsz);
}

static inline size_t ib_umem_num_dma_blocks(struct ib_umem *umem,
					    unsigned long pgsz)
{
	return rlist_dma_num_blocks(&umem->rdma, pgsz);
}

static inline size_t ib_umem_num_pages(struct ib_umem *umem)
{
	return rlist_dma_num_blocks(&umem->rdma, PAGE_SIZE);
}

static inline size_t ib_umem_length(struct ib_umem *umem)
{
	return rlist_cpu_length(&umem->rcpu);
}

struct ib_block_iter {
	struct rlist_dma_state rls;
	struct rlist_dma_entry entry;
};

/**
 * rdma_umem_for_each_dma_block - iterate over contiguous DMA blocks of the umem
 * @umem: umem to iterate over
 * @pgsz: Page size to split the list into
 *
 * pgsz must be <= PAGE_SIZE or computed by ib_umem_find_best_pgsz(). The
 * returned DMA blocks will be aligned to pgsz and span the range:
 * ALIGN_DOWN(umem->address, pgsz) to ALIGN(umem->address + umem->length, pgsz)
 *
 * Performs exactly ib_umem_num_dma_blocks() iterations.
 */
static inline void ____rdma_umem_block_iter_start(struct ib_block_iter *biter,
						  struct ib_umem *umem,
						  unsigned long blocksz)
{
	rlsdma_init(&biter->rls, &umem->rdma);
	biter->rls.valid =
		rlsdma_block_iter_reset(&biter->rls, &biter->entry, blocksz);
}

#define rdma_umem_for_each_dma_block(umem, biter, blocksz)         \
	for (____rdma_umem_block_iter_start(biter, umem, blocksz); \
	     (biter)->rls.valid;                                   \
	     (biter)->rls.valid = rlsdma_block_iter_next(          \
		     &(biter)->rls, &(biter)->entry, blocksz))

/**
 * rdma_block_iter_dma_address - get the aligned dma address of the current
 * block held by the block iterator.
 * @biter: block iterator holding the memory block
 */
static inline dma_addr_t
rdma_block_iter_dma_address(struct ib_block_iter *biter)
{
	return biter->entry.dma_address;
}

#ifdef CONFIG_INFINIBAND_USER_MEM

struct ib_umem *ib_umem_get(struct ib_device *device, unsigned long addr,
			    size_t size, int access);
void ib_umem_release(struct ib_umem *umem);

/*
 * Copy from the given ib_umem's pages to the given buffer.
 *
 * umem - the umem to copy from
 * offset - offset to start copying from
 * dst - destination buffer
 * length - buffer length
 *
 * Returns 0 on success, or an error code.
 */
static inline int ib_umem_copy_from(void *dst, struct ib_umem *umem,
				    size_t offset, size_t length)
{
	return rlist_cpu_copy_from(dst, &umem->rcpu, offset, length);
}

static inline unsigned long ib_umem_find_best_pgsz(struct ib_umem *umem,
						   unsigned long pgsz_bitmap,
						   unsigned long virt)
{
	struct rlist_dma_segmentation segment = {
		.has_block_list_hwva = 1,
		.block_list_supported = pgsz_bitmap,
		.block_list_hwva = umem->iova,
	};
	return rlist_dma_find_best_blocksz(&umem->rdma, &segment);
}

/**
 * ib_umem_find_best_pgoff - Find best HW page size
 *
 * @umem: umem struct
 * @pgsz_bitmap bitmap of HW supported page sizes
 * @pgoff_bitmask: Mask of bits that can be represented with an offset
 *
 * This is very similar to ib_umem_find_best_pgsz() except instead of accepting
 * an IOVA it accepts a bitmask specifying what address bits can be represented
 * with a page offset.
 *
 * For instance if the HW has multiple page sizes, requires 64 byte alignemnt,
 * and can support aligned offsets up to 4032 then pgoff_bitmask would be
 * "111111000000".
 *
 * If the pgoff_bitmask requires either alignment in the low bit or an
 * unavailable page size for the high bits, this function returns 0.
 */
static inline unsigned long ib_umem_find_best_pgoff(struct ib_umem *umem,
						    unsigned long pgsz_bitmap,
						    u64 pgoff_bitmask)
{
	struct rlist_dma_entry entry;
	struct rlist_dma_segmentation segment = {
		.has_block_list_hwva = 1,
		.block_list_supported = pgsz_bitmap,
	};

	if (!rlist_dma_first(&umem->rdma, &entry))
		return 0;

	segment.block_list_hwva = entry.dma_address & pgoff_bitmask;
	return rlist_dma_find_best_blocksz(&umem->rdma, &segment);
}

struct ib_umem_dmabuf *ib_umem_dmabuf_get(struct ib_device *device,
					  unsigned long offset, size_t size,
					  int fd, int access,
					  const struct dma_buf_attach_ops *ops);
struct ib_umem_dmabuf *ib_umem_dmabuf_get_pinned(struct ib_device *device,
						 unsigned long offset,
						 size_t size, int fd,
						 int access);
int ib_umem_dmabuf_map_pages(struct ib_umem_dmabuf *umem_dmabuf);
void ib_umem_dmabuf_unmap_pages(struct ib_umem_dmabuf *umem_dmabuf);
void ib_umem_dmabuf_release(struct ib_umem_dmabuf *umem_dmabuf);

#else /* CONFIG_INFINIBAND_USER_MEM */

#include <linux/err.h>

static inline struct ib_umem *ib_umem_get(struct ib_device *device,
					  unsigned long addr, size_t size,
					  int access)
{
	return ERR_PTR(-EOPNOTSUPP);
}
static inline void ib_umem_release(struct ib_umem *umem) { }
static inline int ib_umem_copy_from(void *dst, struct ib_umem *umem, size_t offset,
		      		    size_t length) {
	return -EOPNOTSUPP;
}
static inline unsigned long ib_umem_find_best_pgsz(struct ib_umem *umem,
						   unsigned long pgsz_bitmap,
						   unsigned long virt)
{
	return 0;
}
static inline unsigned long ib_umem_find_best_pgoff(struct ib_umem *umem,
						    unsigned long pgsz_bitmap,
						    u64 pgoff_bitmask)
{
	return 0;
}
static inline
struct ib_umem_dmabuf *ib_umem_dmabuf_get(struct ib_device *device,
					  unsigned long offset,
					  size_t size, int fd,
					  int access,
					  struct dma_buf_attach_ops *ops)
{
	return ERR_PTR(-EOPNOTSUPP);
}
static inline struct ib_umem_dmabuf *
ib_umem_dmabuf_get_pinned(struct ib_device *device, unsigned long offset,
			  size_t size, int fd, int access)
{
	return ERR_PTR(-EOPNOTSUPP);
}
static inline int ib_umem_dmabuf_map_pages(struct ib_umem_dmabuf *umem_dmabuf)
{
	return -EOPNOTSUPP;
}
static inline void ib_umem_dmabuf_unmap_pages(struct ib_umem_dmabuf *umem_dmabuf) { }
static inline void ib_umem_dmabuf_release(struct ib_umem_dmabuf *umem_dmabuf) { }

#endif /* CONFIG_INFINIBAND_USER_MEM */
#endif /* IB_UMEM_H */
