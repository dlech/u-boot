// SPDX-License-Identifier: GPL-2.0+
/*
 * Tests for the firmware-owned devicetree source (firmware_fdt_load()).
 *
 * Uses a sandbox mmc image (mmc11) carrying a GPT 'firmware' partition with
 * a FAT filesystem holding the FIT (fdt.itb: a base DTB and one
 * overlay, with two configurations). The image is built by
 * setup_firmware_fdt_image() in test/py/tests/test_ut.py.
 *
 * The tests create the complete source topology at runtime, including the
 * mmc11 provider and its phandle. The shared test.dts stays unconfigured,
 * while the sandbox DM test framework restores its FDT snapshot after every
 * test, including a failed one.
 */

#include <dm.h>
#include <env.h>
#include <firmware_fdt.h>
#include <malloc.h>
#include <mapmem.h>
#include <os.h>
#include <asm/global_data.h>
#include <dm/lists.h>
#include <dm/ofnode.h>
#include <dm/root.h>
#include <linux/libfdt.h>
#include <test/test.h>
#include <test/ut.h>
#include "bootstd_common.h"

DECLARE_GLOBAL_DATA_PTR;

#define FWFDT_NODE_PATH		"/fw-fdt"
#define FWFDT_STORE_PROP	"firmware-fdt-store"
#define FWFDT_STORE_PHANDLE	0x10000
#define FWFDT_TYPE_UUID		"384e979b-eb76-435a-a3a6-1a071dbad91d"
#define FWFDT_TEST_FLAGS	(UTF_DM | UTF_SCAN_FDT | UTF_FLAT_TREE)

static ofnode fwfdt_source_node(void)
{
	return ofnode_path(FWFDT_NODE_PATH);
}

/* Bind the runtime-created mmc node that owns the firmware-FDT image */
static int fwfdt_bind_mmc(struct unit_test_state *uts)
{
	struct udevice *dev;
	ofnode node;

	node = ofnode_path("/mmc11");
	ut_assert(ofnode_valid(node));
	ut_assertok(lists_bind_fdt(gd->dm_root, node, &dev, NULL, false));

	return 0;
}

/*
 * Create the full firmware-FDT topology. Set @with_source to false for the
 * no-source test, which still needs the media device.
 */
static int fwfdt_configure(struct unit_test_state *uts, bool with_source)
{
	char fname[256];
	ofnode root, mmc, src;

	ut_assertok(os_persistent_file(fname, sizeof(fname), "mmc11.img"));
	root = oftree_root(oftree_default());
	ut_assertok(ofnode_add_subnode(root, "mmc11", &mmc));
	ut_assertok(ofnode_write_string(mmc, "compatible", "sandbox,mmc"));
	ut_assertok(ofnode_write_string(mmc, "filename", fname));
	ut_assertok(ofnode_write_u32(mmc, "phandle", FWFDT_STORE_PHANDLE));

	if (!with_source)
		return 0;

	ut_assertok(ofnode_add_subnode(root, "fw-fdt", &src));
	ut_assertok(ofnode_write_string(src, "compatible",
					"u-boot,firmware-fdt-block"));
	ut_assertok(ofnode_write_u32(src, FWFDT_STORE_PROP,
				     FWFDT_STORE_PHANDLE));
	ut_assertok(ofnode_write_string(src, "partition-type-uuid",
					FWFDT_TYPE_UUID));
	ut_assertok(ofnode_write_string(src, "partition-name", "firmware"));
	ut_assertok(ofnode_write_string(src, "filename", "fdt.itb"));

	return 0;
}

/* Clear the environment values used by the tests */
static int fwfdt_clear_env(struct unit_test_state *uts)
{
	env_set("fw_fdt_part", NULL);
	env_set("fw_fdt_config", NULL);

	return 0;
}

/* Happy path: the FIT's default configuration applies base + overlay */
static int firmware_fdt_test_load(struct unit_test_state *uts)
{
	struct firmware_fdt fw;
	void *fdt;

	ut_assertok(fwfdt_configure(uts, true));
	ut_assertok(fwfdt_bind_mmc(uts));

	ut_assertok(firmware_fdt_load(&fw));

	ut_asserteq_str("fdt.itb", fw.name);
	ut_assert(fw.size > 0);
	ut_assert(fw.fdt_owned);

	fdt = fw.fdt;
	ut_assertok(fdt_check_header(fdt));
	/* the size reports the packed devicetree, not a padded buffer */
	ut_asserteq(fw.size, fdt_totalsize(fdt));
	/* the base property is present... */
	ut_assertnonnull(fdt_getprop(fdt, 0, "fw-base-prop", NULL));
	/* ...and the overlay was applied on top */
	ut_assertnonnull(fdt_getprop(fdt, 0, "fw-overlay-prop", NULL));

	firmware_fdt_free(&fw);
	ut_assertnull(fw.fdt);

	ut_assertok(fwfdt_clear_env(uts));

	return 0;
}

BOOTSTD_TEST(firmware_fdt_test_load, FWFDT_TEST_FLAGS);

/* 'fw_fdt_config' selects another configuration the FIT ships */
static int firmware_fdt_test_select(struct unit_test_state *uts)
{
	struct firmware_fdt fw;
	void *fdt;

	ut_assertok(fwfdt_configure(uts, true));
	ut_assertok(fwfdt_bind_mmc(uts));

	ut_assertok(env_set("fw_fdt_config", "conf-base"));
	ut_assertok(firmware_fdt_load(&fw));

	fdt = fw.fdt;
	ut_assert(!fw.fdt_owned);
	ut_assertnonnull(fdt_getprop(fdt, 0, "fw-base-prop", NULL));
	/* the base-only configuration applies no overlay */
	ut_assertnull(fdt_getprop(fdt, 0, "fw-overlay-prop", NULL));

	firmware_fdt_free(&fw);

	ut_assertok(fwfdt_clear_env(uts));

	return 0;
}

BOOTSTD_TEST(firmware_fdt_test_select, FWFDT_TEST_FLAGS);

/*
 * Configuration chaining could assemble a combination which was never signed
 * as one unit. Only one FIT configuration may be selected.
 */
static int firmware_fdt_test_config_chain(struct unit_test_state *uts)
{
	struct firmware_fdt fw;

	ut_assertok(fwfdt_configure(uts, true));
	ut_assertok(fwfdt_bind_mmc(uts));

	ut_assertok(env_set("fw_fdt_config", "conf-base#conf-overlay"));
	ut_asserteq(-EINVAL, firmware_fdt_load(&fw));

	ut_assertok(fwfdt_clear_env(uts));

	return 0;
}

BOOTSTD_TEST(firmware_fdt_test_config_chain, FWFDT_TEST_FLAGS);

/* The shared EFI helper stages the result and owns its returned filename */
static int firmware_fdt_test_stage(struct unit_test_state *uts)
{
	char *name;
	ulong size;
	void *buf;

	ut_assertok(fwfdt_configure(uts, true));
	ut_assertok(fwfdt_bind_mmc(uts));

	buf = malloc(FIRMWARE_FDT_MAX_SIZE);
	ut_assertnonnull(buf);
	ut_assertok(efi_stage_firmware_fdt(map_to_sysmem(buf), &size, &name));
	ut_asserteq_str("fdt.itb", name);
	ut_asserteq(size, fdt_totalsize(buf));
	ut_assertnonnull(fdt_getprop(buf, 0, "fw-base-prop", NULL));
	ut_assertnonnull(fdt_getprop(buf, 0, "fw-overlay-prop", NULL));
	free(name);
	free(buf);

	ut_assertok(fwfdt_clear_env(uts));

	return 0;
}

BOOTSTD_TEST(firmware_fdt_test_stage, FWFDT_TEST_FLAGS);

/* Compatible best-match against the control DT selects conf-sandbox */
static int firmware_fdt_test_best_match(struct unit_test_state *uts)
{
	struct firmware_fdt fw;
	const char *value;
	ofnode node;

	ut_assertok(fwfdt_configure(uts, true));
	node = fwfdt_source_node();
	ut_assert(ofnode_valid(node));
	ut_assertok(ofnode_write_string(node, "filename", "fdt-best.itb"));
	ut_assertok(fwfdt_bind_mmc(uts));

	ut_assertok(firmware_fdt_load(&fw));
	value = fdt_getprop(fw.fdt, 0, "fw-best-prop", NULL);
	ut_assertnonnull(value);
	ut_asserteq_str("sandbox", value);
	firmware_fdt_free(&fw);

	ut_assertok(fwfdt_clear_env(uts));

	return 0;
}

BOOTSTD_TEST(firmware_fdt_test_best_match, FWFDT_TEST_FLAGS);

/* A corrupted base fails hash verification and cannot fall back */
static int firmware_fdt_test_corrupt(struct unit_test_state *uts)
{
	struct firmware_fdt fw;
	ofnode node;

	ut_assertok(fwfdt_configure(uts, true));
	node = fwfdt_source_node();
	ut_assert(ofnode_valid(node));
	ut_assertok(ofnode_write_string(node, "filename", "fdt-corrupt.itb"));
	ut_assertok(fwfdt_bind_mmc(uts));

	ut_asserteq(-EACCES, firmware_fdt_load(&fw));

	ut_assertok(fwfdt_clear_env(uts));

	return 0;
}

BOOTSTD_TEST(firmware_fdt_test_corrupt, FWFDT_TEST_FLAGS);

/* A corrupted overlay is fatal too; it must never be silently skipped */
static int firmware_fdt_test_corrupt_overlay(struct unit_test_state *uts)
{
	struct firmware_fdt fw;
	ofnode node;

	ut_assertok(fwfdt_configure(uts, true));
	node = fwfdt_source_node();
	ut_assert(ofnode_valid(node));
	ut_assertok(ofnode_write_string(node, "filename",
					"fdt-corrupt-overlay.itb"));
	ut_assertok(fwfdt_bind_mmc(uts));

	ut_asserteq(-EACCES, firmware_fdt_load(&fw));

	ut_assertok(fwfdt_clear_env(uts));

	return 0;
}

BOOTSTD_TEST(firmware_fdt_test_corrupt_overlay, FWFDT_TEST_FLAGS);

/*
 * A selector naming a configuration the FIT does not ship must be
 * fatal: -ENOENT strictly means "no source configured", so the inner miss
 * must not leak out and let the caller fall back (fail closed).
 */
static int firmware_fdt_test_bad_config(struct unit_test_state *uts)
{
	struct firmware_fdt fw;

	ut_assertok(fwfdt_configure(uts, true));
	ut_assertok(fwfdt_bind_mmc(uts));

	ut_assertok(env_set("fw_fdt_config", "conf-nonexistent"));
	ut_asserteq(-ENODEV, firmware_fdt_load(&fw));

	ut_assertok(fwfdt_clear_env(uts));

	return 0;
}

BOOTSTD_TEST(firmware_fdt_test_bad_config, FWFDT_TEST_FLAGS);

/* Pinning a partition that does not exist is a hard error (fail closed) */
static int firmware_fdt_test_no_part(struct unit_test_state *uts)
{
	struct firmware_fdt fw;

	ut_assertok(fwfdt_configure(uts, true));
	ut_assertok(fwfdt_bind_mmc(uts));

	ut_assertok(env_set("fw_fdt_part", "9"));
	ut_asserteq(-ENODEV, firmware_fdt_load(&fw));

	ut_assertok(fwfdt_clear_env(uts));

	return 0;
}

BOOTSTD_TEST(firmware_fdt_test_no_part, FWFDT_TEST_FLAGS);

/*
 * Without a source node, -ENOENT is the only result which lets callers fall
 * back. This also proves the provider alone does not configure the feature.
 */
static int firmware_fdt_test_no_source(struct unit_test_state *uts)
{
	struct firmware_fdt fw;

	ut_assertok(fwfdt_configure(uts, false));
	ut_assertok(fwfdt_bind_mmc(uts));

	ut_asserteq(-ENOENT, firmware_fdt_load(&fw));
	/* Source detection precedes address validation in the EFI helper */
	ut_asserteq(-ENOENT, efi_stage_firmware_fdt(0, NULL, NULL));

	return 0;
}

BOOTSTD_TEST(firmware_fdt_test_no_source, FWFDT_TEST_FLAGS);

/*
 * A 'firmware-fdt-store' phandle that does not resolve is a broken
 * configuration and must be fatal, not mistaken for "no source".
 */
static int firmware_fdt_test_bad_source(struct unit_test_state *uts)
{
	struct firmware_fdt fw;
	fdt32_t bad;
	ofnode node;

	ut_assertok(fwfdt_configure(uts, true));
	node = fwfdt_source_node();
	ut_assert(ofnode_valid(node));

	bad = cpu_to_fdt32(0x7fffffff);
	ut_assertok(ofnode_write_prop(node, FWFDT_STORE_PROP, &bad,
				      sizeof(bad), true));
	ut_asserteq(-EINVAL, firmware_fdt_load(&fw));

	ut_assertok(fwfdt_clear_env(uts));

	return 0;
}

BOOTSTD_TEST(firmware_fdt_test_bad_source, FWFDT_TEST_FLAGS);

/* A FIT using external data is refused: it must be self-contained */
static int firmware_fdt_test_external(struct unit_test_state *uts)
{
	struct firmware_fdt fw;
	ofnode node;

	ut_assertok(fwfdt_configure(uts, true));

	node = fwfdt_source_node();
	ut_assert(ofnode_valid(node));
	ut_assertok(ofnode_write_string(node, "filename", "fdt-ext.itb"));

	ut_assertok(fwfdt_bind_mmc(uts));

	ut_asserteq(-EINVAL, firmware_fdt_load(&fw));

	ut_assertok(fwfdt_clear_env(uts));

	return 0;
}

BOOTSTD_TEST(firmware_fdt_test_external, FWFDT_TEST_FLAGS);

/*
 * When both partition-type-uuid and partition-name are configured, both
 * must match: a name matching nothing must not fall back to whichever
 * same-type (A/B) partition comes first.
 */
static int firmware_fdt_test_part_mismatch(struct unit_test_state *uts)
{
	struct firmware_fdt fw;
	ofnode node;

	ut_assertok(fwfdt_configure(uts, true));

	node = fwfdt_source_node();
	ut_assert(ofnode_valid(node));
	/* the type UUID matches both A/B partitions; this name matches none */
	ut_assertok(ofnode_write_string(node, "partition-name", "nomatch"));

	ut_assertok(fwfdt_bind_mmc(uts));

	ut_asserteq(-ENODEV, firmware_fdt_load(&fw));

	ut_assertok(fwfdt_clear_env(uts));

	return 0;
}

BOOTSTD_TEST(firmware_fdt_test_part_mismatch, FWFDT_TEST_FLAGS);
