// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/rlist.h>
#include <kunit/test.h>

static size_t rlist_num_entries(struct rlist *rlist)
{
	struct rlist_entry entry;
	RLIST_STATE(rls, rlist);
	unsigned int num = 0;

	rlist_for_each_entry(&rls, &entry)
		num++;
	return num;
}

static bool entry_compare(struct rlist_entry *entry, struct rlist_entry ref)
{
	return memcmp(entry, &ref, sizeof(ref)) == 0;
}

static void simple_test(struct kunit *test)
{
	struct rlist rlist;

	rlist_init(&rlist);
	KUNIT_EXPECT_EQ(test, true, rlist_empty(&rlist));
	KUNIT_EXPECT_EQ(test, 0, rlist_num_entries(&rlist));

	/* Add one entry */
	{
		RLIST_STATE_APPEND(rlsa, &rlist);
		struct rlist_entry entry;

		KUNIT_EXPECT_EQ(test, 0, rls_append_begin(&rlsa));
		entry = (struct rlist_entry){ .base = 10, .length = 10 };
		KUNIT_EXPECT_EQ(test, 0, rls_append(&rlsa, &entry, GFP_KERNEL));
		rls_append_end(&rlsa);
	}

	KUNIT_EXPECT_EQ(test, 1, rlist_num_entries(&rlist));

	{
		struct rlist_entry entry = {};
		RLIST_STATE(rls, &rlist);

		KUNIT_EXPECT_EQ(test, true, rls_reset(&rls, &entry));
		KUNIT_EXPECT_EQ(test, true,
				entry_compare(&entry, (struct rlist_entry){
							      .base = 10,
							      .length = 10 }));
	}

	rlist_destroy(&rlist);
}

static struct kunit_case rlist_test_cases[] = {
	KUNIT_CASE(simple_test),
	{},
};

static struct kunit_suite rlist_test_suite = {
	.name = "rlist",
	.test_cases = rlist_test_cases,
};

kunit_test_suite(rlist_test_suite);

MODULE_LICENSE("GPL");
