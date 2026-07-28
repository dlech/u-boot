// SPDX-License-Identifier: GPL-2.0+
/*
 * Firmware-owned devicetree source.
 *
 * Load a firmware-owned FIT, select and verify one configuration, and
 * assemble its base devicetree and overlays for consumers.
 */

#define LOG_CATEGORY LOGC_BOOT

#include <blk.h>
#include <dm.h>
#include <env.h>
#include <firmware_fdt.h>
#include <fs.h>
#include <image.h>
#include <log.h>
#include <malloc.h>
#include <mapmem.h>
#include <part.h>
#include <vsprintf.h>
#include <dm/ofnode.h>
#include <linux/libfdt.h>
#include <linux/string.h>

/* The FIT lives in a filesystem on a GPT partition of a block device */
#define FW_FDT_COMPAT_BLOCK	"u-boot,firmware-fdt-block"

/* Default FIT filename on the firmware partition */
#define FW_FDT_FILENAME		"fdt.itb"

/**
 * fw_fdt_get_source() - find the configured firmware-FDT source node
 *
 * Scans the control devicetree for an enabled node with the
 * "u-boot,firmware-fdt-block" compatible. Disabled nodes are skipped, so a
 * devicetree may ship the node with status "disabled" and a variant (or a
 * test) enable it.
 *
 * @srcp: returns the source ofnode on success
 * Return: 0 on success, -ENOENT if no source is configured
 */
static int fw_fdt_get_source(ofnode *srcp)
{
	ofnode node;

	node = ofnode_by_compatible(ofnode_null(), FW_FDT_COMPAT_BLOCK);
	while (ofnode_valid(node) && !ofnode_is_enabled(node))
		node = ofnode_by_compatible(node, FW_FDT_COMPAT_BLOCK);

	if (!ofnode_valid(node))
		return -ENOENT;

	*srcp = node;

	return 0;
}

/**
 * fw_fdt_get_blk() - resolve the block device holding the FIT
 *
 * The source node points at its media device (e.g. &mmc0) through the
 * 'firmware-fdt-store' phandle. The lookup also probes the device, which
 * the EFI boot-manager path relies on.
 *
 * @src: the firmware-FDT source node
 * @descp: returns the block descriptor on success
 * Return: 0 on success, -EINVAL if 'firmware-fdt-store' is missing or its
 * phandle does not resolve, other negative on error
 */
static int fw_fdt_get_blk(ofnode src, struct blk_desc **descp)
{
	struct udevice *media, *blk;
	ofnode store;
	int ret;

	store = ofnode_parse_phandle(src, "firmware-fdt-store", 0);
	if (!ofnode_valid(store))
		return log_msg_ret("store", -EINVAL);

	/* a source is configured: remap -ENOENT to -ENODEV to fail closed */
	ret = device_get_global_by_ofnode(store, &media);
	if (ret)
		return log_msg_ret("media", ret == -ENOENT ? -ENODEV : ret);

	ret = blk_get_from_parent(media, &blk);
	if (ret)
		return log_msg_ret("blk", ret == -ENOENT ? -ENODEV : ret);

	*descp = dev_get_uclass_plat(blk);

	return 0;
}

/**
 * fw_fdt_find_part() - find the firmware partition on @desc
 *
 * 'fw_fdt_part', if set, pins an explicit partition number. Otherwise the
 * first partition matching every configured selector is used: when both
 * @type_uuid and @name are configured, both must match, so a misprovisioned
 * disk fails instead of silently selecting whichever same-type (e.g. A/B)
 * partition comes first. At least one selector must be configured.
 *
 * @desc: block device to scan
 * @type_uuid: GPT type UUID to match, or NULL
 * @name: partition name to match, or NULL
 * Return: partition number (>= 1), -EINVAL if 'fw_fdt_part' is set to an
 * invalid partition number or no selector is configured, or -ENODEV if no
 * partition matched
 */
static int fw_fdt_find_part(struct blk_desc *desc, const char *type_uuid,
			    const char *name)
{
	const char *sel;
	struct disk_partition info;
	bool want_type = type_uuid && *type_uuid;
	bool want_name = name && *name;
	int p;

	sel = env_get("fw_fdt_part");
	if (sel && *sel) {
		char *end;
		ulong pin;

		pin = dectoul(sel, &end);
		if (*end || !pin || pin > MAX_SEARCH_PARTITIONS)
			return log_msg_ret("pin", -EINVAL);

		if (part_get_info(desc, pin, &info))
			return log_msg_ret("pin", -ENODEV);

		return pin;
	}

	if (!want_type && !want_name)
		return log_msg_ret("sel", -EINVAL);

	for (p = 1; p <= MAX_SEARCH_PARTITIONS; p++) {
		bool type_match, name_match;

		if (part_get_info(desc, p, &info))
			continue;

		/* UUID text is case-insensitive (RFC 4122) */
		type_match = !want_type ||
			!strncasecmp(disk_partition_type_guid(&info), type_uuid,
				     UUID_STR_LEN);
		name_match = !want_name ||
			!strcmp((const char *)info.name, name);

		if (type_match && name_match)
			return p;
	}

	return -ENODEV;
}

/**
 * fw_fdt_read_fit() - read the FIT into an allocated buffer
 *
 * @desc: block device holding the firmware partition
 * @part: partition number
 * @fname: FIT filename
 * @bufp: returns the allocated buffer holding the FIT
 * @sizep: returns the FIT size in bytes
 * Return: 0 on success, negative on error
 */
static int fw_fdt_read_fit(struct blk_desc *desc, int part, const char *fname,
			   void **bufp, ulong *sizep)
{
	loff_t size;
	int ret;

	ret = fs_set_blk_dev_with_part(desc, part);
	if (ret)
		return log_msg_ret("fs", -EIO);

	ret = fs_size(fname, &size);
	if (ret)
		return log_msg_ret("size", -EIO);

	if (!size || size > FIRMWARE_FDT_MAX_SIZE)
		return log_msg_ret("big", -E2BIG);

	/* fs_size() consumed the mount */
	ret = fs_set_blk_dev_with_part(desc, part);
	if (ret)
		return log_msg_ret("fs2", -EIO);

	ret = fs_read_alloc(fname, size, 0, bufp);
	if (ret)
		return log_msg_ret("read", ret);

	*sizep = size;

	return 0;
}

/**
 * fw_fdt_check_images() - reject FITs that are not self-contained
 *
 * @fit: the FIT
 * Return: 0 if every image is embedded and has no load address, -EINVAL
 * otherwise
 */
static int fw_fdt_check_images(const void *fit)
{
	int images, node;

	images = fdt_path_offset(fit, FIT_IMAGES_PATH);
	if (images < 0)
		return log_msg_ret("img", -EINVAL);

	fdt_for_each_subnode(node, fit, images) {
		if (!fit_image_check_type(fit, node, IH_TYPE_FLATDT))
			return log_msg_ret("type", -EINVAL);

		if (fdt_getprop(fit, node, FIT_LOAD_PROP, NULL))
			return log_msg_ret("load", -EINVAL);

		if (fdt_getprop(fit, node, FIT_DATA_OFFSET_PROP, NULL) ||
		    fdt_getprop(fit, node, FIT_DATA_POSITION_PROP, NULL))
			return log_msg_ret("ext", -EINVAL);
	}

	return 0;
}

/**
 * fw_fdt_assemble() - load the FIT and assemble the devicetree
 *
 * @out: returns the assembled devicetree and its backing buffers
 * @src: the (validated) firmware-FDT source node
 * Return: 0 on success, negative on error
 */
static int fw_fdt_assemble(struct firmware_fdt *out, ofnode src)
{
	struct bootm_headers images;
	const char *type_uuid, *part_name, *fname, *conf;
	struct blk_desc *desc;
	ulong data, len;
	void *fdt;
	int part, ret;

	memset(&images, '\0', sizeof(images));
	images.verify = 1;

	fname = ofnode_read_string(src, "filename");
	if (!fname)
		fname = FW_FDT_FILENAME;

	type_uuid = ofnode_read_string(src, "partition-type-uuid");
	part_name = ofnode_read_string(src, "partition-name");

	ret = fw_fdt_get_blk(src, &desc);
	if (ret)
		return ret;

	part = fw_fdt_find_part(desc, type_uuid, part_name);
	if (part < 0)
		return log_msg_ret("part", part);

	ret = fw_fdt_read_fit(desc, part, fname, &out->fit, &out->fit_size);
	if (ret)
		return ret;

	ret = fit_check_format(out->fit, out->fit_size);
	if (ret)
		return log_msg_ret("fit", -EINVAL);

	ret = fw_fdt_check_images(out->fit);
	if (ret)
		return ret;

	/*
	 * boot_get_fdt_fit() verifies the base image and the selected
	 * configuration, but historically skips an overlay which fails to
	 * load. Verify every image up front so a bad overlay cannot silently
	 * turn a signed base-plus-overlay configuration into the base alone.
	 */
	if (!fit_all_image_verify(out->fit))
		return log_msg_ret("verify", -EACCES);

	conf = env_get("fw_fdt_config");
	if (conf && !*conf)
		conf = NULL;
	/*
	 * boot_get_fdt_fit() accepts '#' to compose several configurations.
	 * A firmware-owned devicetree must use one configuration so its base,
	 * overlay set and ordering remain one authenticated unit.
	 */
	if (conf && strchr(conf, '#'))
		return log_msg_ret("chain", -EINVAL);

	ret = boot_get_fdt_fit(&images, map_to_sysmem(out->fit), NULL, &conf,
			       IH_ARCH_DEFAULT, &data, &len,
			       &out->fdt_owned);
	if (ret < 0)
		return log_msg_ret("conf", ret);

	fdt = map_sysmem(data, len);

	out->fdt = fdt;
	out->size = len;

	if (len > FIRMWARE_FDT_MAX_SIZE)
		return log_msg_ret("bigfdt", -E2BIG);

	ret = fdt_check_full(fdt, len);
	if (ret)
		return log_msg_ret("chk", -EINVAL);

	out->name = strdup(fname);
	if (!out->name)
		return log_msg_ret("name", -ENOMEM);

	return 0;
}

static int fw_fdt_load_source(struct firmware_fdt *out, ofnode src)
{
	int ret;

	memset(out, '\0', sizeof(*out));

	ret = fw_fdt_assemble(out, src);
	if (ret) {
		firmware_fdt_free(out);

		/*
		 * Callers treat -ENOENT as "no source configured" and fall
		 * back to their normal devicetree. A source IS configured
		 * here, so remap any downstream -ENOENT (missing FIT,
		 * missing FIT configuration, ...) to -ENODEV to keep the
		 * failure fatal (fail closed).
		 */
		if (ret == -ENOENT)
			ret = -ENODEV;
	}

	return ret;
}

int firmware_fdt_load(struct firmware_fdt *out)
{
	ofnode src;
	int ret;

	memset(out, '\0', sizeof(*out));

	ret = fw_fdt_get_source(&src);
	if (ret)
		return ret;

	return fw_fdt_load_source(out, src);
}

int efi_stage_firmware_fdt(ulong fdt_addr, ulong *fdt_sizep, char **namep)
{
	struct firmware_fdt fw;
	ofnode src;
	char *name = NULL;
	int ret;

	if (namep)
		*namep = NULL;

	/*
	 * Check for a configured source before validating the staging address:
	 * an absent source must remain -ENOENT even on systems which do not
	 * provide fdt_addr_r.
	 */
	ret = fw_fdt_get_source(&src);
	if (ret)
		return ret;

	if (!fdt_addr)
		return log_msg_ret("addr", -EINVAL);

	ret = fw_fdt_load_source(&fw, src);
	if (ret) {
		log_err("Failed to assemble the firmware devicetree (err %d)\n",
			ret);
		return ret;
	}

	if (fw.size > FIRMWARE_FDT_MAX_SIZE) {
		ret = -E2BIG;
		goto out;
	}

	if (namep) {
		name = strdup(fw.name);
		if (!name) {
			ret = -ENOMEM;
			goto out;
		}
	}

	memcpy(map_sysmem(fdt_addr, fw.size), fw.fdt, fw.size);
	*fdt_sizep = fw.size;
	if (namep)
		*namep = name;
	log_debug("Using firmware-owned devicetree\n");

out:
	firmware_fdt_free(&fw);

	return ret;
}

void firmware_fdt_free(struct firmware_fdt *fw)
{
	if (fw->fdt_owned)
		free(fw->fdt);

	free(fw->name);
	free(fw->fit);
	memset(fw, '\0', sizeof(*fw));
}
