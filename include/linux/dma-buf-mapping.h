/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DMA BUF Mapping Helpers
 *
 * The mapping type is negotiated between importer and exporter and defines
 * what information is exchanged during the attachment.
 *
 */
#ifndef __DMA_BUF_MAPPING_H__
#define __DMA_BUF_MAPPING_H__
#include <linux/dma-buf.h>

struct device;
struct dma_buf;
struct dma_buf_attachment;
struct dma_buf_mapping_exp_ops;

struct dma_buf_mapping_type {
	const char *name;

	/* 0 is succeess
	 * -EOPNOTSUPP means ignore the failure and continue
	 * Everything else aborts the search and fails.
	 */
	int (*match)(struct dma_buf *dmabuf,
		     const struct dma_buf_mapping_match *exp,
		     const struct dma_buf_mapping_match *imp);
};

struct dma_buf_mapping_exp_ops {
	int (*attach)(struct dma_buf *dmabuf,
		      struct dma_buf_attachment *attach);
	void (*detach)(struct dma_buf *dmabuf,
		       struct dma_buf_attachment *attach);
};

struct dma_buf_match_args {
	struct dma_buf *dmabuf;
	const struct dma_buf_mapping_exp_ops *exp_ops;
	const struct dma_buf_mapping_match *imp_mappings;
	size_t imp_len;
	size_t imp_match_idx;
};

struct sg_table *dma_buf_phys_vec_to_sgt(struct dma_buf_attachment *attach,
					 struct p2pdma_provider *provider,
					 struct dma_buf_phys_vec *phys_vec,
					 size_t nr_ranges, size_t size,
					 enum dma_data_direction dir);
void dma_buf_free_sgt(struct dma_buf_attachment *attach, struct sg_table *sgt,
		      enum dma_data_direction dir);

int dma_buf_match_mapping(struct dma_buf_match_args *args,
			  const struct dma_buf_mapping_match *exp_mappings,
			  size_t exp_len);

#endif
