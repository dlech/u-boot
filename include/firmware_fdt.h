/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __FIRMWARE_FDT_H
#define __FIRMWARE_FDT_H

#include <linux/errno.h>
#include <linux/sizes.h>
#include <linux/types.h>

/* Maximum size of both the firmware FIT and the assembled devicetree */
#define FIRMWARE_FDT_MAX_SIZE	SZ_4M

/**
 * struct firmware_fdt - an assembled, firmware-owned devicetree
 *
 * @fdt: pointer to the assembled devicetree in memory
 * @size: size of the assembled devicetree, in bytes
 * @name: owned FIT filename (for diagnostics)
 * @fit: internal: buffer holding the FIT
 * @fit_size: internal: size of the FIT, in bytes
 * @fdt_owned: internal: true if @fdt is a separate allocation
 *
 * All memory is owned by the helper: release it with firmware_fdt_free()
 * once the devicetree has been consumed (installed or copied).
 */
struct firmware_fdt {
	void *fdt;
	ulong size;
	char *name;
	void *fit;
	ulong fit_size;
	bool fdt_owned;
};

#if CONFIG_IS_ENABLED(FIRMWARE_FDT)
/**
 * firmware_fdt_load() - assemble the devicetree from a firmware partition
 *
 * Assemble the devicetree (the base DTB with its overlays applied, as
 * described by the FIT on the configured firmware partition) and
 * return it in @out, ready to hand to the OS.
 *
 * @out: returns the assembled devicetree on success
 * Return: 0 on success; -ENOENT if no source is configured (the caller may
 *	   fall back to its normal devicetree); another negative errno if a
 *	   configured source fails to assemble (the caller must fail, never
 *	   fall back)
 */
int firmware_fdt_load(struct firmware_fdt *out);

/**
 * firmware_fdt_free() - release the memory behind an assembled devicetree
 *
 * Safe to call on a zeroed or already-freed @fw.
 *
 * @fw: the assembled devicetree to release
 */
void firmware_fdt_free(struct firmware_fdt *fw);

/**
 * efi_stage_firmware_fdt() - stage a firmware-owned devicetree for EFI
 *
 * This is the common policy adapter for EFI consumers. It first checks
 * whether a source is configured, then assembles and copies its devicetree
 * to @fdt_addr. Only -ENOENT permits a caller to try another source.
 *
 * Callers must read ``fdt_addr_r`` once and pass that value as @fdt_addr.
 * The same value must be used for any fallback source, so an environment
 * change cannot make the two paths disagree.
 *
 * @fdt_addr: destination address, normally the caller's ``fdt_addr_r`` value
 * @fdt_sizep: returns the staged devicetree size
 * @namep: if non-NULL, returns an allocated copy of the FIT filename
 * Return: 0 if staged; -ENOENT if no source is configured; another negative
 *	   errno if a configured source cannot be staged
 */
int efi_stage_firmware_fdt(ulong fdt_addr, ulong *fdt_sizep, char **namep);
#else
static inline int firmware_fdt_load(struct firmware_fdt *out)
{
	return -ENOENT;
}

static inline void firmware_fdt_free(struct firmware_fdt *fw)
{
}

static inline int efi_stage_firmware_fdt(ulong fdt_addr, ulong *fdt_sizep,
					 char **namep)
{
	return -ENOENT;
}
#endif

#endif /* __FIRMWARE_FDT_H */
