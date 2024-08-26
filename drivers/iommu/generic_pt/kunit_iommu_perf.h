/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2024-2025, Oracle and/or its affiliates.
 */
#include "kunit_iommu.h"
#include "kunit_iommu_cmp.h"

#define LOOPS 10000
#define START_TIMER(id)	ktime_t start_##id = ktime_get()
#define STOP_TIMER(id) ktime_to_ns(ktime_sub(ktime_get(), start_##id))

struct map_unmap_test_cfg {
	unsigned int iopte_cnt;
	pt_vaddr_t pgsize_bitmap;
	//struct cmp_benchmark_results *cmp_results;
	const char *desc;
};

enum pt_impl_type {
	GENPT_IMPL,
	IOPT_IMPL,
	PT_IMPL_TYPE_MAX,
};

struct benchmark_times {
	ktime_t avg_time;
	ktime_t max_time;
	ktime_t min_time;
	s64 percent;
};

struct cmp_benchmark_results {
	char name[300];
	struct benchmark_times map_timing[PT_IMPL_TYPE_MAX];
	struct benchmark_times unmap_timing[PT_IMPL_TYPE_MAX];
};

typedef void (*benchmark_fn_t)(struct kunit *test,
			       const struct map_unmap_test_cfg *test_args,
			       struct cmp_benchmark_results *benchmark_entry,
			       unsigned int pgsz_lg2);

static inline void compute_single_timing(struct benchmark_times *timing,
					 unsigned int iterations)
{
	struct benchmark_times *genpt = &timing[GENPT_IMPL]; //new
	struct benchmark_times *iopt = &timing[IOPT_IMPL]; //old

	genpt->avg_time = div64_ul(genpt->avg_time, iterations);
	iopt->avg_time = div64_ul(iopt->avg_time, iterations);

	genpt->percent = ((s64)iopt->min_time - (s64)genpt->min_time) *
			 (s64)100 / (s64)(iopt->min_time);
}

static inline void compute_map_timing_stats(struct cmp_benchmark_results *entry,
					    unsigned int iterations)
{
	compute_single_timing(entry->map_timing, iterations);
	compute_single_timing(entry->unmap_timing, iterations);
}

static inline void update_min_time(ktime_t *cur_min, ktime_t delta)
{
	if (likely(*cur_min))
		*cur_min = min_t(ktime_t, *cur_min, delta);
	else
		*cur_min = delta;
}

static inline void update_max_time(ktime_t *cur_max, ktime_t delta)
{
	*cur_max = max_t(ktime_t, *cur_max, delta);
}

static inline void update_measurements(struct benchmark_times *timing,
				       ktime_t delta)
{
	timing->avg_time += delta;

	update_max_time(&timing->max_time, delta);
	update_min_time(&timing->min_time, delta);
}

static void time_map_pages(struct kunit *test, pt_vaddr_t va, pt_oaddr_t pa,
			   pt_vaddr_t len, struct benchmark_times *map_timing)
{
	const unsigned int prot = (IOMMU_READ | IOMMU_WRITE);
	struct kunit_iommu_cmp_priv *cmp_priv = test->priv;
	struct kunit_iommu_priv *priv = &cmp_priv->fmt;
	ktime_t delta;
	int ret_map;

	START_TIMER(genpt);
	ret_map = iommu_map(&priv->domain, va, pa, len, prot, GFP_KERNEL);
	delta = STOP_TIMER(genpt);

	update_measurements(&map_timing[GENPT_IMPL], delta);
	KUNIT_EXPECT_EQ(test, ret_map, 0);

	START_TIMER(iopt);
	ret_map = iommu_map(&cmp_priv->pgtbl_domain, va, pa, len, prot,
			    GFP_KERNEL);
	delta = STOP_TIMER(iopt);

	update_measurements(&map_timing[IOPT_IMPL], delta);
	KUNIT_EXPECT_EQ(test, ret_map, 0);
}

static void time_unmap_pages(struct kunit *test, pt_vaddr_t va, pt_vaddr_t len,
			     struct benchmark_times *unmap_timing)
{
	struct kunit_iommu_cmp_priv *cmp_priv = test->priv;
	struct kunit_iommu_priv *priv = &cmp_priv->fmt;
	size_t ret_unmap;
	ktime_t delta;

	START_TIMER(genpt);
	ret_unmap = iommu_unmap(&priv->domain, va, len);
	delta = STOP_TIMER(genpt);

	update_measurements(&unmap_timing[GENPT_IMPL], delta);

	KUNIT_EXPECT_EQ(test, ret_unmap, len);

	START_TIMER(iopt);
	ret_unmap = iommu_unmap(&cmp_priv->pgtbl_domain, va, len);
	delta = STOP_TIMER(iopt);

	update_measurements(&unmap_timing[IOPT_IMPL], delta);

	KUNIT_EXPECT_EQ(test, ret_unmap, len);
}

/*
 * Test {un}map_pages(), no mem allocation.
 */
static void do_map_unmap_benchmark(
	struct kunit *test, const struct map_unmap_test_cfg *test_case,
	struct cmp_benchmark_results *benchmark_entry, unsigned int pgsz_lg2)
{
	struct benchmark_times *unmap_timing = benchmark_entry->unmap_timing;
	struct benchmark_times *map_timing = benchmark_entry->map_timing;
	struct kunit_iommu_cmp_priv *cmp_priv = test->priv;
	struct kunit_iommu_priv *genpt_priv = &cmp_priv->fmt;
	struct pt_range top_range = pt_top_range(cmp_priv->fmt.common);
	unsigned int iopte_cnt;
	unsigned int loops = 0;
	pt_vaddr_t test_va;
	pt_oaddr_t test_pa;
	pt_vaddr_t len;

	/* If test case does not specify IOPTE count, assume 1 */
	iopte_cnt = test_case->iopte_cnt;
	if (!iopte_cnt)
		iopte_cnt = 1;
	len = test_case->iopte_cnt * log2_to_int(pgsz_lg2);

	if (len >= top_range.last_va)
		return;

	/*
	 * Generate an OA that is aligned to the page size Generate a VA that is
	 * within the range but is different from the OA to ensure there is no
	 * page combining.
	 */
	test_pa = oalog2_set_mod(genpt_priv->test_oa, 0, pgsz_lg2);
	test_va = log2_set_mod(~test_pa, 0, pgsz_lg2);
	test_va = log2_mod(test_va, pgsz_lg2 + 1);
	test_va |= top_range.va;
	KUNIT_ASSERT_LE(test, top_range.va, test_va);
	KUNIT_ASSERT_GT(test, top_range.last_va, test_va);
	KUNIT_ASSERT_GT(test, top_range.last_va, test_va + (len - 1));

	/* Throw away first mapping to avoid timing memory allocation */
	time_map_pages(test, test_va, test_pa, len, map_timing);
	time_unmap_pages(test, test_va, len, unmap_timing);
	memset(benchmark_entry, 0, sizeof(*benchmark_entry));

	if (iopte_cnt == 1)
		snprintf(benchmark_entry->name, sizeof(benchmark_entry->name),
			 "2^%u", pgsz_lg2);
	else
		snprintf(benchmark_entry->name, sizeof(benchmark_entry->name),
			 "%u*2^%u", iopte_cnt, pgsz_lg2);

	/* Timing loop */
	for (loops = 0; loops < LOOPS; loops++) {
		/* map_pages() benchmark */
		time_map_pages(test, test_va, test_pa, len, map_timing);

		/* unmap_pages() benchmark */
		time_unmap_pages(test, test_va, len, unmap_timing);
	}

	/*
	 * Calculate avg duration for both implementations.
	 * TODO: use MEASURE_{} macros to improve readability.
	 */
	compute_map_timing_stats(benchmark_entry, loops);
}

static inline void
test_on_valid_pgsize(struct kunit *test, benchmark_fn_t fn,
		     const struct map_unmap_test_cfg *test_args,
		     struct cmp_benchmark_results *cmp_results,
		     pt_vaddr_t pgsize_bitmap)
{
	unsigned int pgsz_lg2;

	for (pgsz_lg2 = 0; pgsz_lg2 != PT_VADDR_MAX_LG2; pgsz_lg2++) {
		/* Skip unsupported page sizes */
		if (!(pgsize_bitmap & log2_to_int(pgsz_lg2)))
			continue;

		fn(test, test_args, &cmp_results[pgsz_lg2], pgsz_lg2);
	}
}

/*
 * Max doesn't seem so useful, it randomly hits high values probably due to
 * scheduling.
 *
 */
#define REPORT_BANNER_STR(op) \
	"\n" op " \n   pgsz  ,avg new,old ns, min new,old ns  , min %% (+ve is better)\n"

#define REPORT_FMT_STR "%9s,% 7lld,%-6lld,% 8lld,%-8lld, % 3lld.%02llu\n"

#define REPORT_PARAM_LIST                                                \
	cmp_results[idx].name, result[GENPT_IMPL].avg_time,              \
		result[IOPT_IMPL].avg_time, result[GENPT_IMPL].min_time, \
		result[IOPT_IMPL].min_time, result[GENPT_IMPL].percent,  \
		abs(result[GENPT_IMPL].percent) % 100

static void report_timing_results(struct kunit *test, pt_vaddr_t pgsize_bitmap,
				  struct cmp_benchmark_results *cmp_results)
{
	unsigned int idx;

	/*
	 * Now all the timing results have been populated, output them in CSV
	 * format for plotting.
	 */
	kunit_info(test, REPORT_BANNER_STR("map_pages"));
	for (idx = 0; idx < PT_VADDR_MAX_LG2; idx++) {
		struct benchmark_times *result = cmp_results[idx].map_timing;

		if (!(pgsize_bitmap & BIT(idx)) || !result->max_time)
			continue;

		pr_info(REPORT_FMT_STR, REPORT_PARAM_LIST);
	}

	kunit_info(test, REPORT_BANNER_STR("unmap_pages"));
	for (idx = 0; idx < PT_VADDR_MAX_LG2; idx++) {
		struct benchmark_times *result = cmp_results[idx].unmap_timing;

		if (!(pgsize_bitmap & BIT(idx)) || !result->max_time)
			continue;

		pr_info(REPORT_FMT_STR, REPORT_PARAM_LIST);
	}
	memset(cmp_results, 0, sizeof(*cmp_results) * PT_VADDR_MAX_LG2);
}

static const struct map_unmap_test_cfg map_unmap_tests[] = {
	{
		.iopte_cnt = 1,
		.desc = "Single IOPTE",
	},
	{
		.iopte_cnt = 256,
		.pgsize_bitmap = (SZ_4K | SZ_2M | SZ_1G),
		.desc = "256 IOPTE",
	},
};

/*
 * Benchmark map/unmap various combinations defined by NS(map_unmap_tests) list.
 * This test is a clear candidate for the parameterized testing support offered
 * by Kunit framework, but that facility is already in use for testing of format
 * specific features, so set this up manually.
 */
static void test_map_unmap_benchmark(struct kunit *test)
{
	struct kunit_iommu_cmp_priv *cmp_priv = test->priv;
	struct kunit_iommu_priv *genpt_priv = &cmp_priv->fmt;
	struct cmp_benchmark_results *cmp_results;
	pt_vaddr_t pgsize_bitmap;
	unsigned int i;

	/*
	 * Use safe pgsize_bitmap determined during test initialization as
	 * baseline, and restrict the pgsizes if required by specific tests.
	 */
	pgsize_bitmap = genpt_priv->safe_pgsize_bitmap;

	/*
	 * Allocate array of struct commpare_timings holding PT_VADDR_MAX_LG2
	 * entries for comparison benchmarks. Entries for unsupported pagesizes
	 * are wasted, so this can be optimized.
	 */
	cmp_results = kunit_kcalloc(test, PT_VADDR_MAX_LG2,
				    sizeof(*cmp_results), GFP_KERNEL);

	for (i = 0; i < ARRAY_SIZE(map_unmap_tests); i++) {
		/* Restrict supported pagesizes if test case requests it */
		if (map_unmap_tests[i].pgsize_bitmap)
			pgsize_bitmap &= map_unmap_tests[i].pgsize_bitmap;

		test_on_valid_pgsize(test, do_map_unmap_benchmark,
				     &map_unmap_tests[i], cmp_results,
				     pgsize_bitmap);

		kunit_info(test, "\nTest case: %s\n", map_unmap_tests[i].desc);
		report_timing_results(test, pgsize_bitmap, cmp_results);
	}
}

static struct kunit_case perf_test_cases[] = {
	KUNIT_CASE_FMT(test_map_unmap_benchmark),
	{},
};

static struct kunit_suite NS(perf_suite) = {
	.name = __stringify(NS(iommu_perf_test)),
	.init = pt_kunit_iommu_cmp_init,
	.exit = pt_kunit_iommu_cmp_exit,
	.test_cases = perf_test_cases,
};
kunit_test_suites(&NS(perf_suite));
