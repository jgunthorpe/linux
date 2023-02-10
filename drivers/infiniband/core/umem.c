/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Cisco Systems.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2020 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/count_zeros.h>
#include <rdma/ib_umem_odp.h>

#include "uverbs.h"

/**
 * ib_umem_get - Pin and DMA map userspace memory.
 *
 * @device: IB device to connect UMEM
 * @addr: userspace virtual address to start at
 * @size: length of region to pin
 * @access: IB_ACCESS_xxx flags for memory being pinned
 */
struct ib_umem *ib_umem_get(struct ib_device *device, unsigned long addr,
			    size_t size, int access)
{
	struct ib_umem *umem;
	unsigned long lock_limit;
	unsigned long new_pinned;
	unsigned long dma_attr = 0;
	struct mm_struct *mm;
	unsigned long npages;
	int ret;
	unsigned int gup_flags = FOLL_LONGTERM;

	/*
	 * If the combination of the addr and size requested for this memory
	 * region causes an integer overflow, return error.
	 */
	if (((addr + size) < addr) ||
	    PAGE_ALIGN(addr + size) < (addr + size))
		return ERR_PTR(-EINVAL);

	if (!can_do_mlock())
		return ERR_PTR(-EPERM);

	if (access & IB_ACCESS_ON_DEMAND)
		return ERR_PTR(-EOPNOTSUPP);

	npages = ALIGN(addr + size, PAGE_SIZE) -  ALIGN_DOWN(addr, PAGE_SIZE);
	if (npages == 0 || npages > UINT_MAX)
		return ERR_PTR(-EINVAL);

	umem = kzalloc(sizeof(*umem), GFP_KERNEL);
	if (!umem)
		return ERR_PTR(-ENOMEM);
	umem->ibdev      = device;
	umem->address    = addr;

	/*
	 * Drivers should call ib_umem_find_best_pgsz() to set the iova
	 * correctly.
	 */
	umem->iova = addr;
	umem->writable   = ib_access_writable(access);
	umem->owning_mm = mm = current->mm;
	mmgrab(mm);

	lock_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;

	new_pinned = atomic64_add_return(npages, &mm->pinned_vm);
	if (new_pinned > lock_limit && !capable(CAP_IPC_LOCK)) {
		atomic64_sub(npages, &mm->pinned_vm);
		ret = -ENOMEM;
		goto umem_kfree;
	}

	if (umem->writable)
		gup_flags |= FOLL_WRITE;

	/* FIXME: somewhere up from this call chain we need a u64_to_uptr() */
	ret = pin_user_pages_rlist(mm, &umem->rcpu, (void __user *)addr, size,
				   gup_flags, NULL);
	if (ret)
		goto err_pinned_vm;

	if (access & IB_ACCESS_RELAXED_ORDERING)
		dma_attr |= DMA_ATTR_WEAK_ORDERING;

	/* FIXME: need to pass in the mr "hca_va" as HWVA through segmentation */
	ret = ib_dma_map_rlist(device, &umem->rcpu, &umem->rdma,
			       &rlist_no_segmentation, DMA_BIDIRECTIONAL,
			       dma_attr, GFP_KERNEL);
	if (ret)
		goto err_rlist;
	return umem;

err_rlist:
	rlist_cpu_destroy(&umem->rcpu, false);
err_pinned_vm:
	atomic64_sub(npages, &mm->pinned_vm);
umem_kfree:
	mmdrop(umem->owning_mm);
	kfree(umem);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(ib_umem_get);

/**
 * ib_umem_release - release memory pinned with ib_umem_get
 * @umem: umem struct to release
 */
void ib_umem_release(struct ib_umem *umem)
{
	if (!umem)
		return;
	if (umem->is_dmabuf)
		return ib_umem_dmabuf_release(to_ib_umem_dmabuf(umem));
	if (umem->is_odp)
		return ib_umem_odp_release(to_ib_umem_odp(umem));

	ib_dma_unmap_rlist(umem->ibdev, &umem->rdma, DMA_BIDIRECTIONAL, 0);
	rlist_cpu_destroy(&umem->rcpu, true);

	atomic64_sub(ib_umem_num_pages(umem), &umem->owning_mm->pinned_vm);
	mmdrop(umem->owning_mm);
	kfree(umem);
}
EXPORT_SYMBOL(ib_umem_release);
