// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/rlist_dma.h>
#include <kunit/test.h>

static size_t rlist_dma_num_entries(struct rlist_dma *rlist)
{
	struct rlist_dma_entry entry;
	RLIST_DMA_STATE(rls, rlist);
	unsigned int num = 0;

	rlist_dma_for_each_entry (&rls, &entry)
		num++;
	return num;
}

static bool entry_compare(struct rlist_dma_entry *entry,
			  struct rlist_dma_entry ref)
{
        return memcmp(entry, &ref, sizeof(ref)) == 0;
}

static void simple_test(struct kunit *test)
{
	struct rlist_dma rlist;

	rlist_dma_init(&rlist);
	KUNIT_EXPECT_EQ(test, true, rlist_dma_empty(&rlist));
	KUNIT_EXPECT_EQ(test, 0, rlist_dma_num_entries(&rlist));

	/* Add one entry */
	{
		RLIST_DMA_STATE_APPEND(rlsa, &rlist);

		KUNIT_EXPECT_EQ(test, 0, rlsdma_append_begin(&rlsa));
		KUNIT_EXPECT_EQ(test, 0,
				rlsdma_append(&rlsa, 10, 11, 0, GFP_KERNEL));
		rlsdma_append_end(&rlsa);
	}

	KUNIT_EXPECT_EQ(test, 1, rlist_dma_num_entries(&rlist));

	{
		struct rlist_dma_entry entry = {};
		RLIST_DMA_STATE(rls, &rlist);

		KUNIT_EXPECT_EQ(test, true, rlsdma_reset(&rls, &entry));
		KUNIT_EXPECT_EQ(test, true,
				entry_compare(&entry, (struct rlist_dma_entry){
							      .dma_address = 10,
							      .length = 11 }));
	}

	rlist_dma_destroy(&rlist);
}

static struct kunit_case rlist_dma_test_cases[] = {
	KUNIT_CASE(simple_test),
	{},
};

static struct kunit_suite rlist_dma_test_suite = {
	.name = "rlist_dma",
	.test_cases = rlist_dma_test_cases,
};

kunit_test_suite(rlist_dma_test_suite);

MODULE_LICENSE("GPL");
