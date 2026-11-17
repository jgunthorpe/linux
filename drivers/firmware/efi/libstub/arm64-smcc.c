// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES
 *
 * Determine if ARM64 can use SMCC 1.1 calls.
 */
#include <linux/acpi.h>

#include <uapi/linux/psci.h>

#include "efistub.h"
#include "arm64-stub.h"

static bool efi_acpi_checksum_valid(const void *table, u32 length)
{
	const u8 *p = table;
	u8 checksum = 0;

	/*
	 * Nothing this code walks is remotely close to this large.  The length
	 * fields are firmware supplied and are used to bound checksum loops, so
	 * cap them to avoid checksumming forever if one is corrupt.
	 */
	if (length > SZ_1M)
		return false;

	while (length--)
		checksum += *p++;

	return checksum == 0;
}

static const struct acpi_table_fadt *
efi_acpi_find_fadt(const struct acpi_table_xsdt *xsdt)
{
	u32 nr_entries;
	u32 i;

	nr_entries = (xsdt->header.length -
		      offsetof(struct acpi_table_xsdt, table_offset_entry)) /
		     sizeof(xsdt->table_offset_entry[0]);
	for (i = 0; i < nr_entries; i++) {
		u64 address = xsdt->table_offset_entry[i];
		const struct acpi_table_header *table;

		if (!address)
			continue;

		table = (const void *)address;
		if (!memcmp(table->signature, ACPI_SIG_FADT,
			    sizeof(table->signature)))
			return (const void *)table;
	}

	return NULL;
}

/**
 * efi_arm64_smc_conduit_available() - Discover an advertised SMC conduit
 *
 * Very lightly parse the ACPI tables to reach FADT. The goal is to parse things
 * that are valid and fail off for odd looking stuff. The checksums are used to
 * double check the limited parsing hasn't gone off the rails.
 *
 * Return: true only if a valid ACPI 2.0-or-newer table hierarchy contains a
 * FADT advertising PSCI over SMC.  This establishes that issuing an SMCCC SMC
 * is permitted
 */
static bool efi_arm64_smc_conduit_available(void)
{
	const struct acpi_table_fadt *fadt;
	const struct acpi_table_xsdt *xsdt;
	const struct acpi_table_rsdp *rsdp;
	u32 length;
	u16 flags;

	rsdp = get_efi_config_table(ACPI_20_TABLE_GUID);
	if (!rsdp) {
		efi_debug(
			"DRTM: ACPI 2.0 RSDP is unavailable; SMC conduit is unknown\n");
		return false;
	}

	if (memcmp(rsdp->signature, ACPI_SIG_RSDP, sizeof(rsdp->signature))) {
		efi_err("DRTM: ACPI RSDP signature is invalid\n");
		return false;
	}

	if (rsdp->revision < 2) {
		efi_err("DRTM: ACPI RSDP revision does not provide an XSDT\n");
		return false;
	}

	length = rsdp->length;
	if (length < sizeof(*rsdp)) {
		efi_err("DRTM: ACPI RSDP length is invalid\n");
		return false;
	}
	if (!efi_acpi_checksum_valid(rsdp, offsetof(struct acpi_table_rsdp,
						    length))) {
		efi_err("DRTM: ACPI RSDP legacy checksum is invalid\n");
		return false;
	}

	if (!efi_acpi_checksum_valid(rsdp, length)) {
		efi_err("DRTM: ACPI RSDP extended checksum is invalid\n");
		return false;
	}

	xsdt = (const void *)rsdp->xsdt_physical_address;
	if (!xsdt) {
		efi_err("DRTM: ACPI RSDP does not provide an XSDT\n");
		return false;
	}

	if (memcmp(xsdt->header.signature, ACPI_SIG_XSDT,
		   sizeof(xsdt->header.signature))) {
		efi_err("DRTM: ACPI XSDT signature is invalid\n");
		return false;
	}

	length = xsdt->header.length;
	if (length < offsetof(struct acpi_table_xsdt, table_offset_entry) ||
	    (length - offsetof(struct acpi_table_xsdt, table_offset_entry)) %
		    sizeof(xsdt->table_offset_entry[0])) {
		efi_err("DRTM: ACPI XSDT length is invalid\n");
		return false;
	}
	if (!efi_acpi_checksum_valid(xsdt, length)) {
		efi_err("DRTM: ACPI XSDT checksum is invalid\n");
		return false;
	}

	fadt = efi_acpi_find_fadt(xsdt);
	if (!fadt) {
		efi_err("DRTM: ACPI XSDT does not contain a FADT\n");
		return false;
	}

	length = fadt->header.length;

	/*
	 * No FADT revision check is needed, the Arm boot flags land on bytes
	 * that every pre-5.1 revision defines as reserved and zero, which
	 * simply reads back as 'no PSCI'.
	 */
	if (length < offsetofend(struct acpi_table_fadt, arm_boot_flags)) {
		efi_debug("DRTM: ACPI FADT does not contain Arm boot flags\n");
		return false;
	}
	if (!efi_acpi_checksum_valid(fadt, length)) {
		efi_err("DRTM: ACPI FADT checksum is invalid\n");
		return false;
	}

	flags = fadt->arm_boot_flags;
	if (!(flags & ACPI_FADT_PSCI_COMPLIANT)) {
		efi_debug("DRTM: ACPI FADT does not advertise PSCI\n");
		return false;
	}

	if (flags & ACPI_FADT_PSCI_USE_HVC) {
		efi_debug(
			"DRTM: ACPI FADT advertises PSCI over HVC, not SMC\n");
		return false;
	}

	return true;
}

/*
 * Discovery must tolerate SMCCC 1.0 clobbering x0-x17. The EFI stub cannot
 * call the kernel's out-of-line SMCCC helper, so issue these calls inline.
 */
static u32 efi_arm64_smc_1_0(u32 function, u32 arg)
{
	register unsigned long x0 asm("x0") = function;
	register unsigned long x1 asm("x1") = arg;

	asm volatile("smc #0"
		     : "+r" (x0), "+r" (x1)
		     :
		     : "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
		       "x10", "x11", "x12", "x13", "x14", "x15", "x16",
		       "x17", "cc", "memory");

	return x0;
}

/* True if SMCCC 1.1 or later in the 1.x series is available. */
bool efi_arm64_smccc_1_1(void)
{
	u32 version;
	s32 status;

	if (!efi_arm64_smc_conduit_available())
		return false;

	version = efi_arm64_smc_1_0(PSCI_0_2_FN_PSCI_VERSION, 0);
	if ((s32)version < 0)
		return false;

	if (PSCI_VERSION_MAJOR(version) < 1)
		return false;

	status = efi_arm64_smc_1_0(PSCI_1_0_FN_PSCI_FEATURES,
				   ARM_SMCCC_VERSION_FUNC_ID);
	if (status < 0)
		return false;

	version = efi_arm64_smc_1_0(ARM_SMCCC_VERSION_FUNC_ID, 0);
	return (version >> 16) == 1 && version >= ARM_SMCCC_VERSION_1_1;
}
