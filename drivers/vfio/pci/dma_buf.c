// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 */
#include <linux/dma-buf.h>
#include <linux/pci-p2pdma.h>
#include <linux/dma-resv.h>

#include "vfio_pci_priv.h"

MODULE_IMPORT_NS(DMA_BUF);

struct vfio_pci_dma_buf {
	struct dma_buf *dmabuf;
	struct vfio_pci_core_device *vdev;
	struct list_head dmabufs_elm;
	unsigned int index;
	unsigned int orig_nents;
	size_t offset;
	bool revoked;
};

static int vfio_pci_dma_buf_attach(struct dma_buf *dmabuf,
				   struct dma_buf_attachment *attachment)
{
	struct vfio_pci_dma_buf *priv = dmabuf->priv;
	int rc;

	rc = pci_p2pdma_distance_many(priv->vdev->pdev, &attachment->dev, 1,
				      true);
	if (rc < 0)
		attachment->peer2peer = false;
	return 0;
}

static void vfio_pci_dma_buf_unpin(struct dma_buf_attachment *attachment)
{
}

static int vfio_pci_dma_buf_pin(struct dma_buf_attachment *attachment)
{
	/*
	 * Uses the dynamic interface but must always allow for
	 * dma_buf_move_notify() to do revoke
	 */
	return -EINVAL;
}

static struct sg_table *
vfio_pci_dma_buf_map(struct dma_buf_attachment *attachment,
		     enum dma_data_direction dir)
{
	size_t sgl_size = dma_get_max_seg_size(attachment->dev);
	struct vfio_pci_dma_buf *priv = attachment->dmabuf->priv;
	struct scatterlist *sgl;
	struct sg_table *sgt;
	dma_addr_t dma_addr;
	unsigned int nents;
	size_t offset;
	int ret;

	dma_resv_assert_held(priv->dmabuf->resv);

	if (!attachment->peer2peer)
		return ERR_PTR(-EPERM);

	if (priv->revoked)
		return ERR_PTR(-ENODEV);

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	nents = DIV_ROUND_UP(priv->dmabuf->size, sgl_size);
	ret = sg_alloc_table(sgt, nents, GFP_KERNEL);
	if (ret)
		goto err_kfree_sgt;

	/*
	 * Since the memory being mapped is a device memory it could never be in
	 * CPU caches.
	 */
	dma_addr = dma_map_resource(
		attachment->dev,
		pci_resource_start(priv->vdev->pdev, priv->index) +
			priv->offset,
		priv->dmabuf->size, dir, DMA_ATTR_SKIP_CPU_SYNC);
	ret = dma_mapping_error(attachment->dev, dma_addr);
	if (ret)
		goto err_free_sgt;

	/*
	 * Break the BAR's physical range up into max sized SGL's according to
	 * the device's requirement.
	 */
	sgl = sgt->sgl;
	for (offset = 0; offset != priv->dmabuf->size;) {
		size_t chunk_size = min(priv->dmabuf->size - offset, sgl_size);

		sg_set_page(sgl, NULL, chunk_size, 0);
		sg_dma_address(sgl) = dma_addr + offset;
		sg_dma_len(sgl) = chunk_size;
		sgl = sg_next(sgl);
		offset += chunk_size;
	}

	/*
	 * Because we are not going to include a CPU list we want to have some
	 * chance that other users will detect this by setting the orig_nents to
	 * 0 and using only nents (length of DMA list) when going over the sgl
	 */
	priv->orig_nents = sgt->orig_nents;
	sgt->orig_nents = 0;
	return sgt;

err_free_sgt:
	sg_free_table(sgt);
err_kfree_sgt:
	kfree(sgt);
	return ERR_PTR(ret);
}

static void vfio_pci_dma_buf_unmap(struct dma_buf_attachment *attachment,
				   struct sg_table *sgt,
				   enum dma_data_direction dir)
{
	struct vfio_pci_dma_buf *priv = attachment->dmabuf->priv;

	sgt->orig_nents = priv->orig_nents;
	dma_unmap_resource(attachment->dev, sg_dma_address(sgt->sgl),
			   priv->dmabuf->size, dir, DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(sgt);
	kfree(sgt);
}

static void vfio_pci_dma_buf_release(struct dma_buf *dmabuf)
{
	struct vfio_pci_dma_buf *priv = dmabuf->priv;

	/*
	 * Either this or vfio_pci_dma_buf_cleanup() will remove from the list.
	 * The refcount prevents both.
	 */
	if (priv->vdev) {
		down_write(&priv->vdev->memory_lock);
		list_del_init(&priv->dmabufs_elm);
		up_write(&priv->vdev->memory_lock);
		vfio_device_put(&priv->vdev->vdev);
	}
	kfree(priv);
}

static const struct dma_buf_ops vfio_pci_dmabuf_ops = {
	.attach = vfio_pci_dma_buf_attach,
	.map_dma_buf = vfio_pci_dma_buf_map,
	.pin = vfio_pci_dma_buf_pin,
	.unpin = vfio_pci_dma_buf_unpin,
	.release = vfio_pci_dma_buf_release,
	.unmap_dma_buf = vfio_pci_dma_buf_unmap,
};

int vfio_pci_core_feature_dma_buf(struct vfio_pci_core_device *vdev, u32 flags,
				  struct vfio_device_feature_dma_buf __user *arg,
				  size_t argsz)
{
	struct vfio_device_feature_dma_buf get_dma_buf;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct vfio_pci_dma_buf *priv;
	int ret;

	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_GET,
				 sizeof(get_dma_buf));
	if (ret != 1)
		return ret;

	if (copy_from_user(&get_dma_buf, arg, sizeof(get_dma_buf)))
		return -EFAULT;

	/* For PCI the region_index is the BAR number like everything else */
	if (get_dma_buf.region_index >= VFIO_PCI_ROM_REGION_INDEX)
		return -EINVAL;

	exp_info.ops = &vfio_pci_dmabuf_ops;
	exp_info.size = pci_resource_len(vdev->pdev, get_dma_buf.region_index);
	if (!exp_info.size)
		return -EINVAL;
	if (get_dma_buf.offset || get_dma_buf.length) {
		if (get_dma_buf.length > exp_info.size ||
		    get_dma_buf.offset >= exp_info.size ||
		    get_dma_buf.length > exp_info.size - get_dma_buf.offset ||
		    get_dma_buf.offset % PAGE_SIZE ||
		    get_dma_buf.length % PAGE_SIZE)
			return -EINVAL;
		exp_info.size = get_dma_buf.length;
	}
	exp_info.flags = get_dma_buf.open_flags;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	INIT_LIST_HEAD(&priv->dmabufs_elm);
	priv->offset = get_dma_buf.offset;
	priv->index = get_dma_buf.region_index;

	exp_info.priv = priv;
	priv->dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(priv->dmabuf)) {
		ret = PTR_ERR(priv->dmabuf);
		kfree(priv);
		return ret;
	}

	/* dma_buf_put() now frees priv */

	down_write(&vdev->memory_lock);
	dma_resv_lock(priv->dmabuf->resv, NULL);
	priv->revoked = !__vfio_pci_memory_enabled(vdev);
	priv->vdev = vdev;
	vfio_device_get(&vdev->vdev);
	list_add_tail(&priv->dmabufs_elm, &vdev->dmabufs);
	dma_resv_unlock(priv->dmabuf->resv);
	up_write(&vdev->memory_lock);

	/*
	 * dma_buf_fd() consumes the reference, when the file closes the dmabuf
	 * will be released.
	 */
	return dma_buf_fd(priv->dmabuf, get_dma_buf.open_flags);
}

void vfio_pci_dma_buf_move(struct vfio_pci_core_device *vdev, bool revoked)
{
	struct vfio_pci_dma_buf *priv;
	struct vfio_pci_dma_buf *tmp;

	lockdep_assert_held_write(&vdev->memory_lock);

	list_for_each_entry_safe(priv, tmp, &vdev->dmabufs, dmabufs_elm) {
		if (!dma_buf_try_get(priv->dmabuf))
			continue;
		if (priv->revoked != revoked) {
			dma_resv_lock(priv->dmabuf->resv, NULL);
			priv->revoked = revoked;
			dma_buf_move_notify(priv->dmabuf);
			dma_resv_unlock(priv->dmabuf->resv);
		}
		dma_buf_put(priv->dmabuf);
	}
}

void vfio_pci_dma_buf_cleanup(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_dma_buf *priv;
	struct vfio_pci_dma_buf *tmp;

	down_write(&vdev->memory_lock);
	list_for_each_entry_safe(priv, tmp, &vdev->dmabufs, dmabufs_elm) {
		if (!dma_buf_try_get(priv->dmabuf))
			continue;
		dma_resv_lock(priv->dmabuf->resv, NULL);
		list_del_init(&priv->dmabufs_elm);
		priv->vdev = NULL;
		priv->revoked = true;
		dma_buf_move_notify(priv->dmabuf);
		dma_resv_unlock(priv->dmabuf->resv);
		vfio_device_put(&vdev->vdev);
		dma_buf_put(priv->dmabuf);
	}
	up_write(&vdev->memory_lock);
}
