/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
#include "kunit_iommu.h"
#include "pt_iter.h"
#include <linux/iommu.h>
#include <linux/io-pgtable.h>

#ifndef PT_KUNIT_IO_PGTBL_DYNAMIC_TOP
#define PT_KUNIT_IO_PGTBL_DYNAMIC_TOP 0
#endif

struct kunit_iommu_cmp_priv {
	/* Generic PT version */
	struct kunit_iommu_priv fmt;

	/* IO pagetable version */
	struct io_pgtable_ops *pgtbl_ops;
	struct io_pgtable_cfg *fmt_memory;
	struct pt_iommu_table ref_table;
};

struct compare_tables {
	struct kunit *test;
	struct pt_range ref_range;
	struct pt_table_p *ref_table;
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

	for_each_pt_level_item(&pts) {
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
	const struct pt_iommu_ops *ops = priv->iommu->ops;
	size_t mapped;
	int ret;

	/* This lacks pagination, must call with perfectly aligned everything */
	if (sizeof(unsigned long) == 8) {
		KUNIT_ASSERT_EQ(test, va % len, 0);
		KUNIT_ASSERT_EQ(test, pa % len, 0);
	}

	mapped = 0;
	ret = ops->map_range(priv->iommu, va, pa, len, prot, GFP_KERNEL,
			     &mapped, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, mapped, len);

	mapped = 0;
	ret = cmp_priv->pgtbl_ops->map_pages(cmp_priv->pgtbl_ops, va, pa, len,
					     1, prot, GFP_KERNEL, &mapped);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, mapped, len);
}

static void do_cmp_unmap(struct kunit *test, pt_vaddr_t va, pt_vaddr_t len)
{
	struct kunit_iommu_cmp_priv *cmp_priv = test->priv;
	struct kunit_iommu_priv *priv = &cmp_priv->fmt;
	const struct pt_iommu_ops *ops = priv->iommu->ops;
	size_t ret;

	if (sizeof(unsigned long) != 4)
		KUNIT_ASSERT_EQ(test, va % len, 0);

	ret = ops->unmap_range(priv->iommu, va, len, NULL);
	KUNIT_ASSERT_EQ(test, ret, len);
	ret = cmp_priv->pgtbl_ops->unmap_pages(cmp_priv->pgtbl_ops, va, len, 1,
					       NULL);
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
	unsigned int max_vasz_lg2;
	pt_vaddr_t last;

	if (PT_KUNIT_IO_PGTBL_DYNAMIC_TOP)
		max_vasz_lg2 = priv->common->max_vasz_lg2;
	else
		max_vasz_lg2 = pt_top_range(priv->common).max_vasz_lg2;

	last = fvalog2_set_mod_max(pt_full_va_prefix(priv->common),
				   max_vasz_lg2);
	/*
	 * Map the very end of the page VA space. This triggers increase on
	 * AMDv1. io_pgtable_ops uses an unsigned long for the va instead
	 * of dma_addr_t, so it truncates when it shouldn't.
	 */
	if (sizeof(unsigned long) == 4 && last >= U32_MAX)
		last = (u32)last;
	do_cmp_map(test, last - (priv->smallest_pgsz - 1), 0,
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
	struct io_pgtable_ops *iopt_ops = cmp_priv->pgtbl_ops;
	struct kunit_iommu_priv *priv = &cmp_priv->fmt;
	const struct pt_iommu_ops *ops = priv->iommu->ops;
	struct io_pgtable_cfg *pgtbl_cfg =
		&io_pgtable_ops_to_pgtable(iopt_ops)->cfg;
	struct pt_range top_range = pt_top_range(priv->common);
	pt_vaddr_t pgsize_bitmap = priv->safe_pgsize_bitmap &
				   pgtbl_cfg->pgsize_bitmap;
	unsigned int pgsz_lg2;

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

			genpt_unmapped = ops->unmap_range(priv->iommu, vaddr,
							  base_len, NULL);

			iopt_unmapped = iopt_ops->unmap_pages(
				cmp_priv->pgtbl_ops, vaddr, base_len, 1, NULL);
			compare_tables(test);

			KUNIT_ASSERT_EQ(test, genpt_unmapped, iopt_unmapped);
			KUNIT_ASSERT_EQ(test, genpt_unmapped, next_len);
		}
	}
}

static int pt_kunit_iommu_cmp_init(struct kunit *test)
{
	struct kunit_iommu_cmp_priv *cmp_priv;
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
