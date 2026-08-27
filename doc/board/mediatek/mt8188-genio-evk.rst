.. SPDX-License-Identifier: GPL-2.0+
.. Copyright (C) 2026 Baylibre SAS

MediaTek Genio 510 and Genio 700 EVKs
======================================

The Genio 510 and Genio 700 EVKs use the same MT8188-family boot flow:

================  ======  ====================================
Board             SoC     Defconfig
================  ======  ====================================
Genio 510 EVK     MT8370  ``mt8370_genio_510_evk_defconfig``
Genio 700 EVK     MT8390  ``mt8390_genio_700_evk_defconfig``
================  ======  ====================================

The configurations share their SPL addresses, size limits, drivers and
firmware-image layout. Each board retains its own control devicetree, DRAM
size and external platform firmware.

Boot chain
----------

The normal eMMC boot chain is::

   BootROM
     -> platform DDR loader
     -> U-Boot SPL
     -> Arm Trusted Firmware-A (BL31)
     -> OP-TEE (BL32)
     -> U-Boot proper (BL33)

The BootROM loads a MediaTek image from the eMMC boot0 hardware partition at
``CONFIG_MTK_GENIO_BROM_LOAD_ADDR``. This image contains a platform DDR loader
followed by U-Boot SPL. The DDR loader initializes DRAM, copies the fixed
``CONFIG_SPL_MAX_SIZE`` byte SPL region from offset
``CONFIG_MTK_GENIO_DDR_LOADER_SIZE`` to ``CONFIG_SPL_TEXT_BASE`` and enters
SPL at EL3 with exceptions masked.

SPL reads a FIT image from partition 1 of the eMMC user area. The FIT contains
Arm Trusted Firmware-A (BL31), OP-TEE (BL32), U-Boot proper (BL33) and the
U-Boot control devicetree. It is an ordinary, directly parseable FIT whose
first word is the FDT magic; the complete FIT container must not be compressed.
Individual FIT payloads may use compression supported by SPL. SPL uses the
standard FIT and Arm Trusted Firmware support to hand off to BL31.

The DDR loader is a platform firmware component built separately from U-Boot
and supplied to binman as an external blob.

DDR-loader handoff contract
---------------------------

The DDR loader and U-Boot SPL have no parameter-block or firmware-call
interface. Their contract consists of a fixed image layout, initialized DRAM
and the execution state at the SPL entry point. Different DDR-loader
implementations may be used as long as they satisfy this contract for the
selected board. The BootROM loads the eMMC boot0 payload at
``CONFIG_MTK_GENIO_BROM_LOAD_ADDR``::

   offset 0                                     +----------------------+
                                                | DDR loader           |
                                                |                      |
   offset CONFIG_MTK_GENIO_DDR_LOADER_SIZE      +----------------------+
                                                | U-Boot SPL           |
                                                | CONFIG_SPL_MAX_SIZE  |
   image end                                    +----------------------+

The DDR loader then performs the following handoff::

   initialize and train DRAM

   copy:
     source      = CONFIG_MTK_GENIO_BROM_LOAD_ADDR
                   + CONFIG_MTK_GENIO_DDR_LOADER_SIZE
     destination = CONFIG_SPL_TEXT_BASE
     size        = CONFIG_SPL_MAX_SIZE

   enter:
     PC          = CONFIG_SPL_TEXT_BASE
     state       = AArch64 EL3h
     exceptions  = masked
     x0..x7      = 0

The loader must copy the complete fixed-size SPL window, make the copied image
coherent, leave the MMU and caches disabled and enter SPL with no live
dependency on its own runtime state. The MMU and caches must be disabled at
entry, and all writes performed by the loader must be visible to the CPU.
SPL does not consume DRAM geometry, a devicetree or boot-source information
from registers.

SPL obtains platform information from its embedded control devicetree and
initializes the console, clocks, pinctrl, watchdog and eMMC through U-Boot
drivers.

There is no runtime negotiation of these values. A replacement DDR loader is
compatible only if it initializes the selected board's DRAM and follows the
same image-layout and entry-state contract. It may represent the corresponding
addresses, offsets and sizes using any implementation-specific mechanism; the
contract does not require particular variable or build-symbol names.

Firmware image prerequisites
----------------------------

The following external binaries are required to assemble the complete
firmware images:

``ddr-loader.bin``
   A MediaTek DDR loader built for the target board and configured to satisfy
   the handoff contract above. The input may be shorter than the configured
   loader region; binman pads it with zeroes to the offset reserved before
   SPL.

``BL31``
   The U-Boot build variable naming the Arm Trusted Firmware-A BL31 binary
   built for the MT8188 platform. It is loaded and entered at
   ``CONFIG_MTK_GENIO_BL31_LOAD_ADDR``.

``TEE``
   The U-Boot build variable naming the OP-TEE binary. A standard OP-TEE v1
   ``tee.bin`` image is supported. Binman removes its header and derives the
   load and entry addresses from it when creating the FIT.

These binaries must match the selected board, its memory layout and its
firmware security policy. In particular, a common image layout does not make
DDR-loader or OP-TEE binaries interchangeable between the two boards. Place
``ddr-loader.bin`` in a directory which can be passed to binman with
``BINMAN_INDIRS``.

Building
--------

Select the defconfig for the target board. For example, for Genio 700::

   $ export CROSS_COMPILE=aarch64-linux-gnu-
   $ export KBUILD_OUTPUT=build
   $ make mt8390_genio_700_evk_defconfig
   $ make

Use ``mt8370_genio_510_evk_defconfig`` instead for Genio 510.

The default configuration builds U-Boot proper and SPL without requiring
external firmware.

This produces ``build/u-boot.bin`` and ``build/spl/u-boot-spl.bin`` without
building ``mtk-boot.bin`` or ``bootloaders.img``. The DDR loader and BL31 are
not needed in this case.

Complete firmware images
~~~~~~~~~~~~~~~~~~~~~~~~

Enable ``CONFIG_MTK_GENIO_BOOT_IMAGES`` to assemble the complete firmware
images.
For example, with the DDR loader in
``/path/to/firmware/ddr-loader.bin``::

   $ export BL31=/path/to/bl31.bin
   $ export TEE=/path/to/tee.bin
   $ make mt8390_genio_700_evk_defconfig
   $ scripts/config --file "${KBUILD_OUTPUT}/.config" \
         --enable MTK_GENIO_BOOT_IMAGES
   $ make olddefconfig
   $ make BINMAN_INDIRS=/path/to/firmware

``TEE`` must be set for both configuration and compilation. U-Boot uses its
presence during configuration to enable the support needed to preserve the
OP-TEE reserved-memory nodes in the devicetree passed to the operating system.
Do not deploy images if binman reports that it used fake or missing external
blobs.

Binman produces two deployable images and one intermediate payload in the
build directory:

``mtk-boot.bin``
   A MediaTek eMMC image with load and entry address
   ``CONFIG_MTK_GENIO_BROM_LOAD_ADDR``. It contains the DDR loader, padded to
   ``CONFIG_MTK_GENIO_DDR_LOADER_SIZE``, followed by U-Boot SPL padded to
   ``CONFIG_SPL_MAX_SIZE``. The fixed regions make every byte copied by the
   external loader part of the BootROM-loaded image.

``mtk-boot-payload.bin``
   The raw DDR-loader and SPL payload contained in ``mtk-boot.bin``, without
   the MediaTek BootROM header. This is an intermediate input for external
   MediaTek secure-boot tooling and must not be flashed directly.

``bootloaders.img``
   A FIT image containing BL31, U-Boot proper, OP-TEE and the U-Boot control
   devicetree. Each component has a SHA-256 hash.

Binman also creates ``mtk-boot.map`` and ``bootloaders.map``. These show the
offset and size of every component. The FIT can be inspected with::

   $ dumpimage -l build/bootloaders.img

Signed bootloader FIT
~~~~~~~~~~~~~~~~~~~~~

The default ``bootloaders.img`` contains hashes but is not authenticated.
Enable ``CONFIG_SPL_FIT_SIGNATURE`` and its RSA/SHA-256 dependencies to sign
the FIT configuration and require verification by SPL. Binman expects an
RSA-3072 private key and certificate named ``bootloaders.key`` and
``bootloaders.crt`` in one of the directories passed through
``BINMAN_INDIRS``.

For example, after enabling ``CONFIG_MTK_GENIO_BOOT_IMAGES`` as above::

   $ scripts/config --file "${KBUILD_OUTPUT}/.config" \
         --enable FIT_SIGNATURE \
         --enable RSA \
         --enable RSA_VERIFY \
         --enable SHA256 \
         --enable SPL_FIT_SIGNATURE \
         --enable SPL_RSA \
         --enable SPL_RSA_VERIFY \
         --enable SPL_SHA256
   $ make olddefconfig
   $ make BINMAN_INDIRS=/path/to/firmware-and-keys

The configuration signature covers BL31, U-Boot proper, OP-TEE and the U-Boot
control devicetree. Binman injects the corresponding public key into the SPL
control devicetree and marks it as required for FIT configurations.

This signs the firmware FIT loaded by SPL, not the BootROM payload. To build a
BootROM-authenticated image, pass ``mtk-boot-payload.bin`` to the platform
secure-boot tooling and let that tooling create the final MediaTek wrapper.
Key provisioning, rollback protection and BootROM authentication policy are
outside U-Boot.

Installing
----------

Use the board provisioning tools to place the images as follows:

===================  ================================================
Image                Destination
===================  ================================================
``mtk-boot.bin``     Start of the eMMC boot0 hardware partition
``bootloaders.img``  GPT partition 1 in the eMMC user area
===================  ================================================

The standard Genio 510 and Genio 700 partition layouts name GPT partition 1
``bootloaders``. SPL selects the partition by number through
``CONFIG_SYS_MMCSD_RAW_MODE_U_BOOT_PARTITION``; it does not locate it by name.

Writing an invalid image to eMMC boot0 can make the board unbootable. Preserve
the platform recovery path and any backup bootloader partition while testing.
Selection of a backup partition is not implemented by this SPL configuration.

Using Genio Tools
~~~~~~~~~~~~~~~~~

Install `Genio Tools
<https://genio.mediatek.com/doc/iot-yocto/latest/tools/genio-tools.html>`_ and
obtain a `prebuilt Genio image
<https://genio.mediatek.com/doc/iot-yocto/latest/sw/yocto/download.html>`_.
The image must be compatible with the target board. ``genio-flash`` needs its
extracted directory for partition metadata and the download bootstrap. To
update only the primary boot chain while retaining the backup bootloader
partition, run::

   $ genio-flash -P /path/to/genio-image \
         mmc0boot0:/absolute/path/to/build/mtk-boot.bin \
         bootloaders:/absolute/path/to/build/bootloaders.img

Genio Tools uses the default bootstrap from the image directory. Keep
``bootloaders_b`` unchanged until the new images have been tested. Add
``--dry-run`` to check the selected files and partitions without accessing the
board.
