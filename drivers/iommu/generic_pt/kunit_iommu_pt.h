/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
#include "kunit_iommu.h"
#include "pt_iter.h"
#include <linux/generic_pt/iommu.h>
#include <linux/iommu.h>

static unsigned int next_smallest_pgsz_lg2(struct kunit_iommu_priv *priv,
					   unsigned int pgsz_lg2)
{
	WARN_ON(!(priv->info.pgsize_bitmap & log2_to_int(pgsz_lg2)));
	pgsz_lg2--;
	for (; pgsz_lg2 > 0; pgsz_lg2--) {
		if (priv->info.pgsize_bitmap & log2_to_int(pgsz_lg2))
			return pgsz_lg2;
	}
	WARN_ON(true);
	return priv->smallest_pgsz_lg2;
}

struct count_valids {
	u64 per_size[PT_VADDR_MAX_LG2];
};

static int __count_valids(struct pt_range *range, void *arg, unsigned int level,
			  struct pt_table_p *table)
{
	struct pt_state pts = pt_init(range, level, table);
	struct count_valids *valids = arg;

	for_each_pt_level_item(&pts) {
		if (pts.type == PT_ENTRY_TABLE) {
			pt_descend(&pts, arg, __count_valids);
			continue;
		}
		if (pts.type == PT_ENTRY_OA) {
			valids->per_size[pt_entry_oa_lg2sz(&pts)]++;
			continue;
		}
	}
	return 0;
}

/*
 * Number of valid table entries. This counts contiguous entries as a single
 * valid.
 */
static unsigned int count_valids(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;
	struct pt_range range = pt_top_range(priv->common);
	struct count_valids valids = {};
	u64 total = 0;
	unsigned int i;

	KUNIT_ASSERT_NO_ERRNO(test,
			      pt_walk_range(&range, __count_valids, &valids));

	for (i = 0; i != ARRAY_SIZE(valids.per_size); i++)
		total += valids.per_size[i];
	return total;
}

/* Only a single page size is present, count the number of valid entries */
static unsigned int count_valids_single(struct kunit *test, pt_vaddr_t pgsz)
{
	struct kunit_iommu_priv *priv = test->priv;
	struct pt_range range = pt_top_range(priv->common);
	struct count_valids valids = {};
	u64 total = 0;
	unsigned int i;

	KUNIT_ASSERT_NO_ERRNO(test,
			      pt_walk_range(&range, __count_valids, &valids));

	for (i = 0; i != ARRAY_SIZE(valids.per_size); i++) {
		if ((1ULL << i) == pgsz)
			total = valids.per_size[i];
		else
			KUNIT_ASSERT_EQ(test, valids.per_size[i], 0);
	}
	return total;
}

static void do_map(struct kunit *test, pt_vaddr_t va, pt_oaddr_t pa,
		   pt_vaddr_t len)
{
	struct kunit_iommu_priv *priv = test->priv;
	const struct pt_iommu_ops *ops = priv->iommu->ops;
	size_t mapped;
	int ret;

	KUNIT_ASSERT_EQ(test, len, (size_t)len);

	/* Mapped accumulates */
	mapped = 1;
	ret = ops->map_range(priv->iommu, va, pa, len, IOMMU_READ | IOMMU_WRITE,
			     GFP_KERNEL, &mapped, NULL);
	KUNIT_ASSERT_NO_ERRNO_FN(test, "map_pages", ret);
	KUNIT_ASSERT_EQ(test, mapped, len + 1);
}

static void do_unmap(struct kunit *test, pt_vaddr_t va, pt_vaddr_t len)
{
	struct kunit_iommu_priv *priv = test->priv;
	const struct pt_iommu_ops *ops = priv->iommu->ops;
	size_t ret;

	ret = ops->unmap_range(priv->iommu, va, len, NULL);
	KUNIT_ASSERT_EQ(test, ret, len);
}

static void do_cut(struct kunit *test, pt_vaddr_t va)
{
	struct kunit_iommu_priv *priv = test->priv;
	const struct pt_iommu_ops *ops = priv->iommu->ops;
	size_t ret;

	ret = ops->cut_mapping(priv->iommu, va, GFP_KERNEL);
	if (ret == -EOPNOTSUPP)
		kunit_skip(
			test,
			"ops->cut_mapping not supported (enable CONFIG_DEBUG_GENERIC_PT)");
	KUNIT_ASSERT_NO_ERRNO_FN(test, "ops->cut_mapping", ret);
}

static void check_iova(struct kunit *test, pt_vaddr_t va, pt_oaddr_t pa,
		       pt_vaddr_t len)
{
	struct kunit_iommu_priv *priv = test->priv;
	const struct pt_iommu_ops *ops = priv->iommu->ops;
	pt_vaddr_t pfn = log2_div(va, priv->smallest_pgsz_lg2);
	pt_vaddr_t end_pfn = pfn + log2_div(len, priv->smallest_pgsz_lg2);

	for (; pfn != end_pfn; pfn++) {
		phys_addr_t res = ops->iova_to_phys(priv->iommu,
						    pfn * priv->smallest_pgsz);

		KUNIT_ASSERT_EQ(test, res, (phys_addr_t)pa);
		if (res != pa)
			break;
		pa += priv->smallest_pgsz;
	}
}

static void test_increase_level(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;
	struct pt_common *common = priv->common;

	if (!pt_feature(common, PT_FEAT_DYNAMIC_TOP))
		kunit_skip(test, "PT_FEAT_DYNAMIC_TOP not set for this format");

	if (IS_32BIT)
		kunit_skip(test, "Unable to test on 32bit");

	KUNIT_ASSERT_GT(test, common->max_vasz_lg2,
			pt_top_range(common).max_vasz_lg2);

	/* Add every possible level to the max */
	while (common->max_vasz_lg2 != pt_top_range(common).max_vasz_lg2) {
		struct pt_range top_range = pt_top_range(common);

		if (top_range.va == 0)
			do_map(test, top_range.last_va + 1, 0,
			       priv->smallest_pgsz);
		else
			do_map(test, top_range.va - priv->smallest_pgsz, 0,
			       priv->smallest_pgsz);

		KUNIT_ASSERT_EQ(test, pt_top_range(common).top_level,
				top_range.top_level + 1);
		KUNIT_ASSERT_GE(test, common->max_vasz_lg2,
				pt_top_range(common).max_vasz_lg2);
	}
}

static void test_map_simple(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;
	struct pt_range range = pt_top_range(priv->common);
	struct count_valids valids = {};
	pt_vaddr_t pgsize_bitmap = priv->safe_pgsize_bitmap;
	unsigned int pgsz_lg2;
	pt_vaddr_t cur_va;

	/* Map every reported page size */
	cur_va = range.va + priv->smallest_pgsz * 256;
	for (pgsz_lg2 = 0; pgsz_lg2 != PT_VADDR_MAX_LG2; pgsz_lg2++) {
		pt_oaddr_t paddr = log2_set_mod(priv->test_oa, 0, pgsz_lg2);
		u64 len = log2_to_int(pgsz_lg2);

		if (!(pgsize_bitmap & len))
			continue;

		cur_va = ALIGN(cur_va, len);
		do_map(test, cur_va, paddr, len);
		if (len <= SZ_2G)
			check_iova(test, cur_va, paddr, len);
		cur_va += len;
	}

	/* The read interface reports that every page size was created */
	range = pt_top_range(priv->common);
	KUNIT_ASSERT_NO_ERRNO(test,
			      pt_walk_range(&range, __count_valids, &valids));
	for (pgsz_lg2 = 0; pgsz_lg2 != PT_VADDR_MAX_LG2; pgsz_lg2++) {
		if (pgsize_bitmap & (1ULL << pgsz_lg2))
			KUNIT_ASSERT_EQ(test, valids.per_size[pgsz_lg2], 1);
		else
			KUNIT_ASSERT_EQ(test, valids.per_size[pgsz_lg2], 0);
	}

	/* Unmap works */
	range = pt_top_range(priv->common);
	cur_va = range.va + priv->smallest_pgsz * 256;
	for (pgsz_lg2 = 0; pgsz_lg2 != PT_VADDR_MAX_LG2; pgsz_lg2++) {
		u64 len = log2_to_int(pgsz_lg2);

		if (!(pgsize_bitmap & len))
			continue;
		cur_va = ALIGN(cur_va, len);
		do_unmap(test, cur_va, len);
		cur_va += len;
	}
	KUNIT_ASSERT_EQ(test, count_valids(test), 0);
}

/*
 * Test to convert a table pointer into an OA by mapping something small,
 * unmapping it so as to leave behind a table pointer, then mapping something
 * larger that will convert the table into an OA.
 */
static void test_map_table_to_oa(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;
	pt_vaddr_t limited_pgbitmap =
		priv->info.pgsize_bitmap % (IS_32BIT ? SZ_2G : SZ_16G);
	struct pt_range range = pt_top_range(priv->common);
	unsigned int pgsz_lg2;
	pt_vaddr_t max_pgsize;
	pt_vaddr_t cur_va;

	max_pgsize = 1ULL << (fls64(limited_pgbitmap) - 1);
	KUNIT_ASSERT_TRUE(test, priv->info.pgsize_bitmap & max_pgsize);

	/* FIXME pgsz_lg2 should be random order */
	/* FIXME we need to check we didn't leak memory */
	for (pgsz_lg2 = 0; pgsz_lg2 != PT_VADDR_MAX_LG2; pgsz_lg2++) {
		pt_oaddr_t paddr = log2_set_mod(priv->test_oa, 0, pgsz_lg2);
		u64 len = log2_to_int(pgsz_lg2);
		pt_vaddr_t offset;

		if (!(priv->info.pgsize_bitmap & len))
			continue;
		if (len > max_pgsize)
			break;

		cur_va = ALIGN(range.va + priv->smallest_pgsz * 256,
			       max_pgsize);
		for (offset = 0; offset != max_pgsize; offset += len)
			do_map(test, cur_va + offset, paddr + offset, len);
		check_iova(test, cur_va, paddr, max_pgsize);
		KUNIT_ASSERT_EQ(test, count_valids_single(test, len),
				max_pgsize / len);

		do_unmap(test, cur_va, max_pgsize);

		KUNIT_ASSERT_EQ(test, count_valids(test), 0);
	}
}

static void test_cut_simple(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;
	pt_oaddr_t paddr =
		log2_set_mod(priv->test_oa, 0, priv->largest_pgsz_lg2);
	pt_vaddr_t pgsz = log2_to_int(priv->largest_pgsz_lg2);
	pt_vaddr_t vaddr = pt_top_range(priv->common).va;

	if (priv->largest_pgsz_lg2 == priv->smallest_pgsz_lg2) {
		kunit_skip(test, "Format has only one page size");
		return;
	}

	/* Chop a big page in half */
	do_map(test, vaddr, paddr, pgsz);
	KUNIT_ASSERT_EQ(test, count_valids_single(test, pgsz), 1);
	do_cut(test, vaddr + pgsz / 2);
	KUNIT_ASSERT_EQ(test, count_valids(test),
			log2_to_int(priv->largest_pgsz_lg2 -
				    next_smallest_pgsz_lg2(
					    priv, priv->largest_pgsz_lg2)));
	do_unmap(test, vaddr, pgsz / 2);
	do_unmap(test, vaddr + pgsz / 2, pgsz / 2);

	/* Replace the first item with the smallest page size */
	do_map(test, vaddr, paddr, pgsz);
	KUNIT_ASSERT_EQ(test, count_valids_single(test, pgsz), 1);
	do_cut(test, vaddr + priv->smallest_pgsz);
	do_unmap(test, vaddr, priv->smallest_pgsz);
	do_unmap(test, vaddr + priv->smallest_pgsz, pgsz - priv->smallest_pgsz);
}

/*
 * Test unmapping a small page at the start of a large page. This always unmaps
 * the large page.
 */
static void test_unmap_split(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;
	const struct pt_iommu_ops *ops = priv->iommu->ops;
	struct pt_range top_range = pt_top_range(priv->common);
	pt_vaddr_t pgsize_bitmap = priv->safe_pgsize_bitmap;
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
			size_t gnmapped;

			if (!(pgsize_bitmap & next_len))
				continue;

			do_map(test, vaddr, paddr, next_len);
			gnmapped = ops->unmap_range(priv->iommu, vaddr,
						    base_len, NULL);
			KUNIT_ASSERT_EQ(test, gnmapped, next_len);
		}
	}
}

static struct kunit_case iommu_test_cases[] = {
	KUNIT_CASE_FMT(test_increase_level),
	KUNIT_CASE_FMT(test_map_simple),
	KUNIT_CASE_FMT(test_map_table_to_oa),
	KUNIT_CASE_FMT(test_cut_simple),
	KUNIT_CASE_FMT(test_unmap_split),
	{},
};

static int pt_kunit_iommu_init(struct kunit *test)
{
	struct kunit_iommu_priv *priv;
	int ret;

	test->priv = priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	ret = pt_kunit_priv_init(test, priv);
	if (ret) {
		kfree(test->priv);
		test->priv = NULL;
		return ret;
	}
	return 0;
}

static void pt_kunit_iommu_exit(struct kunit *test)
{
	struct kunit_iommu_priv *priv = test->priv;

	if (!test->priv)
		return;

	pt_iommu_deinit(priv->iommu);
	kfree(test->priv);
}

static struct kunit_suite NS(iommu_suite) = {
	.name = __stringify(NS(iommu_test)),
	.init = pt_kunit_iommu_init,
	.exit = pt_kunit_iommu_exit,
	.test_cases = iommu_test_cases,
};
kunit_test_suites(&NS(iommu_suite));

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kunit for generic page table");
MODULE_IMPORT_NS(GENERIC_PT_IOMMU);
