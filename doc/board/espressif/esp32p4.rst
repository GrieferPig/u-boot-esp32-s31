.. SPDX-License-Identifier: GPL-2.0+

Espressif ESP32-P4 revision 1.3
=============================

The ROM loads the SPL from SPI NOR at offset ``0x2000``. The SPL brings up
cached PSRAM and validates the fixed-position FIT. U-Boot proper runs from HP
SRAM in RISC-V M-mode. Standard Boot selects the mapped NOR boot method, which
prepares the FIT's Linux device tree in PSRAM and enters the kernel in place.

Build the stages in separate output directories with an RV32 Espressif GNU
toolchain:

.. code-block:: bash

   export CROSS_COMPILE=riscv32-esp-elf-
   make O=out/spl ARCH=riscv espressif_esp32p4_spl_defconfig
   make O=out/spl ARCH=riscv
   make O=out/proper ARCH=riscv espressif_esp32p4_sram_defconfig
   make O=out/proper ARCH=riscv

In the integration repository, ``make p4-fit TARGET=esp32p4`` wraps the SPL
for the ROM and assembles the images. Supply the P4 Linux XIP Image, DTB and
SquashFS with ``P4_LINUX_IMAGE``, ``P4_LINUX_DTB`` and ``P4_ROOTFS_IMAGE``.
The integration build selects 200 MHz PSRAM for the Waveshare display board.
The unmodified SPL defconfig uses 80 MHz.

The fixed FIT offsets are relative to the start of SPI NOR.

.. list-table::
   :header-rows: 1

   * - Offset
     - Payload
   * - ``0x00002000``
     - ESP ROM image containing U-Boot SPL
   * - ``0x000f0000``
     - FIT manifest
   * - ``0x00100000``
     - U-Boot proper
   * - ``0x00200000``
     - Linux XIP Image
   * - ``0x00b00000``
     - Linux device tree
   * - ``0x00c00000``
     - Root filesystem

On the U-Boot serial console, interrupt autoboot and run ``bootflow scan``
to boot the mapped FIT. The application FIT uses SHA-256 integrity checks.
The development SPL refuses a chip with secure boot or flash encryption enabled.
