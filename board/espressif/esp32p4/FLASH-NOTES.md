# ESP32-P4 SPL flash read notes

Goal: read the boot images from SPI NOR in the SPL so U-Boot proper can be
loaded without the IDF second-stage loader.

## What is known to work

- The SPL itself is loaded by the ROM from flash offset 0x2000 into SRAM
  (`0x4ff40000`) — the ROM does the flash reads for us, but only for the
  segment it knows about.
- PSRAM access at `0x48000000` works from the SPL (see `PSRAM-NOTES.md`).

## What does not work yet

| Attempt | Result |
|---|---|
| MSPI0 user-mode transaction (`CMD 0x03`, 24-bit address, MISO 64 bytes) | completes, data buffers read back as `0` |
| Direct load from the flash XIP window `0x40000000` before any cache/MMU setup | `0x0addbad0` (bus error marker) |
| Flash MMU mapping + ROM cache enables, then load from `0x40000000` | `0xffffffff` |
| Same with the caches enabled before the MMU entries are written | `0xffffffff` |

Writing an MMU entry and reading it back works, which proves the register pair
is reachable and writable:

```
index   = MSPI0 + 0x380
content = MSPI0 + 0x37c
write index 0x21, content 0x12345 -> read back 0x21 / 0x2345 (high bits masked)
```

Note the content register keeps only the low bits (page number, access type,
valid/sensitive flags), so a 64 KiB page mapping is written as
`content = page | BIT(12)` (flash valid, access = flash = 0).

MSPI0 also initialises as `0x37c = 0x3ff`, `0x380 = 0x13200004`, `0x384 = 0xf`,
which suggests the ROM left MMU state behind.

## Registers and helpers collected for the port

```
MSPI0 base            0x5008c000
MMU item content      MSPI0 + 0x37c
MMU item index        MSPI0 + 0x380
MMU page size         64 KiB, fixed (mmu_ll_set_page_size asserts MMU_PAGE_64KB)
Flash XIP window      0x40000000, 0x400000 zero-filled 1024 entries
Entry encoding        page | BIT(12) valid, access = 0 (flash)
ROM_Boot_Cache_Init   0x4fc003dc  (no arguments)
Cache_Enable_L1_DCache        0x4fc004f4
Cache_Enable_L1_CORE0_ICache  0x4fc004d4
Cache_Enable_L1_CORE1_ICache  0x4fc004e4
Cache_Enable_L2_Cache         0x4fc00504
Cache_Sync_Addr               0x4fc003e0
Cache_Invalidate_Addr         0x4fc003e4
Cache_WriteBack_Addr          0x4fc003f4
```

ESP-IDF's own init sequence (`bootloader_init_ext_mem`) is
`cache_hal_init()` (which on the P4 enables the *L2* cache, since
`CACHE_LL_LEVEL_EXT_MEM == 2`, plus preload strategy) followed by
`mmu_hal_init()` (`ROM_Boot_Cache_Init()` + unmap-all).

## Next ideas to try

1. Port `cache_ll_enable_cache(2, CACHE_TYPE_ALL, CACHE_LL_ID_ALL, ...)` field by
   field instead of relying on the ROM helpers, and confirm with a cache
   register read-back that the cache is actually enabled.
2. Check whether the SPL's M-mode accesses to the flash window are blocked by
   a PMP/PMA entry (the S31 port had to add a PMA entry for PSRAM stores); the
   `0x0addbad0` bus-error marker points in that direction.
3. Fall back to reading through the ROM's flash code by calling
   `esp_rom_spiflash_attach()` and then using the ROM's SPI read path.

## Working recipe (round 7)

The flash read works with this exact sequence, run once before the first load:

1. `HP_SYS_CLKRST + 0x38 |= (1 << 1) | (1 << 4) | (1 << 7) | (1 << 10)`
   (cache block clock force-on, same bits the S31 port uses).
2. `ROM_Boot_Cache_Init()` (`0x4fc003dc`) — required, otherwise the cache
   helpers trap.
3. `Cache_Invalidate_All(CACHE_MAP_L1_ALL)` (`0x4fc00404`).
4. Flash MMU page size: `MSPI0 + 0x384` bits [4:3] = **2** (64 KiB). Leaving it
   at the reset value 0 (max page size) makes every read return `0xffffffff`.
5. Clear all 1024 entries, then map `content = page | BIT(12)` with
   `index = page` (registers `MSPI0 + 0x37c` content, `MSPI0 + 0x380` index).
6. `Cache_Enable_L1_DCache(0)`, `Cache_Enable_L1_CORE0_ICache(0)`,
   `Cache_Enable_L1_CORE1_ICache(0)` (`0x4fc004f4`, `0x4fc004d4`, `0x4fc004e4`).
7. Read with plain loads from `0x40000000 + offset`.

Do **not** call `Cache_Disable_L1_CORE0_ICache()` (`0x4fc004d0`) — it traps even
after `ROM_Boot_Cache_Init()`.

Verified: flash offset 0 reads `0xffffffff` (erased) and offset 0x10000 reads
`0x4f0306e9`, the first instruction of the U-Boot payload flashed there.

## PSRAM window limit

The ROM leaves only part of the PSRAM window accessible from the CPU:

- `0x48000000` reads and writes fine (the PSRAM check passes there).
- `0x48100000` traps, so the payload cannot be placed at `0x48c00000` yet.

Writing the PSRAM MMU table (`MSPI2 + 0x37c` / `0x380`, entry
`page | BIT(11) | BIT(10)`) reads back correctly but has no effect on access,
so the table is not what gates the window (the candidate is the `SMEM_PMS*`
window registers or an MMU mode bit). Until that is solved the SPL stops after
confirming PSRAM and the flash read.

## Round 8 findings

- The whole 160 KiB payload can be read from flash in ~15 ms (0.75 ms per
  16 KiB chunk), so the copy is not slow and watchdog timeouts are not the
  issue.
- The L2 cache has to be enabled for external memory; `Cache_Enable_L2_Cache`
  (`0x4fc00504`) was missing from the first bring-up. With it, the payload
  address `0x48c00000` becomes writable (`payload 0x12345678` read back).
- The cache block (`0x3ff10000`, 0x00..0x60) and the PSRAM MMU power register
  (`MSPI2 + 0x384 = 0x13200004`) in the SPL are **identical** to a working
  ESP-IDF boot, so the cache is not the remaining difference.
- The flash MMU entries read back exactly as written (128 pages, 0 bad) and the
  flash read completes over the whole payload.
- The remaining limit is the PSRAM window: copying with `dest = 0x48c00000 +
  offset` traps at offset 0x10000 (`0x48d00000`), while a copy into a fixed
  `0x48c00000` buffer works. So only a small window around `0x48c00000` is
  mapped.
- Trying to disable the data/L2 caches before rewriting the PSRAM MMU table
  (`Cache_Disable_L1_DCache` `0x4fc004f0`, `Cache_Disable_L2_Cache`
  `0x4fc00500`) traps, just like the icache disable did.

Next: clear the PSRAM MMU table (write 0 to every entry, as IDF's
`mmu_hal_unmap_all` does) *before* mapping all 512 pages, and probe the window
one 64 KiB page at a time to measure its real size.

## Round 9 findings: the PSRAM window is only partly mapped

Measured with a page-by-page write/read walk from the SPL:

```
0x48000000 .. 0x480d0000  usable   (14 pages = 896 KiB)
0x480e0000                faults
0x48c00000                usable   (the IDF payload area)
```

So the ROM maps a small low window plus the payload area, and nothing else.

The PSRAM MMU registers behave like a staging/latch pair, not like a directly
readable table:

| Context | Reading entry 0x00 / 0xc0 |
|---|---|
| working ESP-IDF boot | `0x00000c00` / `0x00000cc0` (linear, as IDF wrote them) |
| SPL | `0x000001a0` / `0x00001299` and different on every boot |

Writing entries from the SPL therefore only changes that latch: after mapping
pages 14..255 (or 0..511, or both PSRAM controller instances) the CPU still
cannot touch `0x480e0000`. Trying to remap a page in the working boot also left
the data unchanged, which fits the same explanation.

Consequence: the payload cannot be placed at `0x48c00000` (beyond its 64 KiB
window) or at `0x48000000` (only 896 KiB) until the real mapping path is found.

### Options for the next round

1. Put U-Boot into the 896 KiB window: rebuild with `TEXT_BASE = 0x48000000`
   and `board_get_usable_ram_top() = 0x480e0000`, load 160 KiB there and jump.
   U-Boot fits; the kernel does not, so the chain would stop at the U-Boot
   prompt (still a real SPL -> U-Boot hand-off).
2. Find the actual mapping path: compare the SPL against the bootloader, e.g.
   dump `SPI_MEM_S_` registers that could gate the table (`MISC`, `CTRL`,
   `AXI_*`) and check whether the table is only writable while the cache is
   configured the way `cache_hal_init()` leaves it (caches on, L2 mapped).

## Round 12 findings: JTAG works, and the trap is decoded

### JTAG

`C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20260703\openocd-esp32\bin\openocd.exe`
with `-f board/esp32p4-builtin.cfg` finds both taps on the built-in USB-JTAG and
halts both harts (telnet on 4444, gdb on 3333). WSL cannot see the USB device, so
OpenOCD must run on Windows.

### The U-Boot trap

Halt just after the hand-over, then read the trap CSRs:

```
PC     0x48000242    Halt cause (2) Illegal Instruction
mcause 0x30000002
mepc   0x48000240
mtval  0x0000bad0    <- the fetched instruction word
```

`0x0addbad0` is this SoC's bus-error filler. Memory reads at `0x48000230`
return `0addbad0 00000000 0addbad0 00000000` (exactly one 32-byte cache line),
while the payload file has zeros there: the payload simply is not in PSRAM.

### Why the copy loses data

Per-chunk checksums (sum of the destination words) compared against checksums
computed from `u-boot.bin` on the host:

| run | result |
|---|---|
| plain copy | chunks 0..8 match, chunk 9 differs |
| + `Cache_WriteBack_All(3)` | chunk 7 differs |
| + per-chunk `Cache_Sync_Addr` | chunk 8 differs |

The failing chunk moves between runs, so part of the payload is lost from the
write-back path (the copy never reaches PSRAM for those lines) or is filled with
the bus-error pattern.

### Cache map bits (from `rom/cache.h`)

```
CACHE_MAP_L1_ICACHE_0 BIT(0)   CACHE_MAP_L1_DCACHE BIT(4)
CACHE_MAP_L1_ICACHE_1 BIT(1)   CACHE_MAP_L2_CACHE  BIT(5)
```

Passing `3` to `Cache_WriteBack_All`/`Cache_Invalidate_All` only touches the two
icaches - the data cache and L2 need `0x33`. That mistake is fixed, but the
corruption persists, so something else keeps dirty lines from reaching PSRAM.

Next: test PSRAM stability on its own (fill 128 KiB with a counter pattern, sync,
sum, compare) to decide whether this is a cache-flush problem or marginal PSRAM
access, then either sync per chunk with the right map (`0x30`) and a verified
`Cache_Sync_Addr` signature, or copy with the data cache off.

## Round 13 findings: the PSRAM window is ~128 KiB, and the cache/PSRAM command registers

Dumped MSPI2 from a fully initialised ESP-IDF boot (via JTAG, board running the
normal chain) and compared with the SPL after the replay:

| offset | IDF | SPL (replay only) | note |
|---|---|---|---|
| 0x3c CACHE_FCTRL  | `40000001` | `c0000000` | differs |
| 0x40 CACHE_SCTRL  | `02f7c679` | `0055c070` | differs |
| 0x44 SRAM_CMD     | `8cfc0000` | `80c00000` | differs |
| 0x48 SRAM_DRD_CMD | `f0000000` | `00000000` | unset |
| 0x4c SRAM_DWR_CMD | `f0008080` | `00000000` | **PSRAM write opcode, unset** |
| 0x50 SRAM_CLK     | `00010001` | `00000000` | unset |
| 0x54              | `00000200` | `00000200` | same |

These are now programmed by `psram_cache_cmds()` after the replay (`psram: cmds`
in the boot log). Without them cached writes never reach the device.

### The actual window

The per-chunk checksums pin the writable window down: with a 160 KiB payload
copied to `0x48000000`, chunks 0..7 (`0x48000000 .. 0x4801ffff`) match the file
exactly and chunk 8 (`0x48020000`) onwards reads back as the `0addbad0` filler.

So the ROM leaves roughly **128 KiB** usable at `0x48000000`, not the 896 KiB
that the round-9 walk suggested. That walk only wrote one word per 64 KiB and
read it back through the cache, so it proved nothing.

### Consequence

U-Boot proper (160 KiB) does not fit. Either the PSRAM mapping has to be
extended for real, or U-Boot must be rebuilt small enough to fit in 128 KiB
(a minimal build without commands/env could reach that).

## Round 14: U-Boot proper runs from SRAM

The PSRAM window is too small for a 160 KiB U-Boot, so the chain now loads
U-Boot into the lower half of the HP SRAM instead (`espressif_esp32p4_sram_defconfig`,
`TEXT_BASE = 0x4ff00000`, `CONFIG_ESP32P4_SRAM_ONLY`):

```
jump: entry 0x4ff00000 dtb 0x00000000

U-Boot 2024.07-gc10b822539-dirty (Nov 18 2026 - 04:39:18 -0800)

DRAM:  768 KiB
```

That is the SPL handing over to U-Boot proper with **no IDF bootloader in the
chain** - the first stage bootloader part of the port works.

Pieces that made it work:

- `P4_UBOOT_LOAD_ADDR = 0x4ff00000` (`ubload.h`, written by the staging script,
  which also refuses to stage a payload whose `_start` is not `0x4ff00000`).
- SRAM is uncached, so the copy needs **no** cache flush. In fact the ROM cache
  flush helpers (`Cache_WriteBack_All` / `Cache_Invalidate_All`) **trap** in the
  SPL and had to be removed - they were the reason the earlier PSRAM attempts
  "lost" data.
- `dram_init()` / `dram_init_banksize()` in `board.c` report the SRAM window
  (base `0x4ff00000`, top `0x4ffc0000`, 768 KiB) instead of the device tree's
  32 MiB PSRAM node; the generic RISC-V versions in
  `arch/riscv/cpu/generic/dram.c` are now `__weak` so the board wins.
- Flash reads are deterministic (three reads of the same 16 KiB chunk give the
  same checksum, and it matches the file), so the copy is trustworthy.

Still open: U-Boot relocates itself to `ram_top - mon_len` (about `0x4ff93a00`)
and then traps at `0x4ff93240` with `mcause 0x30000002` (illegal instruction) -
i.e. it jumps a few hundred bytes below the relocated image into unused SRAM.
Next: check `gd->mon_len`/`relocaddr` handling for this layout (or disable
relocation for the SRAM build).

## Round 15: where the SRAM hand-over stops

`CONFIG_ESP32P4_SRAM_ONLY` now has an initcall tracer: `lib/initcall.c` writes
the address of every entry of `init_sequence[]` to UART0 before calling it
(only for this configuration). The boot shows U-Boot running the **whole**
`init_sequence_f` list:

```
U-Boot 2024.07-...        banner
 4ff08908 ... 768 KiB     every initcall traced, DRAM reported
 ... 4ff089da KLMN        last initcall is setup_reloc, then jump_to_copy
```

Markers placed in `arch/riscv/cpu/start.S` (`K` before `invalidate_icache_all`,
`L` after it, `M` after `flush_dcache_all`, `N` before `jr t4`) all appear, so
the code reaches the jump into `board_init_r` - and dies there.

JTAG shows why: the relocation destination is still **empty**:

```
0x4ff93000: 00000000 00000000 ...    <- where U-Boot should have copied itself
0x4ff00000: f1402573 84ae822a ...    <- source image, intact
mepc 0x4ff93280  mcause 0x30000002   <- running zeros
```

So `jr t4` jumps into untouched SRAM. The copy loop in `relocate_code` either is
skipped (`beq t0, s4`) or writes somewhere else. `relocaddr` comes from
`ram_top - mon_len` = `0x4ffc0000 - 0x2c5e8` rounded down = `0x4ff93000`, which
does not overlap the source, so the arithmetic looks right - the next step is a
direct print of `gd->relocaddr`, `gd->start_addr_sp` and `gd->mon_len` right
before the `memcpy` in `setup_reloc` (the helper is in place as
`p4_reloc_report()`, but its output did not appear in the last run).

Also learned: `GD_FLG_SKIP_RELOC` is not an option on RISC-V - `board_init_f`
must not return, so skipping relocation just falls through into SPL code.

## Round 16: U-Boot runs in place and reaches board_init_r

The relocation problem was the write-back cache: the copy lands in the data
cache while the instruction fetch reads SRAM, so the relocated code is invisible
(and `Cache_WriteBack_All` from the ROM traps in this port). Fixed by running
U-Boot **in place** for `CONFIG_ESP32P4_SRAM_ONLY`:

- `reserve_uboot()` keeps `gd->relocaddr = CONFIG_TEXT_BASE` and puts
  `gd->start_addr_sp` at `gd->ram_top`, so `relocate_code()` finds
  `dest == _start`, skips the copy, sets `t6 = 0`, clears the real BSS and jumps
  to `board_init_r` in place.
- `initr_malloc()` puts the heap right above the image instead of below a
  relocated copy (`gd->relocaddr + gd->mon_len`).

The trace after the hand-over now shows U-Boot's *second* init sequence running:

```
KLMN 4ff08ee0 4ff08ee4 4ff0901c 4ff0900c 4ff08ef4 4ff09028 4ff08fe6 ...
Unhandled exception code: 939524103        <- 0x38000007: store access fault
EPC: 4ff008f8 (memset+0x68)  RA: 4ff08ffa (initr_malloc+0x14)
TVAL: 4fed1000
```

So `board_init_r` is reached, the global data pointer and console plumbing work
(U-Boot prints its own exception dump now), and the current stop is
`initr_malloc()`: the heap start is still computed below the SRAM window
(`0x4fed1000`). The `initr_malloc()` patch above addresses that; the next run
died inside it again, so the remaining item is the malloc/`TOTAL_MALLOC_LEN`
arithmetic (`TOTAL_MALLOC_LEN` may be larger than `CONFIG_SYS_MALLOC_LEN`).

Resolved symbols for reference: `4ff08fe6 initr_malloc`, `4ff09028
initr_barrier`, `4ff0901c initr_reloc_global_data`, `4ff0900c initr_caches`,
`4ff09012 event_init`.

## Round 17: state of the SRAM chain

The SPL -> U-Boot hand-over is stable and reproducible with the tracing removed:

```
jump: entry 0x4ff00000 dtb 0x00000000

U-Boot 2024.07-... (Nov 18 2026)

DRAM:  768 KiB
```

U-Boot continues into `board_init_r` and runs `init_sequence_r` up to
`initr_malloc`. Measurements taken with temporary prints:

- `TOTAL_MALLOC_LEN = 0x2f000` (192 KiB), not just `CONFIG_SYS_MALLOC_LEN`.
- The heap is now placed at `0x4ff40000`, the SRAM the SPL used and that is
  free after the hand-over (`start = 0x4ff40000` in `initr_malloc` for
  `CONFIG_ESP32P4_SRAM_ONLY`); it ends at `0x4ff6f000`, well inside SRAM.
- The `DRAM:  768 KiB` line and the tracing showed that everything before
  `initr_malloc` succeeds, including `initr_reloc_global_data`, `initr_caches`
  and `event_init`.

Still to solve: after `initr_malloc` the board resets without an exception dump,
i.e. the next steps (DM/console init, `initr_of_live`, `initr_env`) fail before
the console is up. Note `u-boot.bin` already contains the device tree
(`u-boot-dtb.bin` and `u-boot.bin` are byte-identical), so the appended DT at
`_end` is intact.

Ideas for the next round: trace `init_sequence_r` the same way we traced
`init_sequence_f` (the tracer is easy to re-add in `lib/initcall.c`), and check
DM init specifically - `initr_dm` needs the DT to probe the UART, and a failure
there would explain the silence.

## Round 18: U-Boot's second init sequence, traced

Two lessons about tracing, both now handled in `lib/initcall.c`:

- printing the address before *and* a `.` after each call separates "called" from
  "returned";
- the tick must watch the UART TX FIFO (`0x500ca01c`, TXFIFO count in bits
  21:16) - a plain `writel` drops characters and made the trace look truncated.

The trace (SRAM build) now reads:

```
U-Boot 2024.07-...
DRAM:  768 KiB
. 4ff08e1c. 4ff08aa2. 4ff089da. ... . 4ff08e70. 4ff08e74. 4ff08fb4. 4ff08fa4.
4ff08e84. 4ff08fc0. 4ff08f76 123
```

`123` are the ticks inside `initr_malloc`, so that function's body runs to
completion (`gd_set_malloc_start`, `mem_malloc_init(0x4ff90000, 0x2f000)`).

The `.` that should follow it never appears, and the next initcall's address is
never printed, so the stop is in `initr_malloc`'s return path / the loop right
after it. `initr_of_live`, `initr_dm_devices`, `initr_bootstage`,
`initr_announce` and `initr_binman` are all `li a0,0; ret` stubs in this build,
so they cannot be the faulting code.

Also confirmed this round: `CONFIG_LOG` is off (so `debug()` is a no-op),
`CONFIG_BOOTSTAGE` is off, and `u-boot.bin == u-boot-dtb.bin` (the DT is
appended at `_end`).

Next: disassemble the current `initr_malloc` (offsets shift between builds) and
tick just before its `ret` and inside the loop after the call; if the return
itself is the problem, dump the stack around `gd->start_addr_sp` (0x4ff8f8b0)
over JTAG while halted at the tick.

## Round 19: U-Boot's second init sequence completes, and the console speaks

Two findings:

1. **`CONFIG_SYS_MALLOC_CLEAR_ON_INIT` broke the return from `initr_malloc`.**
   With it enabled, `mem_malloc_init()` memsets the whole `TOTAL_MALLOC_LEN`
   (0x2f000) region, and `initr_malloc` never returned (`123` printed but no
   `.` after the call). Disabling it in `espressif_esp32p4_sram_defconfig` fixes
   that and the whole `init_sequence_r` now runs.
2. **U-Boot's own console output works.** The trace now ends with real U-Boot
   messages:

```
... : 4ff0a008.: 4ff0d772
No serial driver found
resetting ...
### ERROR ### Please RESET the board ###
```

So `printf`/`puts`/`panic_str` all work: the early console is fine. What fails
is `serial_init()` finding a `UCLASS_SERIAL` device, which panics because
`CONFIG_REQUIRE_SERIAL_CONSOLE=y`.

Things checked this round:

- `arch/riscv/dts/esp32p4.dts` has `uart0: serial@500ca000` with
  `compatible = "esp,uart0"`, `aliases { serial0 = &uart0; }` and
  `chosen { stdout-path = "serial0:115200n8"; }` - all correct.
- `drivers/serial/serial_esp.c` is in the build (the `esp,uart0` string is in
  the image) and its `setbrg` falls back to `clock-frequency` when no clock
  driver is present, so `clock-frequency = <40000000>` was added to the uart0
  node - the panic is unchanged, so the device is not being bound at all
  rather than failing to probe.
- `CONFIG_LOG` and `CONFIG_BOOTSTAGE` are off, `CONFIG_ESP_SERIAL=y`.

Next: check whether device model sees the DT at all (print `gd->fdt_blob` and
the result of `initr_dm`), then either fix the bind or set
`CONFIG_SERIAL_SEARCH_ALL` so `serial_init` can fall back to any working device.

## Round 20: the driver model gets a device tree, and U-Boot prints

The serial panic had one root cause: **the appended DT was being wiped by the
BSS clear.**

- `_end` is placed *before* `.bss` in `arch/riscv/cpu/u-boot.lds`, and for
  `OF_SEPARATE` the DT is appended at `_end`, so the blob sits exactly where
  `clear_bss` runs. Disabling `reloc_fdt` made it worse (`fdt_check_header`
  returned `-9` = bad magic).
- Fix: `reserve_fdt()` parks the copy at a fixed free SRAM address
  (`0x4ff70000`, between the SPL area and the heap) and `reloc_fdt()` keeps
  copying it there before the BSS wipe.

Evidence (`D <fdt> <hdr> <ret> <serials> <offset> <size> <simplebus>`):

```
D 4ff70000 00000000 00000000 00000001 00000328 000015c3 00000002
```

- header check 0 (valid), `dm_init_and_scan` 0, **one serial device**,
  `/soc/serial@500ca000` found at offset 0x328, `simple-bus` bound.

And U-Boot now talks over its own console:

```
Core:  7 devices, 5 uclasses, devicetree: separate
Loading Environment from nowhere... OK
```

The next stop is `jumptable_init()` (+0x30, `TVAL 0`): it stores through a NULL
pointer, i.e. `malloc(sizeof(struct jt_funcs))` did not give a usable heap -
the allocator is the next thing to fix (`initr_malloc` only sets the globals
because `CONFIG_SYS_MALLOC_CLEAR_ON_INIT` and `..._DEFAULT_TO_INIT` are off).

Note `himport_r: can't insert "bootm_low=..." into hash table` appears because
there is no environment storage yet; harmless for now.

Timeline of the console, for reference:

```
768 KiB                       <- DRAM line (no console, printed by puts)
.: <initcall addresses>       <- instrumentation
Core: 7 devices, 5 uclasses   <- first output through the DM serial driver
Loading Environment ... OK
```

## Round 21: the heap is the remaining blocker

`jumptable_init()` (right after `initr_env`) faults because `malloc()` returns
NULL and the function stores straight through `gd->jt` (`EPC` inside
`jumptable_init`, `RA` = its own `+0x10`, `TVAL 0`). Disassembly confirms the
function has no NULL check:

```
sw a0,136(s0)      <- gd->jt = malloc(...)  (136 = offsetof(gd_t, jt))
lw a5,136(gp)
...
sw a3,4(a4)        <- *(jt + 4) = ... with jt == 0
```

Tried and **reverted** this round:

- `CONFIG_SYS_MALLOC_DEFAULT_TO_INIT=y` (calls `malloc_init()` from
  `mem_malloc_init`): no change, still NULL, and the trace moved slightly, so it
  was reverted to keep the tree at the verified round-20 state.
- A temporary report inside `jumptable_init` (reverted).

Current verified state (round-20 commit `6afafe9c68`, reproduced after the
revert):

```
Core:  7 devices, 5 uclasses, devicetree: separate
Loading Environment from nowhere... OK
```

So the console is fine; only the allocator is missing. `initr_malloc` sets
`mem_malloc_start = 0x4ff90000`, `mem_malloc_end = 0x4ffbf000` (TOTAL_MALLOC_LEN
0x2f000) and nothing else because `CONFIG_SYS_MALLOC_CLEAR_ON_INIT` is off.

Next ideas, in order:

1. Print `mem_malloc_start/end/brk` and the `malloc()` return value from
   `jumptable_init` (the report harness is already written, just re-apply it)
   to see whether the region is registered at all.
2. Check whether `CONFIG_SYS_MALLOC_F` (`gd->malloc_base/ptr/limit`) interferes -
   with the in-place layout the early heap limit may be stale.
3. If dlmalloc keeps failing, give the SRAM build a `malloc()` that serves from
   a bump allocator over `0x4ff90000..0x4ffbf000`; nothing in the first-stage
   path needs free().

## Round 22: the U-Boot prompt is up from the SPL chain

Two changes turned the corner:

1. **Heap placement.** `start = 0x4ff90000` left `malloc()` returning NULL;
   moving the heap to `0x4ff30000` - the free window between the end of the
   in-place U-Boot image (0x4ff2c598) and the SPL BSS (0x4ff60000) - fixed it.
2. **No autoboot.** `CONFIG_BOOTDELAY=-1` and a harmless
   `CONFIG_BOOTCOMMAND="echo P4-SRAM-U-BOOT"`, because the previous
   `booti 0x48000000 - 0x48f00000` command reset the board.

Result (SPL chain, COM9 115200):

```
U-Boot SPL 2024.07-...
ESP32-P4 SPL active
...
jump: entry 0x4ff00000 dtb 0x00000000

U-Boot 2024.07-...

DRAM:  768 KiB
Core:  7 devices, 5 uclasses, devicetree: separate
Loading Environment from nowhere... OK
In:    serial@500ca000
Out:   serial@500ca000
Err:   serial@500ca000
P4_UBOOT: M-mode bootloader active (emulator only)
esp32p4>
```

That is the full first-stage chain: ROM -> SPL -> U-Boot proper with driver
model, environment, serial console and an interactive prompt, all inside the
768 KiB of SRAM (no PSRAM needed for U-Boot itself).

Notes for the next round:

- The board apparently resets shortly after the prompt appears (a second ROM
  banner shows up in the capture). `CONFIG_WDT` is off, so the HP system
  watchdog / SWDT that the ROM starts is the likely cause; feeding or disabling
  it is the next step so the prompt stays alive.
- The initcall tracer in `lib/initcall.c` (guarded by
  `CONFIG_ESP32P4_SRAM_ONLY`) is still enabled and prints `.`/`<address>` per
  initcall. It should be removed once the port is done.
- `CONFIG_USE_BOOTCOMMAND=y` with the echo command means the prompt never tries
  to boot anything; the real `booti` path (kernel at 0x48000000, DT at
  0x48f00000 in PSRAM) still needs the PSRAM window and a loader step.

## Round 23: the board still resets ~0.3 s after the prompt

The SPL->U-Boot chain reaches `esp32p4>` reliably, but the board then resets
about every 0.6 s (22 ROM banners and 20 prompts in a 14 s capture). Findings:

- Watchdog register map confirmed from IDF
  (`components/soc/esp32p4/register/hw_ver3/soc/lp_wdt_*`):
  `LP_WDT @0x50116000`, CONFIG0 `0x00` (`wdt_en` bit 31, `flashboot_mod_en`
  bit 14), CONFIG1..4 `0x04..0x10`, FEED `0x14`, WPROTECT `0x18`,
  SWD_CONFIG `0x1c` (`swd_auto_feed_en` bit 18, `swd_feed` bit 31,
  `swd_disable` bit 30), SWD_WPROTECT `0x20`; key `0x50d83aa1`.
- The SPL already uses the right offsets and key, and JTAG confirmed
  `CONFIG0 == 0` (RWDT off) and both TIMG MWDTs off (`0x500c2048`,
  `0x500c3048` are 0).
- IDF's own recipe for the super watchdog is auto-feed, not disable:
  `bootloader_esp32p4.c: bootloader_super_wdt_auto_feed()` = write key to
  SWD_WPROTECT, `REG_SET_BIT(SWD_CONFIG, SWD_AUTO_FEED_EN)`, clear the key.
  Implemented in `board.c: p4_wdt_off()` (called from `board_late_init`) - the
  resets continue.
- Feeding both RWDT and SWDT from the serial polling path did not help either
  (that hook was removed again).
- Halting the CPU with JTAG for 7 s produced no "hart unexpectedly reset",
  which suggests the reset is watchdog-driven (watchdogs pause under debug)
  rather than a software `reset_cpu()` (`reset_cpu()` would print
  "resetting ...", and that never appears after the prompt).

So there is a watchdog we have not located yet - OpenOCD reports both
"Reset cause (18) Super Watchdog" and "(7) HP WDT resets system". Next round:
dump the PMU/HP_SYS blocks (0x50115000, 0x500e6000) and the remaining timer
groups for an enabled watchdog, and compare against an IDF boot where the
prompt stays alive. `esp32p4>` itself is reproducible, which is the important
part - the reset is the last obstacle to an idle prompt.

## Round 24: watchdog state matches the working chain, resets remain

`reset_reasons.h` for the P4 maps the reason the ROM prints:

```
RESET_REASON_CORE_MWDT      = 0x07   <- what we see ("HP WDT resets system")
RESET_REASON_CORE_RWDT      = 0x09
RESET_REASON_SYS_RWDT       = 0x10
RESET_REASON_SYS_SUPER_WDT  = 0x12   <- also reported by OpenOCD sometimes
```

JTAG comparison of our chain against the working IDF chain (same addresses):

| register | IDF chain | our chain |
|---|---|---|
| `LP_WDT+0x00` CONFIG0 (wdt_en bit31) | 0 | 0 |
| `LP_WDT+0x04` CONFIG1 | 0x956a0 | 0 |
| `LP_WDT+0x1c` SWD_CONFIG | 0x40000000 (disabled) | 0x40000000 (disabled) |
| `TIMG0+0x48` | 0 | 0 |
| `TIMG1+0x48` | 0 | 0 |
| `TIMG(+0x3000*2)+0x48` | 0 | 0 |
| `TIMG(+0x3000*3)+0x48` | 0x000000c7 (EN=0) | 0 |

So no watchdog is enabled on our chain either, and feeding all of them
(RWDT feed, SWD feed, and the four TIMG WDT feed keys) from the console poll
path did not stop the resets - that experiment was removed again.
`board.c: p4_wdt_off()` now disables the SWDT exactly like the IDF chain
(`SWD_CONFIG |= BIT(30)` behind the SWD key) instead of only auto-feeding.

Useful discovery while trying an A/B test: **the reference chain does not flash
a U-Boot image separately.** `uartflash.ps1` writes only 0x2000 (IDF
bootloader), 0x8000 (table), 0x10000 (the hardware loader app), 0x200000
(kernel), 0x900000 (DT) and 0xa00000 (metadata); U-Boot is embedded inside
`esp32p4_linux_hardware_loader.bin`. Flash offset 0x100000 is not used by the
reference chain.

Next: bisect the reset - instrument `main_loop()`/`cli_loop()` (does the CLI
return?) and compare the SPL's PMU/clock hand-off against the IDF loader's,
since the register state of every watchdog already matches.

## Round 25: the reset was a watchdog, and the prompt is now stable

Two things cracked it.

**1. The build pipeline had been silently failing since 05:36.** Adding
`p4_wdt_off()` gave `board.c:143: implicit declaration of function 'writel'`
(missing `#include <asm/io.h>`), `build_sep.sh` failed, but `stage_sram.sh`
piped the build through `tail -1`, so the failure was swallowed and the script
copied the **stale** `u-boot.bin` from 05:36. Rounds 23 and 24 therefore tested
the same binary over and over, which is why every watchdog change looked
ineffective. Fix: include added, and `stage_sram.sh` now runs the build
unpiped with `set -eo pipefail`, so a failed build aborts staging.

**2. Feeding the watchdogs does fix the reset.** With a heartbeat in
`esp_serial_pending()` (a `.` every 20000 polls) the dots ran right up to the
reset, proving the CPU was alive and polling - so it was a watchdog, not a
hang. Wiring `p4_wdt_feed()` (RWDT feed, SWDT feed, and the feed key of all four
TIMG MWDTs) into that same poll path every 2000 polls gives:

```
entering cli
esp32p4> ..........................................   (22 s, no reset)
```

- 22 s capture: **0 ROM banners, 1 prompt, 1040 heartbeat dots**.
- Without the feed: a reset roughly every second.

So the idle prompt is stable now. Follow-ups:

- the feed currently lives in `drivers/serial/serial_esp.c` (guarded by
  `CONFIG_ESP32P4_SRAM_ONLY`); the idiomatic place is
  `CONFIG_HW_WATCHDOG` + `hw_watchdog_reset()` called from `WATCHDOG_RESET()`,
- the heartbeat print should be removed once the port is finished,
- the real objective now is loading the DT and kernel and running `booti`,
  which needs a PSRAM window beyond the ~128 KiB the SPL can map today.

## Round 26: clean watchdog hook, and the prompt can be driven

The watchdog feed is now idiomatic:

- `arch/riscv/cpu/cpu.c` has a `__weak void hw_watchdog_reset(void) {}` default,
  which matters because the ESP32-P4 SPL is a **separate U-Boot build** (its own
  defconfig, no `CONFIG_SPL_BUILD`), so the SPL links the weak stub while
  U-Boot proper uses the board implementation.
- `board/espressif/esp32p4/board.c` implements it: RWDT feed, SWDT feed and the
  feed key of all four TIMG MWDTs (`0x50116000`, key `0x50d83aa1`).
- `drivers/serial/serial_esp.c` calls it from `esp_serial_pending()`, which is
  the only regular activity U-Boot has while idling at the prompt.

Verified: 20 s capture, **0 ROM banners, 1 prompt**, no resets.

A small tool to drive the prompt from Windows now exists:
`C:\Users\evanl\AppData\Local\Temp\p4x\p4_cmd.ps1 -Commands "cmd1;cmd2"`
(resets the board, waits for `esp32p4>`, then sends the commands and captures
the output). `md` and `bdinfo` work over it:

```
esp32p4> md 0x48000000 4
48000000: 00000000 01010101 02020202 03030303
esp32p4> bdinfo
relocaddr   = 0x4ff00000
reloc off   = 0x00000000
fdt_blob    = 0x4ff70000
```

So U-Boot can read PSRAM at 0x48000000 (the first bytes are the SPL's own test
pattern), but **writing at 0x48100000 resets the board**: the mapped PSRAM
window is still only about 128 KiB, exactly as measured from the SPL earlier.

Next round: extend the PSRAM MMU mapping in the SPL. The flash window is already
mapped read-only at 0x40000000 (`md 0x40200000` reads the kernel image), so once
PSRAM beyond 128 KiB works the path is:

```
copy flash 0x900000 -> PSRAM 0x48f00000   (DT)
copy flash 0x200000 -> PSRAM 0x48000000   (kernel)
booti 0x48000000 - 0x48f00000
```

## Round 27: PSRAM MMU programming from the SPL resets the board

Tried extending the PSRAM window from the SPL: added `p4_spl_psram_map()` in
`spl_nor.c` (same ITEM/INDEX protocol as flash, on the MSPI2 "S" view) and
called it right after `p4_spl_psram_init()`:

- S-side page size set to 64 KiB in `0x5008e384`,
- 256 entries cleared, then `i | SOC_MMU_PSRAM_VALID | SOC_MMU_ACCESS_PSRAM`,
- `ROM_CACHE_INVALIDATE_ALL(CACHE_MAP_L1_ALL)` afterwards.

The SPL printed `spl: psram mapped`, but U-Boot never came up again: the board
reset-looped (18 ROM banners) and the prompt never appeared. Reverting the
change restored the working state immediately (0 banners, prompt, and
`md 0x48000000` still reads the SPL's pattern).

Checked against IDF afterwards:

- `mmu_ll.h` for the P4 uses `SPI_MEM_S_MMU_ITEM_INDEX/CONTENT_REG` =
  `DR_REG_PSRAM_MSPI0_BASE + 0x380/0x37c`, and
  `DR_REG_PSRAM_MSPI0_BASE = DR_REG_HPPERIPH0_BASE + 0x8E000` = **0x5008e000**,
  so the register base we used was right.
- `mmu_hal_init()` calls `ROM_Boot_Cache_Init()` (only when
  `ESP_ROM_RAM_APP_NEEDS_MMU_INIT`), `mmu_ll_set_page_size(0, ...)` for the
  **flash** MMU first, then `mmu_hal_unmap_all()` which unmaps both MMUs, and
  only later maps PSRAM through `mmu_hal_map_region()` after the PSRAM cache
  has been initialised.

So the next attempt should follow that order: set the page size through MMU id 0
(flash), unmap both tables, and only map the PSRAM window after the PSRAM cache
is up (that is, after the MSPI2 cache config written by `psram: cmds`), and
re-check the S-side `MMU_POWER_CTRL` semantics before the first entry write.

## Round 28: PSRAM beyond the first pages still faults

Experiments from the U-Boot prompt (much faster than rebuilding the SPL):

1. Single entry, done by hand:
   `mw.l 0x5008e380 1` (index), `mw.l 0x5008e37c 0xc01` (content = page 1 |
   PSRAM_VALID | ACCESS_PSRAM), then `mw.l 0x48010000 0xa5a5a5a5` and `md`
   read back `a5a5a5a5`.
2. Same for entry 16 (`content 0xc10`, address 0x48100000): **the board resets**
   as soon as that page is touched.

Checked IDF's encoder to be sure the value is right
(`hal/esp32p4/include/hal/mmu_ll.h`, `mmu_ll_write_entry()`):

```c
mmu_val = paddr >> page_shift;
mmu_val |= SOC_MMU_PSRAM_VALID;   /* BIT(11) */
mmu_val |= SOC_MMU_ACCESS_PSRAM;  /* BIT(10) */
REG_WRITE(SPI_MEM_S_MMU_ITEM_INDEX_REG, entry_id);
REG_WRITE(SPI_MEM_S_MMU_ITEM_CONTENT_REG, mmu_val);
```

That is exactly what we write, with the same registers (`0x5008e380/0x5008e37c`),
so the encoding is not the problem.

A full mapping from U-Boot C code (`p4_psram_map(16 MiB)`, 256 entries, printed
`PSRAM mapped 16 MiB`) also resets the board on the first access beyond the
ROM-mapped pages, and adding `Cache_Invalidate_All(0x3f)` right after the writes
resets even earlier - confirming again that the ROM cache-flush helpers must not
be called from this port.

The read-back of entry 1 may well have been served by the data cache, so treat
"PSRAM works at 0x48010000" as unproven too: what is certain is that everything
beyond the ROM-mapped ~128 KiB faults.

All of this is reverted; the chain is back to the verified state (prompt alive,
`md 0x48000000` reads `00000000 01010101`).

Next idea: the PSRAM window is probably limited by the **MSPI2 cache
configuration**, not by the MMU table - `SPI_MEM_S_CTRL/CTRL2/CACHE_SCTRL`
(0x08/0x10/0x40 on 0x5008e000) describe the address window and the PSRAM size.
Compare those against a working IDF boot (`psram: cmds` replayed only a subset),
and check `SPI_MEM_S_MISC` for the "cache size" field before touching the MMU.

## Round 29: the PSRAM MMU block does not answer on our chain

Compared our chain with the working reference (IDF) chain over JTAG, register by
register.

**MSPI2 (0x5008e000) is identical** - CMD/CTRL/CTRL1/CTRL2/CLOCK/USER1/USER2,
`CACHE_FCTRL` 0x3c = 0x40000001, `CACHE_SCTRL` 0x40 = 0x02f7c679, the SRAM
command registers 0x44..0x54, PMS 0x100..0x10c = 3 and 0x160 = 0 - all match the
reference boot where PSRAM works.

**The MMU entries answer on the reference and not on ours.** Reading an entry is
"write the index to `+0x380`, read `+0x37c`" (exactly what IDF's
`mmu_ll_read_entry()` does):

| entry | reference chain | our chain |
|---|---|---|
| 0 | `00000c00` | `000001a0` |
| 1 | `00000c01` | `00001510` |
| 16 | `00000c10` | `00000e9d` |
| 255 | `00000cff` | `00000d73` |

So the reference has the expected `page | 0xC00` entries for the whole 16 MiB,
while our reads are garbage - the S-side MMU is not responding at all.
`MMU_POWER_CTRL` (0x384) reads `0x13200004` on **both** chains, so the page size
and power bits are not the difference.

**HP_SYS_CLKRST (0x500e6000) differs**, and these are the clock enables IDF sets
that our SPL does not:

| offset | our chain | reference |
|---|---|---|
| 0x14 | `e6df97ff` | `e6dfb7ff` (bit 13) |
| 0x18 | `0e040004` | `0e041034` (bits 12, 5, 4) |
| 0x1c | `60a000be` | `60a020be` (bit 13) |
| 0x2c | `00000000` | `00000100` (bit 8) |
| 0x38 | `00000492` | `00000000` (we set the cache force-on bits here) |
| 0x3c | `00000000` | `000002a3` |
| 0x40 | `00000000` | `08000000` (bit 27) |

Setting only 0x40 bit 27 from the prompt is not enough (the board still resets on
the first access past the ROM-mapped pages), so the remaining enables are among
0x2c bit 8 and 0x3c = 0x2a3 (they look like the cache/PSRAM clock dividers).

Next round: replicate the whole IDF `HP_SYS_CLKRST` state (0x14 |= BIT(13),
0x18 |= 0x1030, 0x1c |= BIT(13), 0x2c |= BIT(8), 0x3c = 0x2a3, 0x40 |= BIT(27)) in
the SPL before the PSRAM cache commands, then re-test the MMU read-back - if the
entries read back as `page | 0xC00` the window is available and the kernel load
path can go ahead.

## Round 30: clkrst and PMU comparison

Mirrored the reference `HP_SYS_CLKRST` bits in the SPL
(`spl_psram.c: psram_clkrst_fixup()`: 0x14 |= BIT(13), 0x18 |= 0x1030,
0x1c |= BIT(13), 0x2c |= BIT(8), 0x3c = 0x2a3, 0x40 |= BIT(27)), added a one-shot
MMU read-back print, and retested:

```
psram: clock
psram: replay
psram: cmds
mmu0 0x000001a0 mmu1 0x00001118     <- still junk, reference reads 0xc00/0xc01
```

So the clkrst bits alone do not wake the PSRAM MMU block.

Dumped the PMU (`0x50115000`) on both chains. **Lots of differences**, e.g.

| offset | our chain | reference |
|---|---|---|
| 0x14 | 00000000 | 7f800000 |
| 0x18 | 00500000 | 02ec0000 |
| 0x1c | 00000000 | 210000a0 |
| 0x20 | 00000000 | ffffffff |
| 0x24 | 00000000 | 08000000 |
| 0x28 | c6677180 | a04037f0 |
| 0x80 | 00500000 | c0800000 |
| 0x84 | 00000000 | 92800200 |
| 0x88 | 00000000 | ffffffff |
| 0x8c | 00000000 | 30000000 |
| 0x90 | c6670000 | c0640000 |
| 0x9c | 00000000 | e8400000 |
| 0x1c0 | 40200000 | 40200180 |
| 0x1c4 | a0000000 | 97000000 |
| 0x1d4 | a0000000 | 86800000 |

Caveat: the reference dump was taken with Linux running, so some of these are
runtime power-management values rather than bootloader ones. The next comparison
must be taken during the IDF **loader** phase (halt a few hundred ms after reset,
before Linux starts) so the PSRAM/analogue power state can be copied faithfully -
`0x80..0x9c` and `0x1c0..0x1d4` look like the PSRAM PHY/LDO configuration that
our `psram: replay` tables do not cover.

## Round 31: reference state captured during the IDF loader phase

Halting 400 ms after a JTAG reset (`reset`, `sleep 400`, `halt`) lands inside the
IDF loader (PC in PSRAM, e.g. `0x48ed3b4c`), i.e. after its PSRAM init and before
Linux. That gives the correct reference state - and it matches the Linux-time
dump, so these really are bootloader values:

```
PMU 0x50115000
 0x014 7f800000   0x018 02ec0000   0x01c 210000a0   0x020 ffffffff
 0x024 08000000   0x028 a04037f0   0x080 c0800000   0x084 92800200
 0x088 ffffffff   0x08c 30000000   0x090 c0640000   0x09c e8400000
 0x1c0 40200180   0x1c4 97000000   0x1d0 40200180   0x1d4 86800000

HP_SYS_CLKRST 0x500e6000
 0x000 00000001   0x004 00000000   0x008 00000001   0x00c 00010000
 0x010 00000000   0x014 e6dfb7ff   0x018 0e041034   0x01c 60a020be
 0x020 00000008   0x024 02011309   0x028 40170503   0x02c 00000101
 0x030 0000d03d   0x034 00000401   0x038 00000000   0x03c 000002a3
```

and the PSRAM MMU entries read cleanly there (`entry 0 = 0xc00`,
`entry 16 = 0xc10`).

`spl_psram.c` now applies all of it (`psram_clkrst_fixup()` + a
`psram_pmu_fixup()` table) before `psram_cache_cmds()`. Result: the SPL still
reads junk from the MMU (`mmu0 0x1a0`, `mmu1 0x1510`) and any access past the
ROM-mapped pages still faults.

So the PMU/clkrst state is necessary but not sufficient; what remains different
must be in how the cache itself is brought up. Note our flash MMU
`MMU_POWER_CTRL` (`0x5008c384`) reads `0x13200014` while the reference reads
`0x13200004` - bit 4 is set only in ours, because our SPL encodes the page size
as `2 << 3`. The reference simply has bit 2. Our flash window works regardless,
but this is the one remaining known difference in the MMU-adjacent registers and
should be the next thing to align (try `2 << 2`/plain `0x04` for the C-side and
check whether the S-side MMU starts answering).

## Round 32: page-size field aligned, clkrst 0x80/0xa4 still differ

`MMU_POWER_CTRL` page-size field was wrong: the SPL encoded 64 KiB pages as
`2 << 3` (bit 4), while the reference has `1 << 2` (bit 2). Fixed in
`spl_nor.c` (`MMU_PAGE_SIZE_MASK` = `3 << 2`, `MMU_PAGE_SIZE_64K` = `1 << 2`).
The flash MMU now reads `0x13200004`, exactly like the reference, and U-Boot
still boots and reaches the prompt.

The PSRAM-side MMU still returns junk (`mmu0 0x5a0`, `mmu1 0x1510`) and still
faults on access past the ROM-mapped pages.

Dumping the reset/clock region next to it (`HP_SYS_CLKRST` 0x80..0xa8) while the
IDF loader runs shows more differences:

| offset | our chain | reference (loader running) |
|---|---|---|
| 0x80 | 01100008 | 00000000 |
| 0x84 | 00010000 | 00000000 |
| 0xa4 | c9000000 | 80000000 |
| 0xa8 | 52400000 | 40000008 |

(0xc0..0xe0 and the 0xdc = 0x640 reset-release value match on both.)

So our SPL leaves clock selectors/dividers at 0x80/0x84 and 0xa4/0xa8 in a state
the reference never has. Next round: identify those fields (they look like the
MSPI/PSRAM clock dividers of `HP_SYS_CLKRST`) and bring them to the reference
values, then re-check whether the PSRAM MMU answers.

## Round 33: the PSRAM MMU responds now - the block was gated by clkrst 0x38

`HP_SYS_CLKRST + 0x38` (`PERI_CLK_CTRL02`) is the gate. Our SPL was setting
`CACHE_CTRL0_FORCE_ON` bits there (`0x492`), while the reference boot leaves the
register at **0**. With those bits set, every read of the PSRAM MMU item content
(`0x5008e37c`) returned junk; with them cleared the same read returns the real
entry:

```
psram: clock / replay / cmds
psram mmu0 0x00000c00          <- correct entry, was 0x1a0 before
```

So after all these rounds the PSRAM-side MMU is finally readable and writable
from our chain. What still fails is the actual *access*: reading (or writing)
`0x48100000` right after mapping still resets the board, while `0x48000000`
works.

Also learned this round: `Cache_Invalidate_All` (0x4fc00404) **does** work from
the SPL when called with the same mask the flash init uses (`0x3`, the
`CACHE_MAP_L1_ALL` value), and resets the board when called with `0x3f`. So the
helper itself is fine - the mask matters.

State: the `0x38` write is removed in `spl_nor.c` (the flash window and the
prompt still work), U-Boot writes the 256 PSRAM entries in `board_late_init`
(`P4_UBOOT: psram mmu written`), and the chain is verified working again
(`md 0x48000000` -> `00000000 01010101`). The mapping code that reads a page as
a test was removed again because that read resets the board.

Next: bisect which page is the last one that works (entry 1, 2, 4, 8...) by
mapping and reading one page at a time from U-Boot, and check whether the PSRAM
chip needs 4-byte addressing or a different mode for pages beyond the first
ones - the MMU side is no longer the suspect.

## Round 34: PSRAM never actually answered - everything was cache

Bisected the accessible range from the U-Boot prompt (each miss hangs and the
watchdog resets the board, so the log shows the last address that worked):

```
md 0x48000000 1   -> 00000000       ok
md 0x48000080 1   -> 20202020       ok
md 0x48000100 1   -> (hang, rst:0x7)
md 0x4800c000 1   -> (hang)
md 0x48010000 1   -> (hang)
```

So exactly the **first 256 bytes** are readable and everything after that hangs.
Then it clicked: the "PSRAM works at 0x48000000" result was always the *data
cache*. Our SPL's `psram_check()` wrote its pattern (`00000000 01010101 ...`)
which the cache absorbed; every read afterwards was served from those same
lines. No real PSRAM transaction has ever completed on this chain.

That also explains the earlier contradictions: the hand-written MMU entries read
back correctly (`0xc00`, `0xc10`) once clkrst 0x38 was cleared, but any new
address still hung - the MMU is fine, the chip is not initialised.

Better reference frame: the reference chain's PSRAM *does* work (Linux keeps its
framebuffer at 0x49000000), so the difference is the chip bring-up, not the MMU.

Conclusion: `psram: replay` (replaying captured register values) cannot
initialise the PSRAM chip. The chip needs real SPI command sequences - reset,
enter QPI, read/write the mode/latency registers - issued through the MSPI2
"SRAM command" registers (`0x5008e044..0x54`) the way IDF's `esp_psram_impl`
does. That is the next piece of work, and it is the last blocker between the SRAM
chain and loading Linux out of flash into PSRAM.

State left behind: the board restored to the reference chain, our chain builds
and boots to the prompt (clkrst 0x38 no longer written, U-Boot writes the 256
PSRAM MMU entries, no test reads that hang the board).

## Round 35: the P4 PSRAM is HEX (16-bit) and needs real transactions

Studied IDF's driver
(`components/esp_psram/device/esp_psram_impl_ap_hex.c`, 605 lines) - the P4's
in-package PSRAM is **hex (16 data line) DDR PSRAM**, not quad/oct like the older
chips. That is why replaying captured register values could never work: the chip
needs its **mode registers written over the bus**, and nothing we do touches them.

The good news: the transactions are issued through **ROM functions**, so we do
not have to implement the SPI engine:

```
esp_rom_spi_set_op_mode = 0x4fc00110
esp_rom_spi_cmd_config  = 0x4fc00108
esp_rom_spi_cmd_start   = 0x4fc0010c
```

### Constants (from esp_psram_impl_ap_hex.c)

```
AP_HEX_PSRAM_SYNC_READ    0x0000    AP_HEX_PSRAM_SYNC_WRITE   0x8080
AP_HEX_PSRAM_BURST_READ   0x2020    AP_HEX_PSRAM_BURST_WRITE  0xA0A0
AP_HEX_PSRAM_REG_READ     0x4040    AP_HEX_PSRAM_REG_WRITE    0xC0C0
AP_HEX_PSRAM_RD_CMD_BITLEN 16   AP_HEX_PSRAM_WR_CMD_BITLEN 16
AP_HEX_PSRAM_ADDR_BITLEN   32
AP_HEX_PSRAM_CS_SETUP_TIME 4    AP_HEX_PSRAM_CS_HOLD_TIME 4
AP_HEX_PSRAM_CS_HOLD_DELAY 3    AP_HEX_PSRAM_VENDOR_ID_AP 0xD
```

Timing set (200 MHz class): `RD_DUMMY = 2*(14-1)`, `RD_REG_DUMMY = 2*(7-1)`,
`WR_DUMMY = 2*(7-1)`, `RD_LATENCY = 4`, `WR_LATENCY = 1`, MPLL default 400 MHz.

Mode register bits used: `mr0.lt = 1`, `mr0.read_latency = RD_LATENCY`,
`mr0.drive_str = 0`, `mr4.wr_latency = WR_LATENCY`, `mr8.bl = 3`, `mr8.bt = 0`,
`mr8.rbx = 1`, `mr8.x16 = 1` (x16 = hex mode; 8-line mode would clear it).

### Sequence (`esp_psram_impl_enable`)

1. MPLL acquire + set to `MPLL_DEFAULT_FREQ` (400 MHz), or reuse the MPLL our
   SPL already starts.
2. `PERIPH_RCC` on MSPI2: enable module clock, reset it, select MPLL as PSRAM
   clock source (also for MSPI3).
3. `mspi_timing_ll_pin_drv_set(2)`, `mspi_timing_ll_enable_dqs(true)`.
4. CS setup/hold/hold-delay; `enable_split_trans(true)`; `set_page_size(2048)`.
5. Slow bus clock: `set_bus_clock(mspi, MPLL/SPIRAM_SPEED)`; `enable_dll(true)`.
6. **Mode register init over MSPI3** (`s_init_psram_mode_reg`): read MR0+MR1
   (addr 0), read MR4+MR8 (addr 4), modify, write MR0 (addr 0), write MR4
   (addr 4), read MR8 (addr 8), modify, write MR8.
7. `s_check_psram_connected()` (reads MR and compares known bits), read MR1
   vendor id (expect 0xD AP or 0x1A UnilC), read MR2 density (0x7 = 32 MB).
8. `s_config_mspi_for_psram()`: wr cmd `SYNC_WRITE`, rd cmd `SYNC_READ`,
   4-byte address, RD/WR dummy, variable dummy, DDR mode on, oct + hex line mode
   on, AXI access + wr/rd splice on.
9. `mspi_timing_psram_tuning()` - the timing tuning step; if that is too much to
   port, run the bus at a low clock where the defaults hold.

### Transaction shape

```c
esp_rom_spi_set_op_mode(mspi, ESP_ROM_SPIFLASH_OPI_DTR_MODE);
esp_rom_spi_cmd_t conf = { .cmd = cmd, .cmdBitLen = cmd_bitlen,
                           .addr = &addr, .addrBitLen = addr_bitlen,
                           .txData = mosi, .txDataBitLen = mosi_bitlen,
                           .rxData = miso, .rxDataBitLen = miso_bitlen,
                           .dummyBitLen = dummy_bits };
esp_rom_spi_cmd_config(mspi, &conf);
esp_rom_spi_cmd_start(mspi, miso, miso_bitlen / 8, 1 << 1 /* cs1 */, false);
```

First milestone for the next round: from the SPL, read MR1 over MSPI3 and print
the vendor id - if that returns `0xD`, we are talking to the chip and the rest of
the init is mechanical.

## Round 36: the transaction scaffolding already exists, only the transport is missing

`spl_psram.c` already carries the hex PSRAM groundwork from an earlier round
(the ROM SPI helpers at lines 72-90, `psram_transaction()`,
`psram_mode_register_init()` with the MR0/MR4/MR8 latency programming), but:

* `psram_mode_register_init()` is **never called** from `p4_spl_psram_init()`,
* `psram_transaction()` is compiled out with `#ifndef P4_PSRAM_USE_ROM_OPI`,
  and the reason is recorded right there:

```c
/*
 * The ROM OPI helper spins forever on the P4 when driven from the SPL
 * (esp_rom_spi_cmd_start never returns), so the transaction path below
 * is disabled until the user-mode sequence is implemented locally.
 */
```

So a previous round already tried the ROM route and hit the same wall: the ROM's
`esp_rom_spi_cmd_start` waits on a "transaction done" condition that never comes
when the SPL calls it.

Naming the real task: the SPL needs its own **user-mode SPI transaction on
MSPI3** - write `SPI_MEM_S_CTRL`, `USER`, `USER1` (16 bit command, 32 bit
address, dummy), `USER2`, `MISO_DLEN`/`MOSI_DLEN`, push the command word into
`SPI_MEM_S_W0`, then poll `SPI_MEM_S_CMD.USR` until it clears, the way
`spi_mem_ll_usr_*` does in IDF. The register block is `0x5008f000`.

Orders of work for the next round:

1. Read `SPI_MEM_S_CMD`/`USER`/`USER1` on the **reference** chain (IDF loader
   phase) to see the exact clock/CS timing state the working driver leaves,
   since our replay may not cover the MSPI3 side.
2. Implement `psram_txn_local()` and use it to read MR0|MR1 and check for
   vendor id 0x0d - that is the "we are talking to the chip" milestone.
3. Then wire `psram_mode_register_init()` in, re-run `psram_check()` and see
   whether the pattern survives at an address past 0x48000100.

Everything else (clkrst gate, MMU wiring, PMU state) is already in place, so this
is the last mechanical piece before Linux can be loaded into PSRAM.

## Round 37: local MSPI3 transaction works, chip still answers zeros

Wrote `psram_txn_local()` (user-mode transaction on `0x5008f000`) instead of the
ROM helper that hangs, and a probe that reads MR0|MR1 with several dummy
lengths. The transaction now **completes** (the `USR` bit clears; before, the ROM
path spun forever), but the returned data is zero for every dummy setting:

```
psram probe: d8=00000000 d12=00000000 d16=00000000 d20=00000000
             d24=00000000 d28=00000000
```

Register fields used (hw_ver3 `spi_mem_s_reg.h`):

```
CMD  (0x00) bit18 USR (start / busy)
USER (0x18) bit27 MOSI, 28 MISO, 29 DUMMY, 30 ADDR, 31 CMD, 26 DUMMY_IDLE
USER1(0x1c) [5:0] DUMMY_CYCLELEN, [8:6] DBYTELEN, [31:26] ADDR_BITLEN
USER2(0x20) [15:0] COMMAND_VALUE, [31:28] COMMAND_BITLEN
0x24 MOSI_DLEN, 0x28 MISO_DLEN, 0x40.. W0..W15
```

Cross-check against the reference (IDF loader, PSRAM working):

| reg | ours (after probe) | reference |
|---|---|---|
| CTRL 0x08 | 002ca27c | 002ca27c |
| CTRL1 0x0c | 00000ffc | 00000ffc |
| CLOCK 0x14 | 00010001 | 00010001 |
| MISC 0x34 | 00000002 | 00000002 |
| USER 0x18 | f4000000 | f0000000 |
| USER1 0x1c | 7c00000e | 7c000019 |

So the controller configuration matches; the difference is only in the leftover
USER bits of the *last* transaction (the reference's last one was a 512 bit
tuning read with `dummy_cyclelen` 25).

Since the address/command phases evidently go out (the transaction completes) but
nothing comes back on MISO, the next suspect is the **pin/pad configuration**:
IDF calls `mspi_timing_ll_pin_drv_set(2)` and enables DQS before talking to the
PSRAM, and the hex PSRAM has 16 dedicated data lines plus DQS. Our SPL never
configures those pads for MSPI3, so the command may reach the chip while the data
lines are not routed back.

Next round: compare the IO-MUX / pad registers that IDF touches for the PSRAM
(`mspi_timing_ll_pin_drv_set`, `*_dqs`, the `HP_SYS_CLKRST` "PSRAM pad" fields)
between the two chains, then re-run the probe.

## Round 38: the PSRAM pads are unconfigured, and writing them blindly is unsafe

Diffed the IO-MUX block (`0x500e1000..0x500e1240`) between our chain and the
reference (IDF loader phase). **61 registers differ**, and the pattern is
unmistakable: the reference has bit 1 set on every PSRAM pad while we leave the
pads at their reset value.

| address | ours | reference | pads |
|---|---|---|---|
| 0x500e1020, 0x500e1024 | 00000800 | 00001b02 | clock / cs |
| 0x500e1028 .. 0x500e1060 | 00000800 | 00000802 | data lines |
| 0x500e1064, 0x500e1068 | 00000c00 | 00000c02 | data lines |
| 0x500e10a0 .. 0x500e10c4 | 00000800 | 00001802 | data lines |
| 0x500e1204 .. 0x500e1218 | 00000000 | 00000010 | pad config |
| 0x500e121c .. 0x500e1238 | 00000000 | 00002020 | pad config |
| 0x500e123c | 00000000 | 00010001 | pad config |

So the pads really are a missing piece. Tried writing the whole reference state in
`psram_pads_init()` before the probe: the probe still read zeros **and** the board
started reset-looping (16 ROM banners), so writing that range verbatim is not
safe - some of those registers are not plain pad configuration (the `0x2020`
patterns look like GPIO output/OE registers for other pins).

Reverted; the chain is verified working again (`md 0x48000000` ->
`00000000 01010101`, no reset).

Next round: narrow the range. Start with just the IO-MUX registers
`0x500e1020..0x500e10dc` (bit 1 = output enable plus drive strength) and leave
`0x500e1200+` alone, then check the probe; if the chip still answers nothing, look
at what IDF's `mspi_timing_ll_pin_drv_set(2)` actually writes instead of copying
whole registers.

## Round 39: the probe was reading the wrong register - no W/BUF buffer exists there

Added only the IO-MUX part of the pad diff (`0x500e1020..0x500e10dc`) and kept
`0x500e1200+` untouched: no more reset loop (0 banners), the chain boots, but the
probe still returned zeros for every dummy from 4 to 60 bits.

Then found the real reason the probe always read zero:
`0x5008e040` is **`SPI_MEM_S_CACHE_SCTRL_REG`**, not a data buffer, and this
register map has **no `W0..W15`/`BUF0..15` registers at all**:

```
0x00 CMD      0x08 CTRL    0x0c CTRL1   0x10 CTRL2   0x14 CLOCK
0x18 USER     0x1c USER1   0x20 USER2   0x2c RD_STATUS
0x34 MISC     0x3c CACHE_FCTRL          0x40 CACHE_SCTRL
0x44 SRAM_CMD 0x48 SRAM_DRD_CMD         0x4c SRAM_DWR_CMD
0x50 SRAM_CLK 0x54 FSM
0xc0.. INT, 0xd4 DDR, 0x100+ PMS regions
```

So `psram_txn_local()` was reading the cache control register as "received data" -
the transaction may well have been working all along, we simply never looked at
the right place, and there is no MOSI/RX staging register to look at: the PSRAM
controller moves payload through the cache/AXI path (or `SRAM_*`), not through a
W buffer like the flash controller does.

The `SRAM_CMD` fields are cache-line oriented (`SCLK_MODE[1:0]`,
`SWB_MODE[9:2]`, `SDIN_DUAL`, `SDOUT_DUAL`, `SADDR_DUAL`, ...), so they are not a
generic transaction path either.

Next round: read the ESP32-P4 TRM section on the PSRAM controller user-mode /
`SPI_MEM_S` command engine to find where the transceive data actually lands
(and how IDF's `esp_rom_spi_cmd_config` moves it), then finish the MR read. The
pads, clocks, MMU gate and controller config are now all in place, so this is the
last unknown.
