.. SPDX-License-Identifier: GPL-2.0+

Firmware-owned devicetree
=========================

Some platforms following EBBR / Arm SystemReady IR treat the devicetree as
part of the firmware: it lives on a firmware-owned partition and is updated
independently of the operating system, instead of being shipped in the OS
image or on the EFI System Partition. U-Boot must read that devicetree,
assemble it (base plus overlays) and hand it to the OS.

The :c:func:`firmware_fdt_load` helper (``CONFIG_FIRMWARE_FDT``) provides a
consumer-facing interface independently of standard boot. The source
compatible identifies the storage backend. The first implemented backend,
``u-boot,firmware-fdt-block``, reads the FIT from a filesystem on a GPT
partition of a block device.

Additional source backends may load the same FIT from other firmware storage,
such as UBI on MTD. They reuse the common FIT configuration selection,
verification and assembly, as well as the consumers below.

The first consumers are the two EFI launch paths, so the firmware-owned
devicetree is installed regardless of how the EFI application is started:

  - the per-device EFI bootmeth (``bootmeth_efi``), and
  - the EFI boot manager (``efi_bootmgr_run()``).

In each case the assembled devicetree is installed into the EFI configuration
table via :c:func:`efi_install_fdt`, exactly like any other source, so the OS
cannot tell where it came from.

This replaces vendor-specific firmware-devicetree commands while keeping
storage discovery behind the source backend.

The FIT
-------

The firmware partition carries a FIT (by default ``fdt.itb``). Its images
hold the base DTB and any overlays, and each of its configurations names one
bootable combination through the standard ``fdt`` property::

    configurations {
        default = "conf-panel";
        conf-panel {
            fdt = "fdt-base", "fdt-panel";
        };
    };

The helper selects a configuration, verifies it, loads the base devicetree
and applies the listed overlays in order (via :c:func:`boot_get_fdt_fit`, the
same code path ``bootm`` uses). The FIT describes and carries the devicetree
as one artefact, updated atomically with it.

Images in the FIT must be self-contained flat devicetrees: images that carry
a ``load`` address and FITs using external data are rejected. With
``FIT_SIGNATURE`` enabled, node and configuration names must not contain
``@``.

Configuration
-------------

The FIT source is described in the control devicetree (see
``doc/device-tree-bindings/firmware-fdt.txt``). Each source compatible
defines one storage backend and its locator properties. The currently
implemented ``u-boot,firmware-fdt-block`` backend points at the media device
through the ``firmware-fdt-store`` phandle and identifies a GPT partition by
type UUID and/or name, with an optional ``filename`` for the FIT path. Its
store phandle follows the FWU metadata (``u-boot,fwu-mdata-*``) pattern.

A future backend may use different locator properties, for example an MTD
device and UBI volume, while preserving the same FIT contents and the common
selection, verification, assembly and fail-closed behavior.

Two optional environment variables select among what the FIT ships:
``fw_fdt_part`` pins a partition number (A/B firmware partitions) and
``fw_fdt_config`` names the FIT configuration to use. Without an explicit
configuration, compatible best-match against the control devicetree is used;
if there is no match, the FIT's ``default`` configuration is used. The
``fw_fdt_config`` value must name one configuration; configuration chaining
with ``#`` is rejected so the selected base, overlay set and ordering remain
one authenticated unit.

If no source is configured, the helper returns ``-ENOENT`` and the caller
falls back to its normal devicetree source (ESP / built-in control FDT). If a
source is configured but cannot be assembled, the error is fatal for that EFI
launch path; this prevents a bad or unauthenticated firmware devicetree from
being silently replaced by another devicetree source.

A firmware-owned devicetree is the complete, authoritative devicetree: no
other devicetree source is layered on top of it. In particular,
extension-board overlays (``extension_scan()``) are intentionally not
applied, since modifying the assembled (and, in secure mode, signed)
devicetree would defeat the authenticated-combination model. Boards using
extension boards should ship each supported combination as a FIT
configuration and select it with ``fw_fdt_config``.

Secure boot
-----------

Signing a FIT configuration authenticates the whole combination: the base,
the overlay set and its ordering are the signed unit, and a tampered selector
can only pick among combinations the firmware author pre-signed. Sign the
FIT and inject the public key into U-Boot's control devicetree as
usual::

    mkimage -f fdt.its -k keys -K u-boot.dtb -r fdt.itb

With ``CONFIG_FIT_SIGNATURE`` enabled and a required key in the control
devicetree, verification is enforced by the standard verified-boot policy:
an unsigned or tampered FIT is rejected and, because a configured source
never falls back, the boot fails closed rather than booting an unverified
devicetree.
