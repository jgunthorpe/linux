// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/sched/mm.h>
#include <linux/vfio_pci_core.h>

#include "cmd.h"

enum {
	MLX5VF_PCI_FREEZED = 1 << 0,
};

enum {
	MLX5VF_REGION_PENDING_BYTES = 1 << 0,
	MLX5VF_REGION_DATA_SIZE = 1 << 1,
};

#define MLX5VF_MIG_REGION_DATA_SIZE SZ_64M
/* Data section offset from migration region */
#define MLX5VF_MIG_REGION_DATA_OFFSET                                          \
	(sizeof(struct vfio_device_migration_info))

#define VFIO_DEVICE_MIGRATION_OFFSET(x)                                        \
	(offsetof(struct vfio_device_migration_info, x))

struct mlx5vf_pci_migration_info {
	u32 vfio_dev_fsm;
	u32 dev_state; /* device migration state */
	u32 region_state; /* Use MLX5VF_REGION_XXX */
	u16 vhca_id;
	struct mlx5_vhca_state_data vhca_state_data;
};

struct mlx5vf_pci_core_device {
	struct vfio_pci_core_device core_device;
	u8 migrate_cap:1;
	/* protect migration state */
	struct mutex state_mutex;
	struct mlx5vf_pci_migration_info vmig;
};

static int mlx5vf_pci_unquiesce_device(struct mlx5vf_pci_core_device *mvdev)
{
	return mlx5vf_cmd_resume_vhca(mvdev->core_device.pdev,
				      mvdev->vmig.vhca_id,
				      MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_MASTER);
}

static int mlx5vf_pci_quiesce_device(struct mlx5vf_pci_core_device *mvdev)
{
	return mlx5vf_cmd_suspend_vhca(
		mvdev->core_device.pdev, mvdev->vmig.vhca_id,
		MLX5_SUSPEND_VHCA_IN_OP_MOD_SUSPEND_MASTER);
}

static int mlx5vf_pci_unfreeze_device(struct mlx5vf_pci_core_device *mvdev)
{
	int ret;

	ret = mlx5vf_cmd_resume_vhca(mvdev->core_device.pdev,
				     mvdev->vmig.vhca_id,
				     MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_SLAVE);
	if (ret)
		return ret;

	mvdev->vmig.dev_state &= ~MLX5VF_PCI_FREEZED;
	return 0;
}

static int mlx5vf_pci_freeze_device(struct mlx5vf_pci_core_device *mvdev)
{
	int ret;

	ret = mlx5vf_cmd_suspend_vhca(
		mvdev->core_device.pdev, mvdev->vmig.vhca_id,
		MLX5_SUSPEND_VHCA_IN_OP_MOD_SUSPEND_SLAVE);
	if (ret)
		return ret;

	mvdev->vmig.dev_state |= MLX5VF_PCI_FREEZED;
	return 0;
}

static int mlx5vf_pci_save_device_data(struct mlx5vf_pci_core_device *mvdev)
{
	u32 state_size = 0;
	int ret;

	if (WARN_ON(!(mvdev->vmig.dev_state & MLX5VF_PCI_FREEZED)))
		return -EFAULT;

	/* If we already read state no reason to re-read */
	if (mvdev->vmig.vhca_state_data.state_size)
		return 0;

	ret = mlx5vf_cmd_query_vhca_migration_state(
		mvdev->core_device.pdev, mvdev->vmig.vhca_id, &state_size);
	if (ret)
		return ret;

	return mlx5vf_cmd_save_vhca_state(mvdev->core_device.pdev,
					  mvdev->vmig.vhca_id, state_size,
					  &mvdev->vmig.vhca_state_data);
}

static int mlx5vf_pci_new_write_window(struct mlx5vf_pci_core_device *mvdev)
{
	struct mlx5_vhca_state_data *state_data = &mvdev->vmig.vhca_state_data;
	u32 num_pages_needed;
	u64 allocated_ready;
	u32 bytes_needed;

	/* Check how many bytes are available from previous flows */
	WARN_ON(state_data->num_pages * PAGE_SIZE <
		state_data->win_start_offset);
	allocated_ready = (state_data->num_pages * PAGE_SIZE) -
			  state_data->win_start_offset;
	WARN_ON(allocated_ready > MLX5VF_MIG_REGION_DATA_SIZE);

	bytes_needed = MLX5VF_MIG_REGION_DATA_SIZE - allocated_ready;
	if (!bytes_needed)
		return 0;

	num_pages_needed = DIV_ROUND_UP_ULL(bytes_needed, PAGE_SIZE);
	return mlx5vf_add_migration_pages(state_data, num_pages_needed);
}

static ssize_t
mlx5vf_pci_handle_migration_data_size(struct mlx5vf_pci_core_device *mvdev,
				      char __user *buf, bool iswrite)
{
	struct mlx5vf_pci_migration_info *vmig = &mvdev->vmig;
	u64 data_size;
	int ret;

	if (iswrite) {
		/* data_size is writable only during resuming state */
		if (vmig->vfio_dev_fsm != VFIO_DEVICE_STATE_RESUMING)
			return -EINVAL;

		ret = copy_from_user(&data_size, buf, sizeof(data_size));
		if (ret)
			return -EFAULT;

		vmig->vhca_state_data.state_size += data_size;
		vmig->vhca_state_data.win_start_offset += data_size;
		ret = mlx5vf_pci_new_write_window(mvdev);
		if (ret)
			return ret;

	} else {
		if (vmig->vfio_dev_fsm != VFIO_DEVICE_STATE_STOP_COPY)
			return -EINVAL;

		data_size = min_t(u64, MLX5VF_MIG_REGION_DATA_SIZE,
				  vmig->vhca_state_data.state_size -
				  vmig->vhca_state_data.win_start_offset);
		ret = copy_to_user(buf, &data_size, sizeof(data_size));
		if (ret)
			return -EFAULT;
	}

	vmig->region_state |= MLX5VF_REGION_DATA_SIZE;
	return sizeof(data_size);
}

static ssize_t
mlx5vf_pci_handle_migration_data_offset(struct mlx5vf_pci_core_device *mvdev,
					char __user *buf, bool iswrite)
{
	static const u64 data_offset = MLX5VF_MIG_REGION_DATA_OFFSET;
	int ret;

	/* RO field */
	if (iswrite)
		return -EFAULT;

	ret = copy_to_user(buf, &data_offset, sizeof(data_offset));
	if (ret)
		return -EFAULT;

	return sizeof(data_offset);
}

static ssize_t
mlx5vf_pci_handle_migration_pending_bytes(struct mlx5vf_pci_core_device *mvdev,
					  char __user *buf, bool iswrite)
{
	struct mlx5vf_pci_migration_info *vmig = &mvdev->vmig;
	u64 pending_bytes;
	int ret;

	/* RO field */
	if (iswrite)
		return -EFAULT;

	if (vmig->vfio_dev_fsm == VFIO_DEVICE_STATE_PRE_COPY ||
	    vmig->vfio_dev_fsm == VFIO_DEVICE_STATE_PRE_COPY_P2P) {
		/*
		 * In pre-copy state we have no data to return for now,
		 * return 0 pending bytes
		 */
		pending_bytes = 0;
	} else {
		if (!vmig->vhca_state_data.state_size)
			return 0;
		pending_bytes = vmig->vhca_state_data.state_size -
				vmig->vhca_state_data.win_start_offset;
	}

	ret = copy_to_user(buf, &pending_bytes, sizeof(pending_bytes));
	if (ret)
		return -EFAULT;

	/* Window moves forward once data from previous iteration was read */
	if (vmig->region_state & MLX5VF_REGION_DATA_SIZE)
		vmig->vhca_state_data.win_start_offset +=
			min_t(u64, MLX5VF_MIG_REGION_DATA_SIZE, pending_bytes);

	WARN_ON(vmig->vhca_state_data.win_start_offset >
		vmig->vhca_state_data.state_size);

	/* New iteration started */
	vmig->region_state = MLX5VF_REGION_PENDING_BYTES;
	return sizeof(pending_bytes);
}

static int mlx5vf_load_state(struct mlx5vf_pci_core_device *mvdev)
{
	if (!mvdev->vmig.vhca_state_data.state_size)
		return 0;

	return mlx5vf_cmd_load_vhca_state(mvdev->core_device.pdev,
					  mvdev->vmig.vhca_id,
					  &mvdev->vmig.vhca_state_data);
}

static void mlx5vf_reset_mig_state(struct mlx5vf_pci_core_device *mvdev)
{
	struct mlx5vf_pci_migration_info *vmig = &mvdev->vmig;

	vmig->region_state = 0;
	mlx5vf_reset_vhca_state(&vmig->vhca_state_data);
}

static int mlx5vf_pci_setup_device_state(struct vfio_device *vdev, u32 new)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(
		vdev, struct mlx5vf_pci_core_device, core_device.vdev);
	u32 cur = mvdev->vmig.vfio_dev_fsm;
	int ret;

	if (cur == VFIO_DEVICE_STATE_RUNNING_P2P && new == VFIO_DEVICE_STATE_STOP)
		return mlx5vf_pci_freeze_device(mvdev);

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_RUNNING_P2P)
		return mlx5vf_pci_unfreeze_device(mvdev);

	if ((cur == VFIO_DEVICE_STATE_RUNNING && new == VFIO_DEVICE_STATE_RUNNING_P2P) ||
	    (cur == VFIO_DEVICE_STATE_PRE_COPY && new == VFIO_DEVICE_STATE_PRE_COPY_P2P))
		return mlx5vf_pci_quiesce_device(mvdev);

	if ((cur == VFIO_DEVICE_STATE_RUNNING_P2P && new == VFIO_DEVICE_STATE_RUNNING) ||
	    (cur == VFIO_DEVICE_STATE_PRE_COPY_P2P && new == VFIO_DEVICE_STATE_PRE_COPY))
		return mlx5vf_pci_unquiesce_device(mvdev);

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_STOP_COPY) {
		mlx5vf_reset_mig_state(mvdev);
		return mlx5vf_pci_save_device_data(mvdev);
	}

	if (cur == VFIO_DEVICE_STATE_PRE_COPY_P2P && new == VFIO_DEVICE_STATE_STOP_COPY) {
		ret = mlx5vf_pci_freeze_device(mvdev);
		if (ret)
			return ret;
		ret = mlx5vf_pci_save_device_data(mvdev);
		if (ret) {
			if (mlx5vf_pci_unfreeze_device(mvdev))
				mvdev->vmig.vfio_dev_fsm =
					VFIO_DEVICE_STATE_ERROR;
			return ret;
		}
		return 0;
	}

	if ((cur == VFIO_DEVICE_STATE_RUNNING && new == VFIO_DEVICE_STATE_PRE_COPY) ||
	    (cur == VFIO_DEVICE_STATE_RUNNING_P2P && new == VFIO_DEVICE_STATE_PRE_COPY_P2P) ||
	    (cur == VFIO_DEVICE_STATE_PRE_COPY && new == VFIO_DEVICE_STATE_RUNNING) ||
	    (cur == VFIO_DEVICE_STATE_PRE_COPY_P2P && new == VFIO_DEVICE_STATE_RUNNING_P2P) ||
	    (cur == VFIO_DEVICE_STATE_STOP_COPY && new == VFIO_DEVICE_STATE_STOP)) {
		mlx5vf_reset_mig_state(mvdev);
		return 0;
	}

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_RESUMING)
		return mlx5vf_pci_new_write_window(mvdev);

	if (cur == VFIO_DEVICE_STATE_RESUMING && new == VFIO_DEVICE_STATE_STOP) {
		ret = mlx5vf_load_state(mvdev);
		if (ret)
			return ret;
		mlx5vf_reset_mig_state(mvdev);
		return 0;
	}

	/* vfio_mig_set_device_state() does not use arcs other than the above */
	WARN_ON(true);
	return -EINVAL;
}

static ssize_t
mlx5vf_pci_handle_migration_device_state(struct mlx5vf_pci_core_device *mvdev,
					 char __user *buf, size_t count,
					 bool iswrite)
{
	u32 device_state;
	int ret;

	if (count != sizeof(device_state))
		return -EINVAL;

	if (iswrite) {
		ret = copy_from_user(&device_state, buf, sizeof(device_state));
		if (ret)
			return -EFAULT;

		ret = vfio_mig_set_device_state(&mvdev->core_device.vdev,
						device_state,
						&mvdev->vmig.vfio_dev_fsm);
		if (ret)
			return ret;
	} else {
		device_state = mvdev->vmig.vfio_dev_fsm;
		ret = copy_to_user(buf, &device_state, sizeof(device_state));
		if (ret)
			return -EFAULT;
	}

	return count;
}

static ssize_t
mlx5vf_pci_copy_user_data_to_device_state(struct mlx5vf_pci_core_device *mvdev,
					  char __user *buf, size_t count,
					  u64 offset)
{
	struct mlx5_vhca_state_data *state_data = &mvdev->vmig.vhca_state_data;
	char __user *from_buff = buf;
	u32 curr_offset;
	u32 win_page_offset;
	u32 copy_count;
	struct page *page;
	char *to_buff;
	int ret;

	curr_offset = state_data->win_start_offset + offset;

	do {
		page = mlx5vf_get_migration_page(&state_data->mig_data,
						 curr_offset);
		if (!page)
			return -EINVAL;

		win_page_offset = curr_offset % PAGE_SIZE;
		copy_count = min_t(u32, PAGE_SIZE - win_page_offset, count);

		to_buff = kmap_local_page(page);
		ret = copy_from_user(to_buff + win_page_offset, from_buff,
				     copy_count);
		kunmap_local(to_buff);
		if (ret)
			return -EFAULT;

		from_buff += copy_count;
		curr_offset += copy_count;
		count -= copy_count;
	} while (count > 0);

	return 0;
}

static ssize_t
mlx5vf_pci_copy_device_state_to_user(struct mlx5vf_pci_core_device *mvdev,
				     char __user *buf, u64 offset, size_t count)
{
	struct mlx5_vhca_state_data *state_data = &mvdev->vmig.vhca_state_data;
	char __user *to_buff = buf;
	u32 win_available_bytes;
	u32 win_page_offset;
	u32 copy_count;
	u32 curr_offset;
	char *from_buff;
	struct page *page;
	int ret;

	win_available_bytes =
		min_t(u64, MLX5VF_MIG_REGION_DATA_SIZE,
		      mvdev->vmig.vhca_state_data.state_size -
			      mvdev->vmig.vhca_state_data.win_start_offset);

	if (count + offset > win_available_bytes)
		return -EINVAL;

	curr_offset = state_data->win_start_offset + offset;

	do {
		page = mlx5vf_get_migration_page(&state_data->mig_data,
						 curr_offset);
		if (!page)
			return -EINVAL;

		win_page_offset = curr_offset % PAGE_SIZE;
		copy_count = min_t(u32, PAGE_SIZE - win_page_offset, count);

		from_buff = kmap_local_page(page);
		ret = copy_to_user(to_buff, from_buff + win_page_offset,
				   copy_count);
		kunmap_local(from_buff);
		if (ret)
			return -EFAULT;

		curr_offset += copy_count;
		count -= copy_count;
		to_buff += copy_count;
	} while (count);

	return 0;
}

static ssize_t
mlx5vf_pci_migration_data_rw(struct mlx5vf_pci_core_device *mvdev,
			     char __user *buf, size_t count, u64 offset,
			     bool iswrite)
{
	int ret;

	if (offset + count > MLX5VF_MIG_REGION_DATA_SIZE)
		return -EINVAL;

	if (iswrite)
		ret = mlx5vf_pci_copy_user_data_to_device_state(mvdev, buf,
								count, offset);
	else
		ret = mlx5vf_pci_copy_device_state_to_user(mvdev, buf, offset,
							   count);
	if (ret)
		return ret;
	return count;
}

static ssize_t mlx5vf_pci_mig_rw(struct vfio_pci_core_device *vdev,
				 char __user *buf, size_t count, loff_t *ppos,
				 bool iswrite)
{
	struct mlx5vf_pci_core_device *mvdev =
		container_of(vdev, struct mlx5vf_pci_core_device, core_device);
	u64 pos = *ppos & VFIO_PCI_OFFSET_MASK;
	int ret;

	mutex_lock(&mvdev->state_mutex);
	/* Copy to/from the migration region data section */
	if (pos >= MLX5VF_MIG_REGION_DATA_OFFSET) {
		ret = mlx5vf_pci_migration_data_rw(
			mvdev, buf, count, pos - MLX5VF_MIG_REGION_DATA_OFFSET,
			iswrite);
		goto end;
	}

	switch (pos) {
	case VFIO_DEVICE_MIGRATION_OFFSET(device_state):
		ret = mlx5vf_pci_handle_migration_device_state(mvdev, buf,
							       count, iswrite);
		break;
	case VFIO_DEVICE_MIGRATION_OFFSET(pending_bytes):
		/*
		 * The number of pending bytes still to be migrated from the
		 * vendor driver. This is RO field.
		 * Reading this field indicates on the start of a new iteration
		 * to get device data.
		 */
		ret = mlx5vf_pci_handle_migration_pending_bytes(mvdev, buf,
								iswrite);
		break;
	case VFIO_DEVICE_MIGRATION_OFFSET(data_offset):
		/*
		 * The user application should read data_offset field from the
		 * migration region. The user application should read the
		 * device data from this offset within the migration region
		 * during the _SAVING mode or write the device data during the
		 * _RESUMING mode. This is RO field.
		 */
		ret = mlx5vf_pci_handle_migration_data_offset(mvdev, buf,
							      iswrite);
		break;
	case VFIO_DEVICE_MIGRATION_OFFSET(data_size):
		/*
		 * The user application should read data_size to get the size
		 * in bytes of the data copied to the migration region during
		 * the _SAVING state by the device. The user application should
		 * write the size in bytes of the data that was copied to
		 * the migration region during the _RESUMING state by the user.
		 * This is RW field.
		 */
		ret = mlx5vf_pci_handle_migration_data_size(mvdev, buf,
							    iswrite);
		break;
	default:
		ret = -EFAULT;
		break;
	}

end:
	mutex_unlock(&mvdev->state_mutex);
	return ret;
}

static struct vfio_pci_regops migration_ops = {
	.rw = mlx5vf_pci_mig_rw,
};

static int mlx5vf_pci_open_device(struct vfio_device *core_vdev)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(
		core_vdev, struct mlx5vf_pci_core_device, core_device.vdev);
	struct vfio_pci_core_device *vdev = &mvdev->core_device;
	int vf_id;
	int ret;

	ret = vfio_pci_core_enable(vdev);
	if (ret)
		return ret;

	if (!mvdev->migrate_cap) {
		vfio_pci_core_finish_enable(vdev);
		return 0;
	}

	vf_id = pci_iov_vf_id(vdev->pdev);
	if (vf_id < 0) {
		ret = vf_id;
		goto out_disable;
	}

	ret = mlx5vf_cmd_get_vhca_id(vdev->pdev, vf_id + 1,
				     &mvdev->vmig.vhca_id);
	if (ret)
		goto out_disable;

	ret = vfio_pci_register_dev_region(vdev, VFIO_REGION_TYPE_MIGRATION,
					   VFIO_REGION_SUBTYPE_MIGRATION,
					   &migration_ops,
					   MLX5VF_MIG_REGION_DATA_OFFSET +
					   MLX5VF_MIG_REGION_DATA_SIZE,
					   VFIO_REGION_INFO_FLAG_READ |
					   VFIO_REGION_INFO_FLAG_WRITE,
					   NULL);
	if (ret)
		goto out_disable;

	mvdev->vmig.vfio_dev_fsm = VFIO_DEVICE_STATE_RUNNING;
	vfio_pci_core_finish_enable(vdev);
	return 0;
out_disable:
	vfio_pci_core_disable(vdev);
	return ret;
}

static void mlx5vf_pci_close_device(struct vfio_device *core_vdev)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(
		core_vdev, struct mlx5vf_pci_core_device, core_device.vdev);

	vfio_pci_core_close_device(core_vdev);
	mlx5vf_reset_mig_state(mvdev);
}

static const struct vfio_device_ops mlx5vf_pci_ops = {
	.name = "mlx5-vfio-pci",
	.open_device = mlx5vf_pci_open_device,
	.close_device = mlx5vf_pci_close_device,
	.ioctl = vfio_pci_core_ioctl,
	.read = vfio_pci_core_read,
	.write = vfio_pci_core_write,
	.mmap = vfio_pci_core_mmap,
	.request = vfio_pci_core_request,
	.match = vfio_pci_core_match,
	.migration_step_device_state = mlx5vf_pci_setup_device_state,
};

static int mlx5vf_pci_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	struct mlx5vf_pci_core_device *mvdev;
	int ret;

	mvdev = kzalloc(sizeof(*mvdev), GFP_KERNEL);
	if (!mvdev)
		return -ENOMEM;
	vfio_pci_core_init_device(&mvdev->core_device, pdev, &mlx5vf_pci_ops);

	if (pdev->is_virtfn) {
		struct mlx5_core_dev *mdev =
			mlx5_vf_get_core_dev(pdev);

		if (mdev) {
			if (MLX5_CAP_GEN(mdev, migration)) {
				mvdev->migrate_cap = 1;
				mutex_init(&mvdev->state_mutex);
			}
			mlx5_vf_put_core_dev(mdev);
		}
	}

	ret = vfio_pci_core_register_device(&mvdev->core_device);
	if (ret)
		goto out_free;

	dev_set_drvdata(&pdev->dev, mvdev);
	return 0;

out_free:
	vfio_pci_core_uninit_device(&mvdev->core_device);
	kfree(mvdev);
	return ret;
}

static void mlx5vf_pci_remove(struct pci_dev *pdev)
{
	struct mlx5vf_pci_core_device *mvdev = dev_get_drvdata(&pdev->dev);

	vfio_pci_core_unregister_device(&mvdev->core_device);
	vfio_pci_core_uninit_device(&mvdev->core_device);
	kfree(mvdev);
}

static const struct pci_device_id mlx5vf_pci_table[] = {
	{ PCI_DRIVER_OVERRIDE_DEVICE_VFIO(PCI_VENDOR_ID_MELLANOX, 0x101e) }, /* ConnectX Family mlx5Gen Virtual Function */
	{}
};

MODULE_DEVICE_TABLE(pci, mlx5vf_pci_table);

static struct pci_driver mlx5vf_pci_driver = {
	.name = KBUILD_MODNAME,
	.id_table = mlx5vf_pci_table,
	.probe = mlx5vf_pci_probe,
	.remove = mlx5vf_pci_remove,
	.err_handler = &vfio_pci_core_err_handlers,
};

static void __exit mlx5vf_pci_cleanup(void)
{
	pci_unregister_driver(&mlx5vf_pci_driver);
}

static int __init mlx5vf_pci_init(void)
{
	return pci_register_driver(&mlx5vf_pci_driver);
}

module_init(mlx5vf_pci_init);
module_exit(mlx5vf_pci_cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Max Gurtovoy <mgurtovoy@nvidia.com>");
MODULE_AUTHOR("Yishai Hadas <yishaih@nvidia.com>");
MODULE_DESCRIPTION(
	"MLX5 VFIO PCI - User Level meta-driver for MLX5 device family");
