# ESP32-P4 v1.3 ROM → U-Boot with matched IDF hardware initialization

**Current default:** see [FIT-BOOT.md](FIT-BOOT.md) for the complete P4 build and new FIT header at 0x0f0000. The old bare-image recipe below documents fallback revision 735a4f1d; it does not generate a complete image for the new SPL.

This is a development image, **not** a replacement safe for secure-boot or
flash-encrypted devices. The adapter refuses those devices and any revision other
than 1.3. No eFuse programming, secure-boot enrollment, partition loader or IDF
application is linked into U-Boot. The full IDF bootloader is built separately as
a *reference*, not executed as another stage. Both source trees remain independent.

## Build the second-stage image

Use the board's exact `firmware/p4-hardware-loader/sdkconfig.reference` from
project `d8fa26d72bd6554257d245c38b5a87ee942777c9` and an activated
Espressif tool environment for ESP-IDF
`a602e67b0bf9ee0806dc4e1df7afc9affedf5c33`, GCC
`esp-15.2.0_20251204`, CMake, Ninja and esptool. First build and size U-Boot
proper and then SPL in **separate** output directories as before:

1. Build `espressif_esp32p4_sram_defconfig`. Check proper's `_start` equals
   0x4ff00000, and set `ubload.h:P4_UBOOT_SIZE` to its actual size rounded up to
   16. For repeatable audit banners use `SOURCE_DATE_EPOCH=1795046400` and
   make command-line `UBOOTRELEASE=2024.07-p4-uart-audit`.
2. Build `espressif_esp32p4_spl_defconfig`. **Replace** the old single-segment
   esptool wrapping step in the staging script with:

   ```sh
   board/espressif/esp32p4/idf-init/build.sh perf "$BOARD_SDKCONFIG" \
       "$SPL_OUT/spl/u-boot-spl-dtb.bin" "$OUT"
   ```

   `stock` is an A/B control. `perf` derives a new configuration from the exact
   stock input, preserving the original and the watchdog and security checks.
   Output `reference/bootloader.{elf,bin,map}` describes the independently bootable
   IDF reference. `p4-hal.{elf,bin,map}` contains only the linked hardware helpers.
   `spl-p4.bin` is the finished U-Boot second stage. Do **not** substitute the
   reference binary or an ordinary single-segment SPL for this output.
3. Use the existing 16 MiB flash layout: completed SPL at 0x2000, actual proper at
   0x100000, original Linux at 0x200000 and DTB at 0x900000. The ROM loads SPL at
   0x4ff40000, plus the separately linked hardware code/data at 0x4ff70000, and
   enters SPL. The latter calls into the hardware code via an integer-only ABI;
   IDF clears its own private BSS. ELF loaders/GDB must not bypass ROM/eFuse.

The board performance defaults name GD25Q128E and SFDP byte 4 = 6. They request
QIO/120-MHz flash, HEX/200-MHz PSRAM and 360-MHz CPU **for the application**.
The performance *bootloader* uses -O2, ERROR logging, the checked stock 9-s RWDT
and fixed 90-MHz CPU. Its actual generated ROM header is **DOUT/80**, then IDF
switches flash to **QIO/80**. Stock is DIO/40. The raw Linux path does not claim
to apply IDF's app-level 120/200/360 clock and calibration. Check physical part,
SFDP, margin and reset behavior before using a faster profile on a different board.

The matching boundary is the return from the pinned vendor's
`bootloader_init()`. Afterwards a different payload necessarily has a different
loader: generic IDF maps an application's IROM/DROM and jumps; our U-Boot maps
the raw NOR and prepares private Linux PSRAM, CPU1 and mailbox state. The super
WDT remains on auto-feed, the boot RWDT is fed during staging and released at
payload handoff, and entropy is disabled and UART flushed there. The existing
kernel, DTB, raw memory ABI and `p4boot` command remain unchanged. The signed
IDF application verifier is not emulated by an unsigned raw Linux loader.

Cloud validation used esp-emu v1.1.0 with the rev0 ROM ELF for pre-v3 P4, a fresh
rev1.3 eFuse identity and 32 MiB PSRAM. A simulator does not validate physical
PLL lock, flash timing eye, analog BOD/entropy, cache writeback, interrupt timing,
or the historical 20-s board stall. The older PSRAM device-specific path is not
replaced by app startup. See the matching round evidence for the measured scope.
