# Standalone ESP32-P4 rev1.3 SPL and FIT

The ROM reads the ESP image at flash offset 0x2000 and enters U-Boot
SPL at 0x4ff40000. There is a single loadable SRAM segment. There is no
ESP-IDF SDK, sdkconfig, hardware helper, app, partition table or precompiled
IDF binary in the build or runtime. ROM functions used by SPL are silicon
entry points, not linked SDK objects.

Build via the project-level `make uboot TARGET=esp32p4`, or the ordinary
`espressif_esp32p4_spl_defconfig` and `espressif_esp32p4_sram_defconfig`,
using a RISC-V toolchain, host U-Boot dependencies and standalone esptool.
For the standalone form, wrap the SPL's combined `u-boot-spl-dtb.bin` in
an RV32 ELF with a single section mapped at 0x4ff40000 and entry at the same
address; use `esptool --chip esp32p4 elf2image`. Then use:

    python3 board/espressif/esp32p4/tools/pack-fit.py --mkimage OUT/tools/mkimage \
        spl-p4.bin u-boot.bin LINUX-IMAGE board.dtb OUT

Use the paired Linux cache-range patch and supplied
`tools/rebuild-linux-image.sh` to regenerate the supported kernel from
the known old payload. Do not combine the old kernel's fixed 16 MiB
executable-cache limit with dynamically sized memory.

The stock PSRAM profile is AP Hex x16 at 80 MHz (paired testing uses NOR
DIO/40). To build PSRAM at 200 MHz enable
`CONFIG_ESP32P4_SPL_PSRAM_200MHZ` in the SPL config (tested separately with
ROM NOR QIO/80). SPI NOR and PSRAM are distinct clocks. Clock registers
and runtime behavior were qualified in esp-emulator, not on silicon.

SPL rejects unsupported revisions and protected devices in read-only
eFuse checks, initializes peripherals and locked regions natively, and
protects the rest of boot using the RTC watchdog until proper handoff.
PSRAM MR checks determine size and geometry, check calibrated timing
with native ROM transactions, verify every MMU entry and test unique
scratch words across all pages before loading a FIT. Supported capacities
are 16/32/64 MiB; 4/8 MiB do not fit the reserved DTB layout. Proper
publishes the AP waiter before releasing hart1, and no SMP kernel starts
without a ready mailbox.

| Flash offset | Image data | Execution or physical destination |
|---|---|---|
| 0x002000 | spl-p4.bin | one segment at 0x4ff40000 |
| 0x0f0000 | p4-fit-header.dtb | verified fixed-profile FIT header |
| 0x100000 | proper u-boot.bin | 0x4ff00000 |
| 0x200000 | Linux Image | 0x48000000 |
| 0x900000 | DTB | staged 0x48600000, handoff 0x48f00000 |

The non-signed FIT checks SHA-256 of the actual loaded bytes and rejects
malformed sizes, addresses and roles. It is not secure boot. The merged
16 MiB flash is for emulator tests. Writing it to a real board would
overwrite other flash contents. No hardware operation is part of this
build or its scripts.

Use official esp-emu v1.1.0, a rev0 ROM ELF, fresh rev1.3 eFuse and
`--psram-size 32M` to run the image. Send `p4boot` at the U-Boot prompt.
Look for both CPUs, P4_SMP, P4_SIGNAL and login. `check-fit-reject.py`
tests the FIT with independently corrupted temporary copies.
