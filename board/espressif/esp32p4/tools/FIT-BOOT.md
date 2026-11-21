# ESP32-P4 rev1.3 NOR XIP boot

The ROM loads U-Boot SPL into internal SRAM. SPL initializes NOR and
PSRAM, verifies the fixed FIT payloads with SHA-256, then loads U-Boot
proper into internal SRAM. U-Boot proper starts M-mode Linux at
0x40200000. Linux executes its text from NOR and initializes its writable
data and BSS in PSRAM. Linux starts hart 1, manages cache operations,
and initializes the Waveshare display and Ethernet DMA.

## Build

Use the project's ESP32-P4 toolchain, Buildroot dependencies and esptool.

    make image TARGET=esp32p4

The build writes a full 16 MiB flash image at images/fit-80/flash.bin
under the output directory. Its manifest gives the payload offsets,
lengths and hashes for flashing individual slots.

| SPI NOR offset | Payload |
| --- | --- |
| 0x002000 | ROM-loadable SPL |
| 0x0f0000 | FIT header |
| 0x100000 | U-Boot proper |
| 0x200000 | Linux xipImage |
| 0xb00000 | Linux DTB |
| 0xc00000 | SquashFS rootfs |

The default PSRAM profile uses AP Hex at 80 MHz. The 200 MHz profile can
be built with P4_PSRAM_MHZ=200. The profiles share the flash layout.

The FIT records a SHA-256 digest for each payload and detects corruption.
The DTB stays in NOR until U-Boot copies it into its 64 KiB PSRAM workspace
for fixups. The kernel stays mapped in NOR across the jump.

## Root filesystem

The Waveshare DTB defaults to root=/dev/mtdblock0 ro. To use an external
root filesystem, stop autoboot and set bootargs before running p4boot.
For example, if the first microSD partition contains an ext4 rootfs,

    setenv bootargs 'clk_ignore_unused root=/dev/mmcblk0p1 rootwait ro'
    p4boot

Linux mounts the partition using the specified root= device.
