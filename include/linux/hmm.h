/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright 2013 Red Hat Inc.
 *
 * Authors: Jérôme Glisse <jglisse@redhat.com>
 *
 * See Documentation/vm/hmm.rst for reasons and overview of what HMM is.
 */
#ifndef LINUX_HMM_H
#define LINUX_HMM_H

#include <linux/kconfig.h>
#include <asm/pgtable.h>

#include <linux/device.h>
#include <linux/migrate.h>
#include <linux/memremap.h>
#include <linux/completion.h>
#include <linux/mmu_notifier.h>

/*
 * On output:
 * HMM_PFN_FAULTABLE - access the pfn is impossible, but a future request
 *                     with HMM_PFN_REQ_FAULT could succeed.
 * HMM_PFN_VALID - the pfn field points to a valid PFN. This PFN is at
 *                 least readable
 * HMM_PFN_ERROR - accessing the pfn is impossible and the device should
 *                 fail. ie poisoned memory, special pages, no vma, etc
 *
 * These bits can only be set if HMM_PFN_VALID:
 * HMM_PFN_DEVICE_PRIVATE - if the pfn is a ZONE_DEVICE MEMORY_DEVICE_PRIVATE
 *                          page. This cannot be set unless
 *                          dev_private_owner != NULL
 * HMM_PFN_WRITE - if the page memory can be written to
 *
 * On input:
 * HMM_PFN_REQ_SNAPSHOT - Make no change to this page, the output may not have
 *                        HMM_PFN_VALID set.
 * HMM_PFN_REQ_FAULT - The output must have HMM_PFN_VALID or hmm_range_fault()
 *                     will fail
 * HMM_PFN_REQ_WRITE - The output must have HMM_PFN_WRITE or hmm_range_fault()
 *                     will fail. Must be combined with HMM_PFN_REQ_FAULT.
 */
enum hmm_pfn_flags {
	HMM_PFN_FAULTABLE = 0,
	HMM_PFN_VALID = 1 << 0,
	HMM_PFN_ERROR = 1 << 1,
	HMM_PFN_WRITE = 1 << 2,
	HMM_PFN_DEVICE_PRIVATE = 1 << 3,

	HMM_PFN_REQ_SNAPSHOT = 0,
	HMM_PFN_REQ_FAULT = HMM_PFN_VALID,
	HMM_PFN_REQ_WRITE = HMM_PFN_WRITE,
};

struct hmm_pfn {
	unsigned long pfn : (BITS_PER_LONG - 4);
	unsigned long flags : 4;
};
static_assert(sizeof(struct hmm_pfn) == sizeof(unsigned long));

/**
 * hmm_pfn_req() - Construct a request hmm_pfn
 *
 * Request hmm_pfn's are passed into hmm_range_fault() and set the HMM_PFN_REQ_*
 * flags. After hmm_range_fault() succeeds no REQ flags will be set.
 */
static inline struct hmm_pfn hmm_pfn_req(unsigned long flags)
{
	return (struct hmm_pfn){ .flags = flags };
}

/*
 * hmm_pfn_to_page() - return struct page pointed to by a device entry
 *
 * This must be called under the caller 'user_lock' after a successful
 * mmu_interval_read_begin(). The caller must have tested for HMM_PFN_VALID
 * already.
 */
static inline struct page *hmm_pfn_to_page(const struct hmm_pfn *pfn)
{
	return pfn_to_page(pfn->pfn);
}

/*
 * struct hmm_range - track invalidation lock on virtual address range
 *
 * @notifier: a mmu_interval_notifier that includes the start/end
 * @notifier_seq: result of mmu_interval_read_begin()
 * @start: range virtual start address (inclusive)
 * @end: range virtual end address (exclusive)
 * @pfns: array of pfns (big enough for the range)
 * @default_flags: default flags for the range (write, read, ... see hmm doc)
 * @pfn_flags_mask: allows to mask pfn flags so that only default_flags matter
 * @dev_private_owner: owner of device private pages
 */
struct hmm_range {
	struct mmu_interval_notifier *notifier;
	unsigned long		notifier_seq;
	unsigned long		start;
	unsigned long		end;
	struct hmm_pfn		*pfns;
	unsigned long		default_flags;
	unsigned long		pfn_flags_mask;
	void			*dev_private_owner;
};

/*
 * Please see Documentation/vm/hmm.rst for how to use the range API.
 */
long hmm_range_fault(struct hmm_range *range);

/*
 * HMM_RANGE_DEFAULT_TIMEOUT - default timeout (ms) when waiting for a range
 *
 * When waiting for mmu notifiers we need some kind of time out otherwise we
 * could potentialy wait for ever, 1000ms ie 1s sounds like a long time to
 * wait already.
 */
#define HMM_RANGE_DEFAULT_TIMEOUT 1000

#endif /* LINUX_HMM_H */
