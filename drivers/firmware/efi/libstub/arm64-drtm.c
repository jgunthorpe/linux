// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES
 *
 * EFI Stub implementation for ARM DEN 0113 "DRTM Architecture for Arm"
 *
 * This code runs on the untrusted side of the boot in the EFI stub. Nothing it
 * does should impact the integrity of the post-launch world and it does not
 * need to take special security precautions.
 */
#include <linux/bitfield.h>
#include <linux/efi.h>
#include <linux/kconfig.h>
#include <linux/overflow.h>
#include <linux/tpm_command.h>

#include <asm/cputype.h>
#include <asm/drtm.h>

#include "efistub.h"
#include "arm64-stub.h"

struct efi_drtm_config {
	u32 launch_features;
	unsigned long dlme_data_size;
	unsigned long nw_dce_size;
	struct arm_drtm_parameters *params_addr;
	struct arm64_drtm_handoff *handoff;
};
static struct efi_drtm_config drtm_cfg;

static bool efi_drtm_query_feature(u64 feature, u64 *value)
{
	s64 status;

	status = arm_drtm_query_feature(feature, value);
	if (status < 1) {
		efi_err("DRTM: feature %llx query failed (x0=%lld)\n", feature,
			status);
		return false;
	}

	return true;
}

/*
 * For the kernel to boot it must have the matching hash algorithm built in to
 * be able to validate the TCB hashes. Assume the TPM hash algorithm is the one
 * that TCB_HASH_TABLE will be using.
 */
static bool efi_drtm_hash_algorithm_supported(u16 algorithm)
{
	switch (algorithm) {
	case TPM_ALG_SHA256:
		return IS_BUILTIN(CONFIG_CRYPTO_SHA256);
	case TPM_ALG_SHA384:
	case TPM_ALG_SHA512:
		return IS_BUILTIN(CONFIG_CRYPTO_SHA512);
	default:
		return false;
	}
}

static bool efi_drtm_probe_tpm(void)
{
	u16 algorithm;
	u8 schemas;
	u64 value;

	if (!efi_drtm_query_feature(ARM_DRTM_FEATURE_TPM, &value))
		return false;
	algorithm = FIELD_GET(ARM_DRTM_TPM_ALG_MASK, value);
	schemas = FIELD_GET(ARM_DRTM_PCR_SCHEMA_MASK, value);

	if (!efi_drtm_hash_algorithm_supported(algorithm)) {
		efi_err("DRTM: firmware hash algorithm 0x%x is unavailable (feature 0x%llx)\n",
			algorithm, value);
		return false;
	}

	if (!(schemas & BIT(ARM_DRTM_PCR_SCHEMA_DEFAULT))) {
		efi_err("DRTM: default PCR schema is unavailable (feature 0x%llx)\n",
			value);
		return false;
	}

	efi_debug("DRTM: TPM algorithm 0x%x, TPM hashing %s, PCR schemas 0x%x\n",
		  algorithm,
		  value & ARM_DRTM_TPM_HASHING ? "available" : "unavailable",
		  schemas);
	return true;
}

static bool efi_drtm_probe_memory(void)
{
	u64 value;

	if (!efi_drtm_query_feature(ARM_DRTM_FEATURE_MIN_MEMORY, &value))
		return false;

	drtm_cfg.dlme_data_size =
		FIELD_GET(ARM_DRTM_DLME_DATA_PAGES_MASK, value) *
		ARM_DRTM_PAGE_SIZE;
	drtm_cfg.nw_dce_size = FIELD_GET(ARM_DRTM_NW_DCE_PAGES_MASK, value) *
			       ARM_DRTM_PAGE_SIZE;

	efi_debug("DRTM: minimum DLME data %lu bytes, Normal-world DCE %lu bytes\n",
		  drtm_cfg.dlme_data_size, drtm_cfg.nw_dce_size);
	return true;
}

static bool efi_drtm_probe_dma(void)
{
	u64 value;

	if (!efi_drtm_query_feature(ARM_DRTM_FEATURE_DMA_PROTECTION, &value))
		return false;

	if (!(FIELD_GET(ARM_DRTM_DMA_PROTECTION_MASK, value) &
	      BIT(ARM_DRTM_DMA_PROTECTION_COMPLETE))) {
		efi_err("DRTM: complete DMA protection is unavailable (feature 0x%llx)\n",
			value);
		return false;
	}
	return true;
}

/* Sanity check nothing has gone wrong */
static bool efi_drtm_probe_boot_pe(void)
{
	u64 current_pe = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;
	u64 boot_pe;

	if (!efi_drtm_query_feature(ARM_DRTM_FEATURE_BOOT_PE, &boot_pe))
		return false;

	if ((boot_pe & MPIDR_HWID_BITMASK) != current_pe) {
		efi_err("DRTM: boot PE 0x%llx does not match current PE 0x%llx\n",
			boot_pe, current_pe);
		return false;
	}

	return true;
}

static void efi_drtm_report_previous_error(void)
{
	s64 error_code;
	s64 status;

	status = arm_drtm_query_function(ARM_DRTM_SMC_GET_ERROR);
	if (status != ARM_DRTM_SUCCESS) {
		efi_debug("DRTM: GET_ERROR is unavailable (x0=%lld)\n", status);
		return;
	}

	status = arm_drtm_get_error(&error_code);
	if (status != ARM_DRTM_SUCCESS) {
		efi_warn("DRTM: failed to read previous error (x0=%lld)\n",
			 status);
		return;
	}

	if (error_code) {
		efi_warn("DRTM: firmware reports previous launch error 0x%llx\n",
			 error_code);
		if (efi_drtm_policy != EFI_DRTM_ENFORCE)
			efi_drtm_policy = EFI_DRTM_OFF;
	}
}

static efi_status_t efi_drtm_failure(void)
{
	if (efi_drtm_policy == EFI_DRTM_AUTO) {
		efi_warn("DRTM: preparation failed, continuing without DRTM\n");
		efi_drtm_policy = EFI_DRTM_OFF;
		return EFI_SUCCESS;
	}

	efi_err("DRTM: enforced preparation failed\n");
	return EFI_UNSUPPORTED;
}

efi_status_t efi_drtm_prepare(void)
{
	s64 feature_status;
	u16 major, minor;
	s32 status;

	if (efi_drtm_policy == EFI_DRTM_OFF)
		return EFI_SUCCESS;

	if (!efi_arm64_smccc_1_1())
		return efi_drtm_failure();

	/*
	 * v1.4B section 3.2.1 "DRTM_VERSION usage" explains that a new major
	 * ABI version may "Change behavior of existing functions". Verify the
	 * major version before calling anything so we don't trigger unknown
	 * behavior.
	 */
	status = arm_drtm_version(&major, &minor);
	if (status != ARM_DRTM_SUCCESS) {
		efi_err("DRTM: failed to read interface version (x0=%d)\n",
			status);
		return efi_drtm_failure();
	}
	if (major != ARM_DRTM_VERSION_MAJOR) {
		efi_err("DRTM: unsupported interface version %u.%u\n", major,
			minor);
		return efi_drtm_failure();
	}
	efi_info("DRTM: interface version %u.%u\n", major, minor);

	efi_drtm_report_previous_error();
	if (efi_drtm_policy == EFI_DRTM_OFF)
		return EFI_SUCCESS;

	feature_status = arm_drtm_query_function(ARM_DRTM_SMC_DYNAMIC_LAUNCH);
	if (feature_status != ARM_DRTM_SUCCESS) {
		efi_err("DRTM: dynamic launch is unavailable (x0=%lld)\n",
			feature_status);
		return efi_drtm_failure();
	}

	if (!efi_drtm_probe_tpm() || !efi_drtm_probe_memory() ||
	    !efi_drtm_probe_dma() || !efi_drtm_probe_boot_pe())
		return efi_drtm_failure();

	drtm_cfg.launch_features =
		ARM_DRTM_LAUNCH_HASH_FIRMWARE |
		FIELD_PREP(ARM_DRTM_LAUNCH_PCR_SCHEMA_MASK,
			   ARM_DRTM_PCR_SCHEMA_DEFAULT) |
		FIELD_PREP(ARM_DRTM_LAUNCH_DMA_PROTECTION_MASK,
			   ARM_DRTM_DMA_PROTECTION_COMPLETE) |
		ARM_DRTM_LAUNCH_NO_AUTH | ARM_DRTM_LAUNCH_KEEP_SECURE_IRQS;

	return EFI_SUCCESS;
}

unsigned long efi_drtm_get_extra_size(void)
{
	/*
	 * See arch/arm64/kernel/vmlinux.lds.S for the DRTM Memory layout.
	 *
	 * v1.4B Table 6 "Return values for DRTM features", feature 0x2 reports
	 * both minimum sizes in 4 KiB pages.
	 *
	 * The linker places the DLME data at the 4 KiB-aligned end of the
	 * static Image as required by R314030 in Table 13 "DLME region
	 * requirements".
	 *
	 * The DLME region must include all of that data (R45200 in Table 30
	 * "DCE requirements for the DLME"), and an optional Normal-world DCE
	 * follows it at another 4 KiB-aligned address as required by R312080 in
	 * Table 10 "DRTM_PARAMETERS requirements".
	 *
	 * A final page holds the 4 KiB-aligned DRTM_PARAMETERS required by
	 * R312010 in the same table. Only the first area is part of the DLME
	 * region.
	 */
	if (efi_drtm_policy == EFI_DRTM_OFF)
		return 0;
	return drtm_cfg.dlme_data_size + drtm_cfg.nw_dce_size +
	       ARM_DRTM_PAGE_SIZE;
}

efi_status_t efi_drtm_prepare_launch(const struct efi_image_info *image_info,
				     unsigned long fdt_addr)
{
	struct arm64_drtm_handoff *handoff =
		efistub_drtm_handoff(image_info);
	struct arm_drtm_parameters *params;
	unsigned long measured_offset;
	unsigned long measured_size;
	unsigned long image_base;
	unsigned long image_size;
	unsigned long dlme_start;
	unsigned long dlme_end;
	unsigned long dce_end;
	unsigned long entry;

	if (efi_drtm_policy == EFI_DRTM_OFF)
		return EFI_SUCCESS;

	image_base = efistub_kernel_base(image_info);
	image_size = efistub_kernel_memsize(image_info);
	measured_offset = efistub_drtm_measured_offset(image_info);
	measured_size = efistub_drtm_measured_size(image_info);
	entry = efistub_drtm_entry_offset(image_info);
	dlme_start = image_base + image_size;
	dlme_end = dlme_start + drtm_cfg.dlme_data_size;
	dce_end = dlme_end + drtm_cfg.nw_dce_size;

	/* Quick checks something didn't go wrong during image construction */
	if (entry < measured_offset ||
	    entry - measured_offset >= measured_size ||
	    !IS_ALIGNED(image_base, ARM_DRTM_PAGE_SIZE) ||
	    !IS_ALIGNED(image_size, ARM_DRTM_PAGE_SIZE) ||
	    !IS_ALIGNED(measured_offset, ARM_DRTM_PAGE_SIZE) ||
	    !IS_ALIGNED(dlme_start, ARM_DRTM_PAGE_SIZE) ||
	    !IS_ALIGNED(dlme_end, ARM_DRTM_PAGE_SIZE) ||
	    !IS_ALIGNED(dce_end, ARM_DRTM_PAGE_SIZE)) {
		efi_err("DRTM: final Image layout is invalid\n");
		return efi_drtm_failure();
	}

	/*
	 * For robustness zero all the trailing space at the end of the
	 * alocation. This contains the DRTM_PARAMETERS structure too, so
	 * must be done before filling it.
	 */
	memset((void *)dlme_start, 0, efi_drtm_get_extra_size());
	params = (void *)dce_end;
	params->revision = cpu_to_le16(1);
	/* PCR authorities, image authentication and secure IRQ disable need v2. */
	if (drtm_cfg.launch_features &
	    (ARM_DRTM_LAUNCH_PCR_SCHEMA_MASK | ARM_DRTM_LAUNCH_AUTH |
	     ARM_DRTM_LAUNCH_DISABLE_SECURE_IRQS))
		params->revision = cpu_to_le16(2);
	params->launch_features = cpu_to_le32(drtm_cfg.launch_features);
	params->dlme_region_address = cpu_to_le64(image_base);
	params->dlme_region_size = cpu_to_le64(dlme_end - image_base);
	params->dlme_image_start = cpu_to_le64(measured_offset);
	params->dlme_entry_point_offset = cpu_to_le64(entry - measured_offset);
	params->dlme_image_size = cpu_to_le64(measured_size);
	params->dlme_data_offset = cpu_to_le64(image_size);
	if (drtm_cfg.nw_dce_size) {
		params->nw_dce_region_address = cpu_to_le64(dlme_end);
		params->nw_dce_region_size = cpu_to_le64(drtm_cfg.nw_dce_size);
	}

	/*
	 * The DLME Data contains its own size in a trusted header, so the
	 * handoff doesn't need to include extra_size. The DCE Data is only
	 * temporary so the kernel also does not need to know about it.
	 */
	handoff->fdt_addr = cpu_to_le64(fdt_addr);
	handoff->drtm_enabled = 1;

	efi_debug("DRTM: will launch, selected launch features 0x%x\n",
		  drtm_cfg.launch_features);

	drtm_cfg.params_addr = params;
	drtm_cfg.handoff = handoff;
	return EFI_SUCCESS;
}

static void efi_drtm_fallback(void)
{
	drtm_cfg.handoff->drtm_enabled = 0;
}

void efi_drtm_launch(void)
{
	if (efi_drtm_policy == EFI_DRTM_OFF)
		return;

	/*
	 * No cache maintenance is required before the launch. v1.4B R42130 in
	 * Table 22 "DCE preamble requirements" requires the DRTM_PARAMETERS to
	 * be accessible as Normal Write-Back Cacheable, Inner Shareable memory,
	 * so the DCE reads them coherently with the writes made above.  R45220
	 * in Table 30 "DCE requirements for the DLME" then has the DCE clean
	 * and invalidate the whole DLME region to the Point of Coherency before
	 * it measures the DLME image, which covers both the Image itself and
	 * the handoff struct placed in the DLME region. Thus once we jump into
	 * the kernel with MMU and caches off the CPU will see everything the
	 * stub wrote.
	 */
	arm_drtm_dynamic_launch(drtm_cfg.params_addr);

	/*
	 * v1.4B section 3.4 "DRTM_DYNAMIC_LAUNCH" says it returns from
	 * DYNAMIC_LAUNCH only on error. Boot services and their diagnostics are
	 * no longer available, so hang.
	 */
	if (efi_drtm_policy == EFI_DRTM_ENFORCE) {
		for (;;)
			asm volatile("wfe");
	}

	efi_drtm_fallback();
}
