/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
#include "kunit_iommu.h"
#include "pt_iter.h"
#include <linux/iommu.h>
#include <linux/io-pgtable.h>

#ifndef __GENERIC_PT_KUNIT_IOMMU_CMP_H
#define __GENERIC_PT_KUNIT_IOMMU_CMP_H

#ifndef PT_KUNIT_IO_PGTBL_DYNAMIC_TOP
#define PT_KUNIT_IO_PGTBL_DYNAMIC_TOP 0
#endif

#ifndef pt_iommu_free_pgtbl_cfg
static void pt_iommu_free_pgtbl_cfg(struct io_pgtable_cfg *pgtbl_cfg)
{
}
#endif

struct kunit_iommu_cmp_priv {
	/* Generic PT version */
	struct kunit_iommu_priv fmt;

	/* IO pagetable version */
	struct iommu_domain pgtbl_domain;
	struct io_pgtable_ops *pgtbl_ops;
	struct io_pgtable_cfg *fmt_memory;
	struct pt_iommu_table ref_table;
};

struct compare_tables {
	struct kunit *test;
	struct pt_range ref_range;
	struct pt_table_p *ref_table;
};

static int kunit_iommu_cmp_map_pages(struct iommu_domain *domain,
				     unsigned long iova, phys_addr_t paddr,
				     size_t pgsize, size_t pgcount, int prot,
				     gfp_t gfp, size_t *mapped)
{
	struct kunit_iommu_cmp_priv *priv =
		container_of(domain, struct kunit_iommu_cmp_priv, pgtbl_domain);
	struct io_pgtable_ops *ops = priv->pgtbl_ops;

	return ops->map_pages(ops, iova, paddr, pgsize, pgcount, prot, gfp,
			      mapped);
}

static size_t kunit_iommu_cmp_unmap_pages(struct iommu_domain *domain,
					  unsigned long iova, size_t pgsize,
					  size_t pgcount,
					  struct iommu_iotlb_gather *gather)
{
	struct kunit_iommu_cmp_priv *priv =
		container_of(domain, struct kunit_iommu_cmp_priv, pgtbl_domain);
	struct io_pgtable_ops *ops = priv->pgtbl_ops;

	return ops->unmap_pages(ops, iova, pgsize, pgcount, gather);
}

static phys_addr_t kunit_iommu_cmp_iova_to_phys(struct iommu_domain *domain,
						dma_addr_t iova)
{
	struct kunit_iommu_cmp_priv *priv =
		container_of(domain, struct kunit_iommu_cmp_priv, pgtbl_domain);
	struct io_pgtable_ops *ops = priv->pgtbl_ops;

	return ops->iova_to_phys(ops, iova);
}

/*
 * Hook things up so we can call the ops through the normal iommu_domain
 * functions.
 */
static const struct iommu_domain_ops pgtbl_ops = {
	.map_pages = &kunit_iommu_cmp_map_pages,
	.unmap_pages = &kunit_iommu_cmp_unmap_pages,
	.iova_to_phys = &kunit_iommu_cmp_iova_to_phys,
	.iotlb_sync = &pt_kunit_iotlb_sync,
};

static int __compare_tables(struct pt_range *range, void *arg,
			    unsigned int level, struct pt_table_p *table)
{
	struct pt_state pts = pt_init(range, level, table);
	struct compare_tables *cmp = arg;
	struct pt_state ref_pts =
		pt_init(&cmp->ref_range, level, cmp->ref_table);
	struct kunit *test = cmp->test;
	int ret;

	for_each_pt_level_entry(&pts) {
		u64 entry, ref_entry;

		cmp->ref_range.va = range->va;
		ref_pts.index = pts.index;
		pt_load_entry(&ref_pts);

		entry = pt_kunit_cmp_mask_entry(&pts);
		ref_entry = pt_kunit_cmp_mask_entry(&ref_pts);

		/*if (entry != 0 || ref_entry != 0)
			printk("Check %llx Level %u index %u ptr %px refptr %px: %llx (%llx) %llx (%llx)\n",
			       pts.range->va, pts.level, pts.index,
			       pts.table,
			       ref_pts.table,
			       pts.entry, entry,
			       ref_pts.entry, ref_entry);*/

		KUNIT_ASSERT_EQ(test, pts.type, ref_pts.type);
		KUNIT_ASSERT_EQ(test, entry, ref_entry);
		if (entry != ref_entry)
			return 0;

		if (pts.type == PT_ENTRY_TABLE) {
			cmp->ref_table = ref_pts.table_lower;
			ret = pt_descend(&pts, arg, __compare_tables);
			if (ret)
				return ret;
		}

		/* Defeat contiguous entry aggregation */
		pts.type = PT_ENTRY_EMPTY;
	}

	return 0;
}

static void compare_tables(struct kunit *test)
{
	struct kunit_iommu_cmp_priv *cmp_priv = test->priv;
	struct kunit_iommu_priv *priv = &cmp_priv->fmt;
	struct pt_range range = pt_top_range(priv->common);
	struct compare_tables cmp = {
		.test = test,
	};
	struct pt_state pts = pt_init_top(&range);
	struct pt_state ref_pts;

	pt_iommu_setup_ref_table(&cmp_priv->ref_table, cmp_priv->pgtbl_ops);
	cmp.ref_range =
		pt_top_range(common_from_iommu(&cmp_priv->ref_table.iommu));
	ref_pts = pt_init_top(&cmp.ref_range);
	KUNIT_ASSERT_EQ(test, pts.level, ref_pts.level);

	cmp.ref_table = ref_pts.table;
	KUNIT_ASSERT_EQ(test, pt_walk_range(&range, __compare_tables, &cmp), 0);
}

static void test_cmp_init(struct kunit *test)
{
	struct kunit_iommu_cmp_priv *cmp_priv = test->priv;
	struct kunit_iommu_priv *priv = &cmp_priv->fmt;
	struct io_pgtable_cfg *pgtbl_cfg =
		&io_pgtable_ops_to_pgtable(cmp_priv->pgtbl_ops)->cfg;

	/* Fixture does the setup */
	KUNIT_ASSERT_NE(test, priv->info.pgsize_bitmap, 0);

	/* pt_iommu has a superset of page sizes (ARM supports contiguous) */
	KUNIT_ASSERT_EQ(test,
			priv->info.pgsize_bitmap & pgtbl_cfg->pgsize_bitmap,
			pgtbl_cfg->pgsize_bitmap);

	/* Empty compare works */
	compare_tables(test);
}

static void do_cmp_map(struct kunit *test, pt_vaddr_t va, pt_oaddr_t pa,
		       pt_oaddr_t len, unsigned int prot)
{
	struct kunit_iommu_cmp_priv *cmp_priv = test->priv;
	struct kunit_iommu_priv *priv = &cmp_priv->fmt;
	int ret;

	/* This lacks pagination, must call with perfectly aligned everything */
	if (sizeof(unsigned long) == 8) {
		KUNIT_ASSERT_EQ(test, va % len, 0);
		KUNIT_ASSERT_EQ(test, pa % len, 0);
	}

	ret = iommu_map(&priv->domain, va, pa, len, prot, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = iommu_map(&cmp_priv->pgtbl_domain, va, pa, len, prot, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

static void do_cmp_unmap(struct kunit *test, pt_vaddr_t va, pt_vaddr_t len)
{
	struct kunit_iommu_cmp_priv *cmp_priv = test->priv;
	struct kunit_iommu_priv *priv = &cmp_priv->fmt;
	size_t ret;

	if (sizeof(unsigned long) != 4)
		KUNIT_ASSERT_EQ(test, va % len, 0);

	ret = iommu_unmap(&priv->domain, va, len);
	KUNIT_ASSERT_EQ(test, ret, len);

	ret = iommu_unmap(&cmp_priv->pgtbl_domain, va, len);
	KUNIT_ASSERT_EQ(test, ret, len);
}

static void test_cmp_one_map(struct kunit *test)
{
	struct kunit_iommu_cmp_priv *cmp_priv = test->priv;
	struct kunit_iommu_priv *priv = &cmp_priv->fmt;
	struct pt_range range = pt_top_range(priv->common);
	struct io_pgtable_cfg *pgtbl_cfg =
		&io_pgtable_ops_to_pgtable(cmp_priv->pgtbl_ops)->cfg;
	const pt_oaddr_t addr =
		oalog2_mod(0x74a71445deadbeef, priv->common->max_oasz_lg2);
	pt_vaddr_t pgsize_bitmap = priv->safe_pgsize_bitmap &
				   pgtbl_cfg->pgsize_bitmap;
	pt_vaddr_t cur_va;
	unsigned int prot = 0;
	unsigned int pgsz_lg2;

	/*
	 * Check that every prot combination at every page size level generates
	 * the same data in page table.
	 */
	for (prot = 0; prot <= (IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE |
				IOMMU_NOEXEC | IOMMU_MMIO);
	     prot++) {
		/* Page tables usually cannot represent inaccessible memory */
		if (!(prot & (IOMMU_READ | IOMMU_WRITE)))
			continue;

		/* Try every supported page size */
		cur_va = range.va + priv->smallest_pgsz * 256;
		for (pgsz_lg2 = 0; pgsz_lg2 != PT_VADDR_MAX_LG2; pgsz_lg2++) {
			pt_vaddr_t len = log2_to_int(pgsz_lg2);

			if (!(pgsize_bitmap & len))
				continue;

			cur_va = ALIGN(cur_va, len);
			do_cmp_map(test, cur_va,
				   oalog2_set_mod(addr, 0, pgsz_lg2), len,
				   prot);
			compare_tables(test);
			cur_va += len;
		}

		cur_va = range.va + priv->smallest_pgsz * 256;
		for (pgsz_lg2 = 0; pgsz_lg2 != PT_VADDR_MAX_LG2; pgsz_lg2++) {
			pt_vaddr_t len = log2_to_int(pgsz_lg2);

			if (!(pgsize_bitmap & len))
				continue;

			cur_va = ALIGN(cur_va, len);
			do_cmp_unmap(test, cur_va, len);
			compare_tables(test);
			cur_va += len;
		}
	}
}

static void test_cmp_high_va(struct kunit *test)
{
	struct kunit_iommu_cmp_priv *cmp_priv = test->priv;
	struct kunit_iommu_priv *priv = &cmp_priv->fmt;
	struct pt_range top_range = {};

	if (PT_KUNIT_IO_PGTBL_DYNAMIC_TOP) {
		top_range.max_vasz_lg2 = priv->common->max_vasz_lg2;
		top_range.last_va =
			fvalog2_set_mod_max(pt_full_va_prefix(priv->common),
					    top_range.max_vasz_lg2);
	} else {
		top_range = pt_upper_range(priv->common);
		if (!IS_ENABLED(PT_KUNIT_CMP_SIGN_EXTEND) ||
		    (sizeof(unsigned long) == 4 && top_range.va > U32_MAX))
			top_range = pt_top_range(priv->common);
	}

	/*
	 * Map the very end of the page VA space. This triggers increase on
	 * AMDv1. io_pgtable_ops uses an unsigned long for the va instead
	 * of dma_addr_t, so it truncates when it shouldn't.
	 */
	if (sizeof(unsigned long) == 4 && top_range.last_va >= U32_MAX) {
		if (top_range.va > U32_MAX) {
			kunit_skip(test, "No 32 bit IOVA available");
			return;
		}
		top_range.last_va  = top_range.last_va;
	}

	do_cmp_map(test, top_range.last_va - (priv->smallest_pgsz - 1), 0,
		   priv->smallest_pgsz, IOMMU_READ | IOMMU_WRITE);
	compare_tables(test);
}

/*
 * Check what happens when a large page is split. iopt always unmaps the full
 * page. Test every pairing of mapping a large page and unmapping the start
 * using every smaller page size.
 */
static void test_cmp_unmap_split(struct kunit *test)
{
	struct kunit_iommu_cmp_priv *cmp_priv = test->priv;
	struct kunit_iommu_priv *priv = &cmp_priv->fmt;
	struct io_pgtable_cfg *pgtbl_cfg =
		&io_pgtable_ops_to_pgtable(cmp_priv->pgtbl_ops)->cfg;
	struct pt_range top_range = pt_top_range(priv->common);
	pt_vaddr_t pgsize_bitmap = priv->safe_pgsize_bitmap &
				   pgtbl_cfg->pgsize_bitmap;
	unsigned int pgsz_lg2;
	unsigned int count = 0;

	if (IS_ENABLED(PT_KUNIT_UNMAP_EXACT))
		kunit_skip(test, "Old implementation requires exact unmap, can't test");

	for (pgsz_lg2 = 0; pgsz_lg2 != PT_VADDR_MAX_LG2; pgsz_lg2++) {
		pt_vaddr_t base_len = log2_to_int(pgsz_lg2);
		unsigned int next_pgsz_lg2;

		if (!(pgsize_bitmap & base_len))
			continue;

		for (next_pgsz_lg2 = pgsz_lg2 + 1;
		     next_pgsz_lg2 != PT_VADDR_MAX_LG2; next_pgsz_lg2++) {
			pt_vaddr_t next_len = log2_to_int(next_pgsz_lg2);
			pt_vaddr_t vaddr = top_range.va;
			pt_oaddr_t paddr = 0;
			size_t genpt_unmapped;
			size_t iopt_unmapped;

			if (!(pgsize_bitmap & next_len))
				continue;

			do_cmp_map(test, vaddr, paddr, next_len,
				   IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE);
			compare_tables(test);

			genpt_unmapped = iommu_unmap(&priv->domain,
						    vaddr, base_len);
			iopt_unmapped = iommu_unmap(&cmp_priv->pgtbl_domain,
						    vaddr, base_len);
			compare_tables(test);

			KUNIT_ASSERT_EQ(test, genpt_unmapped, iopt_unmapped);
			KUNIT_ASSERT_EQ(test, genpt_unmapped, next_len);

			count++;
		}
	}

	if (count == 0)
		kunit_skip(
			test,
			"Test needs two page sizes active=%llx safe=%llx ref=%llx",
			(u64)pgsize_bitmap, (u64)priv->safe_pgsize_bitmap,
			(u64)pgtbl_cfg->pgsize_bitmap);
}

static int pt_kunit_iommu_cmp_init(struct kunit *test)
{
	struct kunit_iommu_cmp_priv *cmp_priv;
	struct io_pgtable_cfg *pgtbl_cfg;
	struct kunit_iommu_priv *priv;
	int ret;

	test->priv = cmp_priv = kzalloc(sizeof(*cmp_priv), GFP_KERNEL);
	if (!cmp_priv)
		return -ENOMEM;
	priv = &cmp_priv->fmt;

	ret = pt_kunit_priv_init(test, priv);
	if (ret)
		goto err_priv;

	/* io-pgtable uses unsigned long for passing the IOVA, not dma_addr_t */
	if (pt_top_range(priv->common).va >= ULONG_MAX) {
		kunit_skip(test,
			   "This configuration cannot be tested on 32 bit");
		return -EOPNOTSUPP;
	}

	cmp_priv->pgtbl_ops = pt_iommu_alloc_io_pgtable(
		&priv->cfg, priv->dummy_dev, &cmp_priv->fmt_memory);
	if (cmp_priv->pgtbl_ops == ERR_PTR(-EOPNOTSUPP)) {
		cmp_priv->pgtbl_ops = NULL;
		kunit_skip(test,
			   "io-pgtable does not support this configuration");
		return -EOPNOTSUPP;
	}
	if (!cmp_priv->pgtbl_ops) {
		ret = -ENOMEM;
		goto err_fmt_table;
	}

	pgtbl_cfg = &io_pgtable_ops_to_pgtable(cmp_priv->pgtbl_ops)->cfg;

	cmp_priv->pgtbl_domain.type = __IOMMU_DOMAIN_PAGING;
	cmp_priv->pgtbl_domain.ops = &pgtbl_ops;
	cmp_priv->pgtbl_domain.pgsize_bitmap = pgtbl_cfg->pgsize_bitmap;
	cmp_priv->pgtbl_domain.geometry = priv->domain.geometry;
	KUNIT_ASSERT_EQ(test,
			(priv->domain.pgsize_bitmap &
			 cmp_priv->pgtbl_domain.pgsize_bitmap),
			cmp_priv->pgtbl_domain.pgsize_bitmap);

	cmp_priv->ref_table = priv->fmt_table;
	return 0;

err_fmt_table:
	pt_iommu_deinit(priv->iommu);
err_priv:
	kfree(test->priv);
	test->priv = NULL;
	return ret;
}

static void pt_kunit_iommu_cmp_exit(struct kunit *test)
{
	struct kunit_iommu_cmp_priv *cmp_priv = test->priv;
	struct kunit_iommu_priv *priv = &cmp_priv->fmt;

	if (!test->priv)
		return;

	if (cmp_priv->pgtbl_ops) {
		free_io_pgtable_ops(cmp_priv->pgtbl_ops);
		pt_iommu_free_pgtbl_cfg(cmp_priv->fmt_memory);
	}
	pt_iommu_deinit(priv->iommu);
	kfree(test->priv);
}

static struct kunit_case cmp_test_cases[] = {
	KUNIT_CASE_FMT(test_cmp_init),
	KUNIT_CASE_FMT(test_cmp_one_map),
	KUNIT_CASE_FMT(test_cmp_high_va),
	KUNIT_CASE_FMT(test_cmp_unmap_split),
	{},
};

static struct kunit_suite NS(cmp_suite) = {
	.name = __stringify(NS(iommu_cmp_test)),
	.init = pt_kunit_iommu_cmp_init,
	.exit = pt_kunit_iommu_cmp_exit,
	.test_cases = cmp_test_cases,
};
kunit_test_suites(&NS(cmp_suite));

#endif
