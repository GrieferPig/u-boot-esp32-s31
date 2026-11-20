# P4: U-Boot replaces the second-stage loader

**There is no ESP-IDF loader or app in front of U-Boot.** ROM loads one ESP image at flash 0x2000 and enters SPL at 0x4ff40000. Its second SRAM segment contains the pinned IDF *hardware functions*, not its partition selector, app image loader or RTOS. In the stock profile those functions print an IDF banner. For that reason the banner alone is not evidence of who owns the handoff.

S31 project 65361ef3 pins U-Boot d692a84e. See [actual build](https://github.com/GrieferPig/esp32-s31-linux/blob/65361ef3e8e9895086d802be0e83e60190cc9df8/mk/boot.mk), [S31 SPL](https://github.com/GrieferPig/u-boot-esp32-s31/blob/d692a84ea0bf80f319512bb8e892da0bbb93a4b4/board/espressif/esp32s31/spl.c) and [FIT definition](https://github.com/GrieferPig/u-boot-esp32-s31/blob/d692a84ea0bf80f319512bb8e892da0bbb93a4b4/arch/riscv/dts/esp32s31-u-boot.dtsi). Its ROM enters SPL in HP SRAM at 0x2f040000. SPL reads NOR FIT at offset 0xe000 and enters XIP OpenSBI fw_dynamic, which hands off to S-mode U-Boot; booti starts Sv32/XIP Linux. S31's checked-in FIT does not declare SHA-256 hash nodes. P4 shares the ROM/SPL/NOR-FIT principle, not S31 addresses, CSR programming, OpenSBI or the Linux ABI.

## Build and inspect

Use the tagged input board sdkconfig described in README.md, a configured environment for IDF a602e67b0bf9ee0806dc4e1df7afc9affedf5c33, GCC esp-15.2.0_20251204, esptool 5.6, U-Boot host dependencies (dtc/fdtget/fdtput, flex/bison, SWIG, Python/libfdt, etc.). Activate the SDK environment, put the cross-compiler and host tools on PATH, export IDF_PATH. With stock and performance use separate OUTPUT directories, **not** different source checkouts.

    board/espressif/esp32p4/idf-init/build-native-fit.sh stock BOARD.sdkconfig Image linux.dtb OUT
    OUT/sram/tools/dumpimage -l OUT/p4.itb
    sha256sum OUT/flash.bin

Replace stock with perf for the faster MSPI profile. Optional P4_IDF_BUILD points to the matching *already compiled* vendor bootloader build directory, to reuse its verified profile and relink only the hardware functions. Otherwise the script compiles the IDF reference from pinned sources. The complete reference bootloader.bin is **not installed**. Build fails for an unknown input sdkconfig. The ESP32-P4 hardware profile is not intended for encrypted or secure-boot devices; it refuses protected devices. No signing or eFuse provisioning.

## Storage map and handoff

| Flash offset | Bytes to place | CPU use |
|---|---|---|
| 0x002000 | OUT/spl-p4.bin | ROM loads native SPL→0x4ff40000, hardware helper segment→0x4ff70000 |
| 0x0f0000 | OUT/p4-fit-header.dtb | Standard FIT header, three SHA-256 nodes, board configuration p4 |
| 0x100000 | OUT/sram/u-boot.bin | M-mode proper→0x4ff00000, appended control DTB |
| 0x200000 | original Image | RV32 NOMMU Linux→0x48000000 |
| 0x900000 | original linux.dtb | stage→0x48600000; patched Linux handoff→0x48f00000 |

OUT/flash.bin is a complete, 16 MiB **emulator** image, with 0xff in unallocated space. Writing a whole merged image to hardware would overwrite other data there. No physical flash command is part of the tests. OUT/p4.itb is the contiguous/sparse-layout FIT starting at 0x0f0000 and already includes all three payloads; its data-position properties are relative to the FIT start. Do not flash that at the U-Boot proper slot.

The P4 SPL parser is a fixed-address board FIT profile over libfdt, not the generic S31 OpenSBI loader. It enforces the selected configuration, roles, target addresses, slot limits, uncompressed form, SHA256, actual DTB totalsize and bounded kernel runtime span. It hashes the bytes loaded (uncached PSRAM alias for kernel and DTB), feeds the boot RWDT after each 16 KiB chunk, and only jumps after every check passes. Kernel BSS tail is zero-filled from the validated runtime size rather than read past the file. Hashes detect accidental corruption; the manifest is unsigned, and a writer able to replace both payload and hash can create a matching pair. Not secure boot.

## Emulator test

    esp-emu --chip esp32p4 --firmware OUT/flash.bin --rom esp32p4_rev0_rom.elf \
      --efuse fresh-rev1.3.efuse --psram-size 32M --console uart0 --net user,restrict=on \
      --inject-on 'esp32p4>' --inject 'p4boot\r' --exit-on 'esp32-p4 login:'
    python board/espressif/esp32p4/idf-init/check-fit-reject.py \
      OUT/flash.bin /path/to/esp-emu /path/to/rev0-rom.elf fresh-rev1.3.efuse NEGATIVE-LOGS

Look for three P4 FIT SHA256 OK lines before proper, two online CPUs, P4_SMP PASS and P4_SIGNAL PASS before login. The negative verifier only modifies temporary emulator copies. Do not infer physical flash/PSRAM timings or a silicon IPI fix. The former raw loader is preserved at revision 735a4f1d / local tag p4-pre-fit-735a4f1d; roll back the whole matching image, not a mixture of artifacts.

## Rev1.3 full-capacity AP Hex RAM

Apply the paired Linux cache-range patch to the P4 Linux tree, then use
`rebuild-linux-image.sh LINUX-TREE ORIGINAL-IMAGE OUT-LINUX` with the original
32-bit NOMMU `Image(2)` and RISC-V ESP GCC 15.2.0. Pass the resulting
`OUT-LINUX/arch/riscv/boot/Image` to `build-native-fit.sh`. The original
kernel baked a 16 MiB executable-cache limit into its driver; it can
occasionally boot with larger DT memory yet later panic during a userspace
exec. Do not use that old kernel with the new full-capacity flash image.

The build links only IDF bootloader hardware support and the AP Hex PSRAM
driver and timing closure from a pinned SDK. The scratch IDF app build
produces archives only; none of its bootloader, app, partition table, FreeRTOS
scheduler, or startup hooks is installed. `P4_IDF_PSRAM_BUILD` can point to
existing pinned app-mode archive output for the selected profile.

Start with `stock` (80 MHz PSRAM). `perf` selects the pinned IDF 200 MHz
profile; it has emulator coverage but is not board timing qualification.
The image detects AP/compatible, known-good-die x16 PSRAM, accepts 16/32/64
MiB, reads back every mapped MMU entry, and simultaneously compares distinct
scratch signatures through the direct aperture before the FIT is read.
The checker restores the scratch words. Absent and unknown parts and 4/8
MiB devices cannot boot this fixed DTB layout. The SPL publishes the usable
size; U-Boot updates the verified DT copy and does not remap it. Linux's
fixed retained-DTB and framebuffer reservations remain in place.

The AP is held through SPL and proper, then enters the ROM with the LP
waiter address already published. Both harts use an exact-aperture PMP7
entry; the AP locks its own entry and no pre-existing PMP entry is unlocked
or overwritten. Its ROM-provided PMA is not replaced with FreeRTOS's region
table. A missing AP handshake refuses to enter SMP Linux. U-Boot proper
also bounds its retained executable cache callback to the measured window.
