// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Copyright (c) 2020 Intel Corporation. All rights reserved.
 */

#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>

#include "uverbs.h"

MODULE_IMPORT_NS(DMA_BUF);

int ib_umem_dmabuf_map_pages(struct ib_umem_dmabuf *umem_dmabuf)
{
	long ret;

	dma_resv_assert_held(umem_dmabuf->attach->dmabuf->resv);

	if (!rlist_cpu_empty(&umem_dmabuf->umem.rcpu))
		goto wait_fence;

	ret = dma_buf_map_attachment_rlist(umem_dmabuf->attach,
					   umem_dmabuf->umem.address,
					   umem_dmabuf->length,
					   &umem_dmabuf->umem.rcpu,
					   DMA_BIDIRECTIONAL);
	if (ret)
		return ret;

	ret = ib_dma_map_rlist(umem_dmabuf->umem.ibdev, &umem_dmabuf->umem.rcpu,
			       &umem_dmabuf->umem.rdma, &rlist_no_segmentation,
			       DMA_BIDIRECTIONAL, 0, GFP_KERNEL);
	if (ret) {
		dma_buf_unmap_attachment_rlist(umem_dmabuf->attach,
					       &umem_dmabuf->umem.rcpu,
					       DMA_BIDIRECTIONAL);
		return ret;
	}

wait_fence:
	/*
	 * Although the sg list is valid now, the content of the pages
	 * may be not up-to-date. Wait for the exporter to finish
	 * the migration.
	 */
	ret = dma_resv_wait_timeout(umem_dmabuf->attach->dmabuf->resv,
				     DMA_RESV_USAGE_KERNEL,
				     false, MAX_SCHEDULE_TIMEOUT);
	if (ret < 0)
		return ret;
	if (ret == 0)
		return -ETIMEDOUT;
	return 0;
}
EXPORT_SYMBOL(ib_umem_dmabuf_map_pages);

void ib_umem_dmabuf_unmap_pages(struct ib_umem_dmabuf *umem_dmabuf)
{
	dma_resv_assert_held(umem_dmabuf->attach->dmabuf->resv);

	if (rlist_cpu_empty(&umem_dmabuf->umem.rcpu))
		return;

	ib_dma_unmap_rlist(umem_dmabuf->umem.ibdev, &umem_dmabuf->umem.rdma,
			   DMA_BIDIRECTIONAL, 0);
	dma_buf_unmap_attachment_rlist(umem_dmabuf->attach,
				       &umem_dmabuf->umem.rcpu,
				       DMA_BIDIRECTIONAL);
	WARN_ON(!rlist_cpu_empty(&umem_dmabuf->umem.rcpu));
}
EXPORT_SYMBOL(ib_umem_dmabuf_unmap_pages);

struct ib_umem_dmabuf *ib_umem_dmabuf_get(struct ib_device *device,
					  unsigned long offset, size_t size,
					  int fd, int access,
					  const struct dma_buf_attach_ops *ops)
{
	struct dma_buf *dmabuf;
	struct ib_umem_dmabuf *umem_dmabuf;
	struct ib_umem *umem;
	unsigned long end;
	struct ib_umem_dmabuf *ret = ERR_PTR(-EINVAL);

	if (check_add_overflow(offset, (unsigned long)size, &end))
		return ret;

	if (unlikely(!ops || !ops->move_notify))
		return ret;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return ERR_CAST(dmabuf);

	if (dmabuf->size < end)
		goto out_release_dmabuf;

	umem_dmabuf = kzalloc(sizeof(*umem_dmabuf), GFP_KERNEL);
	if (!umem_dmabuf) {
		ret = ERR_PTR(-ENOMEM);
		goto out_release_dmabuf;
	}

	umem = &umem_dmabuf->umem;
	umem->ibdev = device;
	umem_dmabuf->length = size;
	umem->address = offset;
	umem->writable = ib_access_writable(access);
	umem->is_dmabuf = 1;

	if (!ib_umem_num_pages(umem))
		goto out_free_umem;

	umem_dmabuf->attach = dma_buf_dynamic_attach(
					dmabuf,
					/* FIXME: device should never be used in rlist mode */
					device->dma_device,
					ops,
					umem_dmabuf);
	if (IS_ERR(umem_dmabuf->attach)) {
		ret = ERR_CAST(umem_dmabuf->attach);
		goto out_free_umem;
	}
	return umem_dmabuf;

out_free_umem:
	kfree(umem_dmabuf);

out_release_dmabuf:
	dma_buf_put(dmabuf);
	return ret;
}
EXPORT_SYMBOL(ib_umem_dmabuf_get);

static void
ib_umem_dmabuf_unsupported_move_notify(struct dma_buf_attachment *attach)
{
	struct ib_umem_dmabuf *umem_dmabuf = attach->importer_priv;

	ibdev_warn_ratelimited(umem_dmabuf->umem.ibdev,
			       "Invalidate callback should not be called when memory is pinned\n");
}

static struct dma_buf_attach_ops ib_umem_dmabuf_attach_pinned_ops = {
	.allow_peer2peer = true,
	.move_notify = ib_umem_dmabuf_unsupported_move_notify,
};

struct ib_umem_dmabuf *ib_umem_dmabuf_get_pinned(struct ib_device *device,
						 unsigned long offset,
						 size_t size, int fd,
						 int access)
{
	struct ib_umem_dmabuf *umem_dmabuf;
	int err;

	umem_dmabuf = ib_umem_dmabuf_get(device, offset, size, fd, access,
					 &ib_umem_dmabuf_attach_pinned_ops);
	if (IS_ERR(umem_dmabuf))
		return umem_dmabuf;

	dma_resv_lock(umem_dmabuf->attach->dmabuf->resv, NULL);
	err = dma_buf_pin(umem_dmabuf->attach);
	if (err)
		goto err_release;
	umem_dmabuf->pinned = 1;

	err = ib_umem_dmabuf_map_pages(umem_dmabuf);
	if (err)
		goto err_unpin;
	dma_resv_unlock(umem_dmabuf->attach->dmabuf->resv);

	return umem_dmabuf;

err_unpin:
	dma_buf_unpin(umem_dmabuf->attach);
err_release:
	dma_resv_unlock(umem_dmabuf->attach->dmabuf->resv);
	ib_umem_release(&umem_dmabuf->umem);
	return ERR_PTR(err);
}
EXPORT_SYMBOL(ib_umem_dmabuf_get_pinned);

void ib_umem_dmabuf_release(struct ib_umem_dmabuf *umem_dmabuf)
{
	struct dma_buf *dmabuf = umem_dmabuf->attach->dmabuf;

	dma_resv_lock(dmabuf->resv, NULL);
	ib_umem_dmabuf_unmap_pages(umem_dmabuf);
	if (umem_dmabuf->pinned)
		dma_buf_unpin(umem_dmabuf->attach);
	dma_resv_unlock(dmabuf->resv);

	dma_buf_detach(dmabuf, umem_dmabuf->attach);
	dma_buf_put(dmabuf);
	kfree(umem_dmabuf);
}
