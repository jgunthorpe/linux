// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/rlist.h>
#include <kunit/test.h>

static void simple_test(struct kunit *test)
{
}

static struct kunit_case rlist_cpu_test_cases[] = {
	KUNIT_CASE(simple_test),
	{},
};

static struct kunit_suite rlist_cpu_test_suite = {
	.name = "rlist_cpu",
	.test_cases = rlist_cpu_test_cases,
};

kunit_test_suite(rlist_cpu_test_suite);

MODULE_LICENSE("GPL");
