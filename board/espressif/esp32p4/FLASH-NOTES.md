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
