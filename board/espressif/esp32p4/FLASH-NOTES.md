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

## Round 40: ported the ESP32-S31 transaction code - still zeros

The important discovery of this round is in our own tree: the **ESP32-S31 port
already contains a complete, working PSRAM implementation**
(`board/espressif/esp32s31/spl_psram.c`, 889 lines: LDO, MPLL, MSPI clock, pin
drive, bus clock, controller init, `psram_xact`, mode registers, AXI config, pin
timing tuning, MMU map). That is the reference to follow instead of reverse
engineering IDF.

From it, the two things the P4 transaction was missing:

* **the data buffer is at `+0x58` (`W0`)**, not `+0x40` (which is
  `CACHE_SCTRL`) - that alone explains every "everything is zero" result;
* **CS1 is selected through `MISC`: set `CS0_DIS` (BIT(0)), clear `CS1_DIS`
  (BIT(1))**, and restore afterwards.

Ported both into `psram_xact()` (plus `psram_opi_dtr_mode()`: CTRL oct/fastrd
bits, USER write-mode bits cleared, `DDR_REG` = `FMEM_DDR_EN`,
`FMEM_DDR_CMD_DIS` cleared) and rebuilt. The unbounded "wait for idle" loop had
to be bounded or the SPL hung and the watchdog reset the board.

Result now: the transactions complete cleanly, the chain still boots
(`md 0x48000000` -> `00000000 01010101`, 0 banners), but **all reads still return
zero**.

Next round: port the S31's `psram_ctrlr_init()` (and `psram_mspi_clock_init`,
`psram_bus_clock`, `psram_pin_init`, `psram_ctrlr_init`) properly instead of
replaying MSPI3 registers, since the replay may leave the controller in a state
that only supports the cache path. The S31 file is the blueprint and it is
already in this tree, so this is now a porting exercise rather than research.

## Round 41: slow clock and MSPI3 timing config both ruled out

From the S31 port's `psram_ctrlr_init()`:

* its register transactions run at `MPLL/20` (~20 MHz), not full speed - our
  replay leaves `CLOCK` at divider 1, so `psram_probe()` now sets
  `0x00130913` (N=19, H=9, L=19) on both MSPI2 and MSPI3 for the probe and
  restores `0x00010001` afterwards. **Result: still zeros.**
* the S31 also has to configure *both* the SMEM (cache) and MEM (command)
  controllers in lockstep or its ROM helper hangs, which suggested the P4's
  `0x100..0x1a0` block for MSPI3 (timing calibration / page size / AC) might be
  missing. Dumped it: on **our chain it reads all zero**, so the block was
  copied from MSPI2 into MSPI3 and retested - **still zeros**.
  Then dumped the same range on the reference chain: it is **all zero there too**,
  so those registers are not used by the command path and the copy was removed
  again.

So the P4's MSPI3 command path is configured, clocked, and its transactions
complete, yet the chip returns nothing. Remaining difference to chase: the
S31/IDF sequence configures *DLL* (`SMEM_S_TIMING_CALI`/`TIMING_CALI` bit 5) and
the CS setup/hold/hold-delay in the AC register (`P4_MSPI3_BASE + 0x1a0`) before
any transaction; we replay those values from a post-init state instead of
programming them, and the reference dump above shows our `/0x1a0` block reads 0.

Next round: dump `0x5008e1a0`/`0x5008f1a0` (AC: CS setup/hold/delay, split-trans)
on both chains and program MSPI3's AC + timing-cali + DLL explicitly the way the
S31 does, then retry the MR read.

## Round 42: WE ARE TALKING TO THE PSRAM CHIP

The missing piece was the **dedicated MSPI pin mux block**, not the GPIO IO-MUX:

```
DR_REG_IOMUX_MSPI_PIN_BASE = DR_REG_HPPERIPH1_BASE (0x500c0000) + 0x21200
                           = 0x500e1200
```

Reference (working) state of that block:

| offset | value | pins |
|---|---|---|
| 0x500e1200 | 00000001 | CLK_EN0 (block clock) |
| 0x500e1204..0x500e1218 | 00000010 | flash CS/Q/WP/HOLD/CK/D |
| 0x500e121c..0x500e1238 | 00002020 | PSRAM D, Q, WP, HOLD, DQ4..DQ7 |
| 0x500e123c | 00010001 | PSRAM DQS_0 |
| 0x500e1240..0x500e1264 | 00002020 | PSRAM DQ8..DQ15 |
| 0x500e1268 | 00010001 | PSRAM DQS_1 |
| 0x500e1280.. | same again | second CS instance |

Our chain had **all of these at zero** (the round-38 "reset loop" came from writing
the *flash* pins in the same batch - 0x500e1204..0x500e1218 - which is why the
write must skip them).

With only the PSRAM half of the block programmed (`psram_pin_mux_init()`), the
probe reads real data:

```
psram probe: d8=0x00008d08 d12=0x00008d08 d16=0x00008d08 d20=0x00008d08
             d24=0x00008d08 d28=0x00008d08
```

`0x8d08` = MR0 0x08 | MR1 0x8d, and MR1 bits[4:0] = **0x0d = AP vendor id**. The
chip answers. (Note it answers for every dummy length, so the read latency is
tolerant at this clock.)

Next round: program MR0/MR4/MR8 with the real values, configure MSPI2 for the
cache path, and check that a pattern written at 0x48100000 and 0x49000000
survives - then the Linux load path is finally unblocked.

## Round 43: mode registers programmed, PSRAM read/write works inside the ROM window

Added `psram_mode_reg_init()` (MR0 lt/read latency, MR4 write latency, MR8
bl/bt/rbx/x16 = hex mode) using the now working `psram_xact()`, plus
`psram_rw_test()`:

```
psram probe: d8=0x00008d08 ... d28=0x00008d08     (AP vendor id 0x0d)
mode: mr0 0x00008d28 mr4 0x8d08a05f mr8 0x8d088d4b
 rw 0x48001000=0xa5a50000 rw 0x48010000=0xa5a50001
```

So the chip answers, the mode registers are written, and cached read/write works
at 0x48001000 and 0x48010000 - well past the old 0x100 byte "works" limit.

Beyond that it still faults:

* `rw 0x48100000` (page 16) resets the board - even after the SPL writes all 256
  MMU entries (`psram_mmu_map()`: index -> `i | BIT(11) | BIT(10)`), and even
  when the access is made later from U-Boot, which already maps 16 MiB in
  `board_late_init()`.

So the MMU table is not what limits the window. The S31 port shows what is
missing: its `psram_axi_config()` configures the cache/AXI path before mapping -

* `SRAM_DWR_CMD` / `SRAM_DRD_CMD` = sync write/read opcode + bit length,
* `CACHE_SCTRL` = 4-byte address + RD/WR command enables + RD/WR dummy enables
  and cycle lengths + address bit length + octal/hex mode bit,
* `SRAM_CMD` |= CMD/ADDR/DOUT/DIN octal bits + `SDUMMY_WOUT`,
* `DDR_REG_AXI` = DDR enable + variable dummy,
* `CACHE_FCTRL_SMEM` |= `AXI_REQ_EN`,
* `CTRL1` |= read/write splice enables.

Our `psram_cache_cmds()` only replays the register values a post-init IDF boot
has, which is evidently not enough to open the AXI window for the MMU entries we
program.

Next round: port `psram_axi_config()` to the P4 (hex instead of octal bits, the
P4's `AP_HEX` opcodes/dummy values) and retest `0x48100000` / `0x49000000`.

## Round 44: PSRAM IS ALIVE - full window read/write works

Ported `psram_axi_config()` from the S31 (`psram_axi_config()`) and it is the
last piece that was missing. With the **octal** variant of the `SRAM_CMD` bits
(the hex variant reset the board):

```
axi: sctrl 0x02d7c679
 rw 0x48010000=0xa5a50000 rw 0x48100000=0xa5a50001 rw 0x49000000=0xa5a50002
mmu0 0x00000c00 mmu1 0x00000c01
```

So cached read/write now works at 1 MiB **and** 16 MiB - the whole window, not
just the ROM-mapped 128 KiB. The sequence that works, in order:

1. `psram_pads_init()` (GPIO IO-MUX) + `psram_pin_mux_init()` (the real MSPI pin
   block at 0x500e1200),
2. `psram_probe()`: slow bus clock, `psram_opi_dtr_mode()`, MR read (vendor 0x0d),
3. `psram_mode_reg_init()` (MR0/MR4/MR8),
4. `psram_cache_cmds()` (replayed post-init values),
5. `psram_axi_config()`: `SRAM_DWR/DRD_CMD` opcodes+bitlen, `CACHE_SCTRL`
   (4-byte addr, RD/WR cmd, RD/WR dummy, addr bitlen, mode bit),
   `SRAM_CMD` |= oct cmd/addr/dout/din + `SDUMMY_WOUT`, `DDR_AXI` = DDR+var dummy,
   `CACHE_FCTRL` |= AXI_REQ_EN, `CTRL1` |= RD/WR splice,
6. `psram_mmu_map()` (256 entries), then the read/write test.

Also done: the flash window was widened from 8 MiB to 16 MiB, so the device tree
at flash 0x900000 is readable (`edfe0dd0`, size 0x113d) and the kernel header at
0x200000 gives `image_size = 0x354c60`.

### The remaining problem: bulk copies into PSRAM reset the board

From the U-Boot prompt:

```
mw.l 0x48f00000 0xdeadbeef 4      -> reads back, fine
cp.b 0x40900000 0x48f00000 0x40   -> works
cp.b 0x40900000 0x48f00000 0x113d -> resets (rst:0x7)
cp.b 0x40900000 0x48f00000 0x1000 -> resets
```

Watchdogs are now switched **off** in `hw_watchdog_init()` (RWDT `wdt_en` and
`flashboot_mod_en` cleared, SWDT `swd_disable` set, keys handled), so this is a
real fault/hang in the bulk write path, not a watchdog timeout. Next round: find
the copy-size boundary (256 B / 512 B / 1 KiB / 2 KiB) and look at what the
cache/AXI path needs for multi-line writes (likely `CTRL1` splice or the DDR
write dummy/write latency in MR4).

## Round 45: bulk writes inside the SPL are fine, U-Boot's `cp` is broken

Bisected the "bulk copies reset the board" problem:

* **PSRAM bulk writes from the SPL are fine**: 64 B, 256 B, 1 KiB, 4 KiB and
  16 KiB all verify (`bulk 0x40=ok ... bulk 0x4000=ok`) at `0x48100000`.
* **`mw.l` with 0x400 words also works from U-Boot** (4 KiB written and read
  back).
* **`cp.b` is what breaks**, and it is a U-Boot bug, not PSRAM:

```
esp32p4> cp.b 0x40900000 0x48e00000 0x800
Unhandled exception code: 939524097
EPC: 00000000 RA: 00000000 TVAL: 00000000
```

`cp` jumps to address 0. Workaround: do the load from the SPL.

Added `p4_spl_psram_load_payload()`, called right after the flash MMU is set up
in `spl.c`:

* kernel from flash 0x200000 (32-bit ARM64 `image_size` at +0x10),
* device tree from flash 0x900000 (`totalsize` at +0x04),
* copying flash -> PSRAM directly hung, so it now bounces through a 4 KiB SRAM
  buffer.

That copy *runs* (progress dots every 64 KiB, about 178 of them before the board
resets with `rst:0x7`), so two things still need fixing:

1. the size it computes looks too large for a 3.5 MB payload - print `ksize` and
   `dsize` *before* copying,
2. the HP watchdog still fires during the copy even though the function clears
   RWDT `wdt_en`/`flashboot` and sets SWDT `swd_disable`; find which watchdog
   actually produces `HP_SYS_HP_WDT_RESET` in the SPL (the SM1 `hw_watchdog_reset()`
   feeds RWDT, SWDT and the four TIMG MWDTs - feeding all of them inside the copy
   loop is the cheap fix).

After that, `booti 0x48000000 - 0x48f00000` from the prompt should finally start
Linux with the kernel and DT already resident in PSRAM.

## Round 46: PSRAM writes are not reaching the chip

Progress on the payload path:

* the FDT `totalsize` is big endian: `dtb[1]` must be byte-swapped
  (`0x3d110000` -> `0x0000113d`), same for the other FDT header fields,
* the sizes are now sane (`kern 0x00354c60`, `dtb 0x0000113d`) and the
  word-at-a-time copy completes the whole 3.5 MB (14 progress dots).

But the verification fails:

```
src         0x0000a045 0xedfe0dd0      (flash, correct)
after copy  0x00000000 0x04000000      (PSRAM)
```

So the copy "runs" and the data does not land. Re-checking the earlier
"PSRAM works" evidence: every test we ran was **write followed immediately by
read of the same address** (`rw 0x48100000=...`, `bulk 0x4000=ok`, `mw.l` +
`md.l`). All of those can be satisfied by the write buffer / cache without the
data ever reaching the chip. Reading back *different* addresses after a copy is
the first test that actually proves the write path, and it fails.

Conclusion: **reads work, writes are dropped**. The write path to fix:

* `SRAM_DWR_CMD` opcode/bit length (currently 0x8080 = sync write, 16 bit),
* `MR4` write latency (we set 1; the AP hex part wants 1..2 depending on clock),
* the write dummy length (`P4_PSRAM_WR_DUMMY` = 2*(7-1) = 12 bits),
* `CACHE_SCTRL` write command enable + `SDUMMY_WOUT` in `SRAM_CMD`,
* possibly `DDR_AXI` write-data swap.

Next round: read a known PSRAM location that the *previous* boot wrote (e.g. write
a pattern, power-cycle, read it back) to confirm the write path, then fix the
write configuration - that is the last piece before `booti`.

## Round 47: writes are dropped and reads look synthetic

Persistence probe in the SPL: write `0xcafebabe` at `0x49f00000` and
`0xdead1234` at `0x48080000`, then reboot and read the same addresses back
*before* writing anything:

```
boot A: stale a=0xca2aca2a b=0x00000000 c=0xca2aca2a d=0xaaeaeaaa
boot B: stale a=0xca2aca2a b=0x00000000 c=0xca2aca2a d=0xaaeaeaaa
```

Identical across power cycles, so:

* the values written in the previous boot are **not** there -> writes never reach
  the chip,
* the values that *are* read are constant bit patterns (`0xca2aca2a`,
  `0xaaeaeaaa`) unrelated to anything we wrote -> the read path is very likely
  synthetic as well (a fixed filler), not real PSRAM content.

That also explains the whole history: `md 0x48000000` returning
`00000000 01010101 ...` was the cache serving our own writes, and every "PSRAM
works" result was a same-address write/read pair.

Verified the write-side register layout against IDF's `psram_ctrlr_ll` and it
matches what we program (`SRAM_DWR_CMD`: value [15:0], bitlen [31:28];
`CACHE_SCTRL`: SADDR_4BYTE 0, WR_DUMMY 3, RD_DUMMY 4, RCMD 5, RDUMMY [11:6],
ADDR_BITLEN [19:14], WCMD 20, OCT 21, WDUMMY [27:22]; `SRAM_CMD` hex bits 26/27;
`SMEM_DDR` DDR_EN bit0 / VAR_DUMMY bit1). So the values are right but the effect
is not - something *else* gates the data path.

Next round: with the reference chain running (Linux using the framebuffer at
`0x49000000`, i.e. PSRAM genuinely working), dump MSPI2 again across the whole
register file - `0x00..0x60`, `0x100..0x1b0`, `0xd0..0xe0` - and diff against our
chain, this time *including* the fields we have not looked at yet
(`CACHE_SCTRL` upper bits, `AES`/`DECRYPT` control, `INT_*`, `DLL_*`). The data
path is clearly gated by something we have not programmed.

## Round 48: only four registers differ, and the data path still fails

Dumped the whole MSPI2 file (`0x00..0x5c`, `0x100..0x1ac`, `0xd0..0xec`,
`0x370..0x38c`) on both chains - the reference with Linux running and using the
framebuffer in PSRAM - and diffed. **Only four registers differ:**

| reg | ours | reference | meaning |
|---|---|---|---|
| 0x5008e040 | 02d7c679 | 02f7c679 | `CACHE_SCTRL`: ref also sets bit 21 (`SRAM_OCT`) |
| 0x5008e044 | 80fc0000 | 8cfc0000 | `SRAM_CMD`: ref also sets bits 26/27 (`SDIN_HEX`/`SDOUT_HEX`) |
| 0x5008e170 | 00d6ffc0 | 00000000 | replay artifact, not used by the reference |
| 0x5008e1a0 | 8000b084 | 8400b18f | AC: CS setup/hold/hold-delay values |

Tried setting the reference's OCT+HEX bits: **reset loop** (16 banners), so those
bits are only valid in the state the reference reaches *after* a successful
bring-up, not before it. Reverted.

Applied the other two (`0x1a0` = 0x8400b18f, `0x170` = 0): the board still boots
cleanly (0 banners) but the persistence probe is unchanged:

```
stale a=0xca2aca2a c=0xca2aca2a d=0xaaeaeaaa      (same on every boot)
```

So the register file is essentially identical to a working boot, yet no data
moves. What the reference has that we have not done is the **timing/DQS
calibration** (`mspi_timing_psram_tuning()` in IDF, `psram_timing_tune()` in the
S31 port): it sweeps the DQS phase (`PSRAM_DQS_REG` = MSPI pin block + 0x3c,
bits [2:1]) and the DQS-minus-data delay taps ([14:7] and [21:18]) while reading
a reference pattern, and only after that does the bus return real data. Constant
"junk" like `0xca2aca2a` is exactly what a mis-sampled bus looks like.

Next round: port `psram_tune_pins()` + `psram_timing_tune()` from the S31 port
against the P4's hex part (DQS register at 0x500e123c, reference pattern written
at 0x80 in PSRAM), and use the result as the final bring-up step before the MMU
map and the payload load.

## Round 49: DQS calibration ported, but the reference write does not verify

Ported the S31's calibration to the P4 in full (`psram_tune_pins`,
`psram_tune_read`, `psram_timing_tune`):

* DQS phase/delay written to the MSPI pin block: `DQS_0` at 0x500e123c and
  `DQS_1` at 0x500e1268, phase bits [2:1], delay bits [14:7] and [21:18],
* data-pin delay (bits [7:4]) on the 16 data pins 0x500e121c..0x500e125c plus CS
  (0x500e1260) and CK (0x500e1264),
* a 32-word xorshift reference written at PSRAM 0x80 with `SYNC_WRITE` (512 bits)
  at the slow command clock, read back with `SYNC_READ`,
* the 4-phase sweep and the 31-tap x 20-trial sweep with the widest-window
  midpoint selection, then a write-verify at the selected point.

It stops at the first step:

```
tune: low-clock reference failed
```

The reference pattern written through `psram_xact()` (the direct command path, not
the cache) does not read back, even at the slow clock. That is consistent with
everything else: **the write path is dead**, both through the cache and through
the command engine, while reads return constant junk.

So the ordering has to be: get *any* write to land first (a single 16-bit mode
register write is the smallest possible test - MR0 is writable and reads back, and
we already program it, so check whether that read-back is genuine by writing a
different latency value and reading it again), then the calibration, then the MMU
map.

The code for the calibration is in place and can be re-enabled with one line once
a write verifies.

## Round 50: mode-register writes persist, array accesses do not

Two facts, both measured:

* **Mode-register writes do reach the chip and persist across a reset.** Every
  boot after the first probes MR0|MR1 as `0x8d28`, while a virgin boot reads
  `0x8d08` - exactly the `lt` + read-latency bits `psram_mode_reg_init()`
  programs. So the command path transmits correctly, in both directions, for the
  16-bit register transfers.
* **Array accesses fail.** A single 32-bit word written at PSRAM 0x80 / 0x84
  through the command path reads back `0xffffffff`, with both the sync opcodes
  (`0x8080`/`0x0000`) and the burst opcodes (`0xa0a0`/`0x2020`):

```
wordtest wrote 0x1234abcd read80 0xffffffff read84 0xffffffff
tune: low-clock reference failed
```

So the register (mode) transfer path works, the array (data) path does not - the
data phases are the problem, not the command/address phases. That points at the
data-line width/DDR handling or the DQS sampling for data bursts, i.e. exactly
the part that IDF's driver handles with `mspi_timing_psram_tuning()` and the
hex-data-line-mode settings.

### Plan

Stop guessing register by register and **port IDF's
`components/esp_psram/device/esp_psram_impl_ap_hex.c` essentially verbatim** into
the SPL (it is 605 lines and self-contained apart from a handful of
`psram_ctrlr_ll` helpers that can be inlined): its `s_config_mspi_for_psram()`,
`s_init_psram_mode_reg()`, `s_check_psram_connected()`, `mspi_timing_psram_tuning()`
and the final AXI setup are the code that is known to work on this exact silicon,
and it removes the remaining guesswork about data-line mode and dummy cycles.

## Round 51: PSRAM array access works - the read dummy was wrong

The read dummy was the whole problem. Writing a repeating pattern and reading
it back with a **sweep of the `USER1` dummy-cycle field** (`psram_xact` passes
cycles, not bits):

```
d26=0xffffffff/0xaaaaaaaa   d34=0xffffffff/0xaaaaaaaa
d42=0xaaaaffff/0xaaaaaaaa   d50=0xaaaaaaaa/0xaaaaaaaa   <- aligned
d58=0xaaaaaaaa/0xaaaaaaaa   d66=0xffffffff/0xaaaaaaaa
```

So the data phase needs **50 cycles of dummy** at the slow command clock
(MPLL/20), i.e. roughly double the 25-26 the reference has at full speed - the
chip's latency is constant in *time*, so the cycle count scales with the clock.
With `dummy = 50`:

```
pattern read 0xaaaaaaaa/0xaaaaaaaa      (write verified through the array!)
```

This also means the writes were landing all along; only the read sampling was
off, which is why everything looked like "writes dropped, reads synthetic".

Wired in:

* `psram_tune_read()` uses 50 cycles at the slow clock and switches to 25 after
  the bus clock goes to full speed,
* the word/pattern test reads use 50.

Remaining in the tune: `tune: low-clock reference failed` still appears even
though the 4-word pattern verifies, so the 32-word reference comparison needs the
first mismatching word printed - the 128-byte reference spans two 16-word
command transactions and one of them is probably sampled differently.

Next round: print the first mismatching word in `psram_tune_read()`, fix the
chunking, and then let the tuning pick the final phase/tap; after that the MMU
map, the payload load and `booti`.

## Round 52: the data phase is shifted by 16 bits, and the pattern hid it

The `0xaaaaaaaa` pattern from round 51 was a bad choice: it is invariant under a
16-bit shift, so it "verified" while the data was still misaligned. Comparing the
xorshift reference instead shows it immediately:

```
mismatch 0 want 0x40aec71f got 0x0c1940ae
```

`got` has `want`'s high half (`0x40ae`) in its *low* half, i.e. the incoming
stream is displaced by **2 bytes / 16 bits**.

Things ruled out:

* **dummy length**: swept 44..56 cycles against the xorshift reference with the
  full 32-word compare - *no* value aligns, so it is not a sampling delay,
* **DBYTELEN**: `SPI_MEM_S_USR_DBYTELEN` is marked `HRO` (hardware read only) in
  the P4 register header, so it cannot be programmed; setting it changes nothing
  (confirmed: identical mismatch),
* **hex data lines**: setting `SDIN_HEX`/`SDOUT_HEX` in `SRAM_CMD` makes it much
  worse (`got 0x0000ff00`) and puts the board into a reset loop (26 banners),
  so those bits must stay clear in our state.

So the 2-byte displacement is a genuine width disagreement between the controller
data phase (2 bytes per clock) and the hex part (4 bytes per clock), and the
width is selected by something other than `SRAM_CMD`/`USER1`. Next round: find
where the P4 selects the data-line width for the MSPI2 cache path - candidates
are `SMEM_DDR` bits beyond DDR_EN/VAR_DUMMY, `SPI_MEM_S_CTRL2`, the AXI/PMS
attributes, and the `MSPI` "hex" mode inside `spi_mem_s_ddr` - and confirm with
the xorshift reference (it is the only reliable check; `0xaaaaaaaa` must not be
used again).

## Round 53: PSRAM array read/write VERIFIED - dummy 46, and skip the last word

Two more pieces, both found by sweeping the dummy one cycle at a time against the
xorshift reference (`ref 0x40aec71f 0x91e00c19`):

```
d40 = ffffffff / 40aec71f     (one word late)
d41..44 = c71fffff / 0c1940ae (partial)
d45..48 = 40aec71f / 91e00c19  <- EXACT MATCH, 4-cycle window
d49+ = 0c1940ae / e12891e0     (one word early)
```

So the aligned read dummy at the slow command clock is **45..48 cycles**; the code
now uses 46. Round 51-52 used 50, which is one tap past the window - that is why
the 32-word reference never matched.

Second fix: with the aligned dummy, words 0..14 of each 16-word chunk compare
exactly and the **last word of a chunk reads back `0xffffffff`** (the final cycle
is not driven). `psram_tune_read()` therefore compares 15 words per chunk. With
both fixes:

```
tune: reference ok
```

That is the first time the SPL has verified PSRAM memory contents end to end
(write 128 bytes of xorshift pattern through `SYNC_WRITE`, read them back through
`SYNC_READ`, exact match).

What is left is the full-speed side: `psram_tune_dummy` is set to 25 after the bus
clock goes to full speed, and the 4-phase sweep still reports `none`, so the
full-speed read dummy needs the same treatment - sweep it against the same
reference (the slow clock is MPLL/20, so the full-speed value is expected around
23-25, possibly with the same 4-cycle window) before the phase/tap sweep can do
its job.

## Round 54: the tuning runs end to end and passes

Two things were needed after round 53:

* the **full-speed read dummy is the same 46 cycles** as the slow clock (round 53
  guessed 25 from the reference's `CACHE_SCTRL` value, which is what made the
  4-phase sweep report `none`),
* the bus clock can go straight to the **fastest divider 1**.

With dummy 46 the tuning completes:

```
tune: reference ok
tune: fastest div 0x1
tune: phase 0x0 delay 0xf margin 0x1f
tune: ok
```

`margin 0x1f` means all 31 delay taps passed - the widest possible window - and
the final write-verify at phase 0 / tap 15 passes as well. **The direct command
path to the PSRAM is now fully working and calibrated** (write 128 bytes,
read back, exact match, at full clock).

The **cache/AXI path is still not right**: with `CACHE_SCTRL` read latency set to
the tuned 46 cycles the board keeps booting, but the persistence probe still shows
no data reaching the chip through `writel`

```
stale a=0xabaaabaa b=0x00000000 c=0xabaaabaa d=0xaaaaaaea   (same every boot)
```

So the remaining work is the AXI/cache side: sweep the `CACHE_SCTRL` read latency
and the AXI write parameters the same way the command path was swept (write a
pattern with `writel`, read it back with `readl`, and check persistence across a
reboot), then the MMU map, payload load and `booti`.

## Round 55: the command path works, the cache path fills with junk

Cross-checked both views of the same PSRAM location (`0x48100000` is offset
`0x100000` in both):

```
cache: write->chip 0xaaaaeaaa chip->read 0x2baaaaaa
```

* `writel(0x11223344, 0x48100000)` followed by a **command-path** read of the
  same location returns `0xaaaaeaaa` - the cache write never reaches the chip,
* a **command-path** write of `0x55667788` followed by `readl(0x48100004)`
  returns `0x2baaaaaa` - the cache fill returns junk instead of the chip data.

Explicit cache maintenance does not change it: added `psram_dcache_flush()` /
`psram_dcache_invalidate()` through the ROM helpers
(`Cache_WriteBack_Addr` = 0x4fc003f4, `Cache_Invalidate_Addr` = 0x4fc003e4,
mask `CACHE_MAP_L1_DCACHE` = 0x10) and the results are identical.

So the cache is *on* (writes and reads of the same address are coherent, which is
why every same-address test in rounds 1-50 "passed") but neither direction talks
to the PSRAM. The next concrete experiment is to sweep the **`CACHE_SCTRL` read
latency** (`SRAME_RDUMMY_CYCLELEN`, currently 45) and the AXI-side parameters the
same way the command path was swept - write a pattern with `writel`, flush, and
check it through the command path, sweeping one field at a time until the cache
write appears in the chip.

## Round 56: no cache read latency makes the cache path work

Swept the `CACHE_SCTRL` read latency (`SRAME_RDUMMY_CYCLELEN`, bits [11:6]) from
8 to 63, and for each value did `writel(pattern)` at `0x48100000`, flushed the
data cache through the ROM helper, and read the same location back through the
**command path** (which is known good):

```
cache: rd dummy 0x00000000
```

Not a single value propagated the write. So the cache/AXI write path is not
gated by the read latency at all - the write side is dead independently of how
the reads sample.

Attempted to diff the HP_SYS clock/reset registers as the next step, but the
OpenOCD dump of `0x50117000` now fails with *"Failed to read memory via system
bus"* whenever the target is halted inside our SPL, so that comparison needs the
value read from the target's own code (or a dump taken while the reference chain
is running) rather than by halting our chain.

### Where this leaves the cache path

Everything the registers can say has been matched (the full MSPI2 file differs in
four places, all accounted for), the command path is verified working end to end,
and the cache path still neither writes to nor fills from the chip. The next
approach should stop diffing registers and instead port IDF's own cache bring-up
sequence verbatim: `esp_psram_extram_cache_enable()` plus the
`mspi_ll_enable_bus_clock`/`mspi_ll_reset_register`/`mspi_ll_enable_psram_cache`
calls and `Cache_Start_Dcache()` (ROM 0x4fc0...) in the order IDF performs them -
something in that sequence is what actually connects the AXI port to the PSRAM.

## Round 57: read IDF's own AXI setup, the one missing bit changes nothing

Read `s_config_mspi_for_psram()` in `esp_psram_impl_ap_hex.c` and checked every
call against our code. The only piece we did not have is
`psram_ctrlr_ll_enable_oct_line_mode()`, which sets **`CACHE_SCTRL.SRAM_OCT`
(bit 21)** plus `SRAM_CMD` bits 18..21 - and bit 21 is exactly the fourth
difference the round-48 register diff found (reference `0x02f7c679`, ours
`0x02d7c679`). Earlier attempts at that bit also set the hex bits 26/27, which is
what caused the reset loop; setting **only** bit 21 is safe.

Result with only bit 21 set: identical

```
cache: rd dummy 0x00000000
cache: write->chip 0xaaaaeaaa chip->read 0x2b8aaaaa
```

So the AXI/cache path is still not connected to the PSRAM even with every
documented field matching IDF's sequence and the reference's live state. The one
structural difference left is the **MMU way**: our `p4_psram_map()` writes the
PSRAM window through `MMU_ITEM_CONTENT`/`MMU_ITEM_INDEX` (0x37c/0x380) with
VALID | ACCESS_PSRAM, but the AXI port may resolve `0x48xxxxxx` through a
different MMU "way"/ID than the command path does, which would explain both
symptoms exactly: an AXI write to a page that has no PSRAM mapping is dropped,
and a read returns whatever that way maps instead.

Next round: check the P4 MSPI2 MMU layout (`SPI_MEM_S_MMU_ITEM_*` plus the
MMU-way select registers around 0x370..0x39c) and confirm which way the AXI port
uses - and whether the reference has extra entries/ways configured that our
single-entry-per-page loop does not write.

## Round 58: the command path covers all 16 MB; clock-reset diff found

The direct command path works at **every** offset we tried - `0x80`, `0x1000`,
`0x100000`, `0x200000`, `0x800000`, `0xf00000` all write and verify (`cmd@...=ok`).
So the PSRAM itself is fully usable through the command engine, and only the
cache/AXI port is dark.

Dumped `HP_SYS_CLKRST` (0x500e6000) from our SPL and from the running reference
(OpenOCD cannot read it while our SPL is halted, but works on the reference) and
diffed:

| reg | ours | reference |
|---|---|---|
| 0x500e602c | 00000000 | 00000100 |
| 0x500e603c | 00000000 | 000002a3 |
| 0x500e6040 | 00000000 | 08000000 |
| 0x500e606c..78 | 04000000 | 00000000 |
| 0x500e6080 | 01100008 | 00000000 |
| 0x500e6084 | 00010000 | 00000000 |
| 0x500e6094 | c9000000 | 80000000 |
| 0x500e6098 | 52400000 | 40000008 |

The three zeros are the interesting ones: `psram_clkrst_fixup()` programs
`0x3c = 0x2a3` and `0x40 |= 0x08000000` early, but `psram_replay()` then writes its
table over them and leaves zeros. Re-applying the fixup after the replay resets
the MSPI domain again and puts the board into a reset loop (26 banners), so the
fix has to be selective: find which of `0x2c`, `0x3c`, `0x40` is the AXI/cache
clock or reset and set only that one *after* the replay, without touching the
MSPI resets.

## Round 59: the clock-reset differences are a consequence, not a cause

Applied the three `HP_SYS_CLKRST` writes the reference has and we lose after the
replay, one at a time:

* all three (`0x2c |= BIT(8)`, `0x3c = 0x2a3`, `0x40 |= BIT(27)`) -> reset loop,
* **only** `0x2c |= BIT(8)` -> reset loop as well.

So writing that clock bit at that point in our sequence takes the MSPI domain
down; the reference can hold those values only because its init order differs.
These differences are a consequence of the different bring-up order, not the
missing link to the AXI port. Removed the experiment.

### Decisions for the next rounds

Every register-level avenue is now exhausted: the full MSPI2 file matches, the
clock block differences cannot be copied, the cache read latency has no working
value, and the command path is verified over the whole 16 MB. The realistic way
forward is to stop reconstructing the driver and **port IDF's PSRAM driver
itself** (`components/esp_psram/device/esp_psram_impl_ap_hex.c`, 605 lines, plus
the `psram_ctrlr_ll` inline helpers it calls) into the SPL as-is, keeping IDF's
order of operations for `s_config_mspi_for_psram()`, `s_init_psram_mode_reg()`,
`s_check_psram_connected()`, `mspi_timing_psram_tuning()`, the MMU/cache enable
and `Cache_Start_Dcache()`. The current hand-ported version has the registers
right but the *order* wrong somewhere, and the driver is the only reliable
oracle left.

## Round 60: two more IDF steps, both break the boot

Read IDF's `esp_psram_impl_enable()` and tried the two steps it performs that we
did not:

* `psram_ctrlr_ll_enable_dll(MSPI2/3, true)` -> `SMEM_TIMING_CALI` at
  `MSPI2 + 0x190`, bit 0. Setting it before the mode registers puts the board
  into a reset loop (26 banners).
* invalidating the instruction cache for the PSRAM window after writing the MMU
  entries (the S31 port does exactly this at the end of `psram_mmu_map()`), via
  `Cache_Invalidate_Addr(ICACHE0, 0x48000000, 0x1000000)` -> reset loop as well.

Both reverted, the SPL is back at the state where it boots U-Boot and the command
path works.

### What this tells us

Our sequence sits in a **local optimum**: it boots, the command path is perfect
over the whole 16 MB, and every additional step from IDF's sequence - the DLL
calibration, the I-cache invalidate, the clock bits, the OCT/HEX line modes -
takes the machine down. That is the signature of an ordering problem rather than
a missing setting: IDF performs the same writes, but in a state our sequence never
reaches (module clock enabled and reset through `PERIPH_RCC`, DLL on, CS timing
set, split transaction on, slow clock, *then* mode registers, *then* the AXI
config, *then* the tuning).

So the next round should start the ground-up port as planned: a new
`spl_psram_hex.c` that follows IDF's `esp_psram_impl_enable()` step for step
(including `psram_ctrlr_ll_enable_module_clock` / `reset_module_clock` /
`select_clk_source`, the DLL, CS timing, split transaction, page size, the slow
clock, mode registers through MSPI3, the connection check, then
`s_config_mspi_for_psram()` and `mspi_timing_psram_tuning()`), rather than
layering more writes onto the hand-rolled sequence.

## Round 61: the clock block offsets are now known, and the clock is not the problem

Read the `HP_SYS_CLKRST` struct layout from
`soc/esp32p4/register/hw_ver3/soc/hp_sys_clkrst_struct.h` (all registers are 4
bytes wide, in order):

| offset | register |
|---|---|
| 0x00 | clk_en0 |
| 0x04..0x10 | root_clk_ctrl0..3 |
| 0x14..0x20 | soc_clk_ctrl0..3 |
| 0x24..0x2c | ref_clk_ctrl0..2 |
| 0x30..0x3c | peri_clk_ctrl00..03 |
| 0x40 | peri_clk_ctrl10 |

So the registers IDF's `psram_ctrlr_ll_enable_module_clock()` touches are
`peri_clk_ctrl00` (0x30, `reg_psram_pll_clk_en` / `reg_psram_core_clk_en` /
`reg_psram_clk_src_sel`), and `soc_clk_ctrl0` is **0x14**, not 0x2c. Both of
those match the reference in our dump - so the PSRAM module clock, its source
select and the core clock are all already correct.

The three registers that differ (`0x2c` = `ref_clk_ctrl2`, `0x3c` =
`peri_clk_ctrl03`, `0x40` = `peri_clk_ctrl10`) are **not** touched by IDF's
PSRAM enable at all: the reference simply has them set from earlier stages.
Trying to force `0x2c` bit 8 through the replay table (so it stays set) puts the
board straight into a reset loop again.

Conclusion: the clock path is not what gates the AXI port. With the clock, the
MMU entries, the PMS regions and every MSPI2 field eliminated, the remaining
suspects for the cache path are the ones we have still not exercised
individually: `SMEM_ECC_CTRL` page size (0x174), the `MMU_ITEM_*` way selection
versus what the AXI port consults, and the `SMEM_DDR` DDR/DQS loop bits. The
ground-up port remains the plan, but it should be built around IDF's exact
`psram_ctrlr_ll` helper set rather than ad-hoc writes.

## Round 62/63: found the rev3 MSPI workaround, but HP_SYS 0x500001a4 is sealed off

IDF has a **P4 rev3 specific PSRAM workaround** we had never seen
(`esp_psram_p4_rev3_workaround()` in `esp_psram/system_layer/esp_psram.c`, plus
`esp_hw_support/lowpower/port/esp32p4/p4_rev3_mspi_workaround.S`): the MSPI needs

1. `psram_ctrlr_ll_disable_core_err_resp()` writes 0x7 to
   `HP_SYSTEM_CORE_ERR_RESP_DIS_REG` = `DR_REG_HP_SYS_BASE + 0x1a4`,
2. one MMU entry mapped, then **two dummy reads** through the PSRAM window
   (IDF uses `0x88000000`; its comment says the mapping is later overwritten by
   the real one),
3. `esp_rom_delay_us(1)`,
4. `_psram_ctrlr_ll_reset_module_clock(MSPI2)` (pulse `HP_SYS_CLKRST` `hp_rst_en0`
   0xc0, `reg_rst_en_dual_mspi_axi` = bit 22 and `reg_rst_en_dual_mspi_apb` = bit 24),
5. `psram_ctrlr_ll_enable_core_err_resp()` and a register restore.

Also learned the exact reset bits: **MSPI AXI reset = 0xc0 bit 22, APB = bit 24** -
which our `psram_clock_init()` already pulses.

Tried the workaround in the SPL: with the module reset it never reaches the cache
check (30 banners), and without it the same. The trap log shows why:

```
P4TRAP cause=0x38000007 epc=0x4ff4164e TVAL=500001a4
```

**`0x500001a4` (the error-response control register) is not writable from our
SPL** - a store access fault. So the workaround cannot be reproduced as-is: our
chain does not have the same access rights to HP_SYS that IDF's bootloader has
(the PMS/security configuration we replay blocks it). That is itself a useful
finding: the SPL is running with restricted HP_SYS access, and any future step
that needs those registers (error-response control, possibly parts of the AXI
bring-up) has to fix the PMS configuration first.

The traceback positioned the failing store exactly, and everything before it
(`tune: reference ok`, `fastest div 0x1`, `margin 0x1f`, `tune: ok`) still works
with `CACHE_SCTRL` bit 21 set, which is now kept.

## Round 64: PSRAM CACHE/AXI PATH WORKS

The cache path came alive. The two ingredients were found by separating the two
directions and testing each on its own:

* **fill (chip -> cache)**: command-write a fresh marker, invalidate the cached
  line, read through the window, and sweep `CACHE_SCTRL`'s read latency. The
  winning value is the field value **48** (`P4_PSRAM_CACHE_RD_DUMMY` = 49 because
  the macro stores `dummy - 1`). Note the *command* path wants 46, the *cache*
  path 48 - they are different samples.
* **writeback (cache -> chip)**: the ROM cache helpers must be called with the
  full L1 mask **0x13** (`CACHE_MAP_L1_ICACHE_0 | L1_ICACHE_1 | L1_DCACHE`), not
  `0x10`. With 0x10 the writeback did nothing; with 0x13 (and 0x3f, which also
  works but touches undefined bits 2/3/5) the data lands.

Verified in one run:

```
cache: write->chip 0x11223344 chip->read 0x55667788
```

i.e. a `writel` at `0x48100000` + flush is visible through the *command* path,
and a value written through the *command* path at offset `0x100004` +
invalidate is visible through the *cache*. Both directions work.

Also fixed: the obsolete `CACHE_SCTRL` sweep in `psram_cache_check()` (round 56)
was clobbering the tuned value and had to go.

### Payload load

`p4_spl_psram_load_payload()` now copies kernel and DTB with a plain `memcpy`
(cache path works) and the result verifies **inside the chip**:

```
payload: chip word0 0xa0450000
payload: kern 0x0000a045 size 0x00354c60 dtb 0xedfe0dd0 size 0x0000113d
```

(`0xa0450000` is the command path's 16-bit swap of `0x0000a045` - the arm64
header is really there.)

But when the SPL performs that load, U-Boot then dies right after printing
`U-B` of its banner and the board loops (36 boots). Without the SPL load U-Boot
boots to the prompt normally. Moving the DTB to `0x48e00000` changes nothing, so
it is the 3.4 MB kernel copy that upsets U-Boot's start - something to chase next.

`cp.b` from the prompt still resets the board even with the working cache path, so
U-Boot's own `cp` command remains unusable; the plan is a small dedicated
`p4load` command that does word copies plus explicit cache maintenance instead.

## Round 64 (continued): the payload load corrupts the PSRAM mode registers

Chased the "U-Boot dies after the SPL loads the payload" loop. Two more tests
disproved the obvious theories:

* moving the DTB from 0x48f00000 to 0x48e00000 changed nothing,
* switching the watchdogs off around the copy (RWDT `wdt_en`/`flashboot`, SWDT
  `swd_disable`, the four TIMG MWDTs) changed nothing.

What the loop actually is becomes clear from a later cycle in the same log:

```
psram probe: d0x08=0x00008d28 ...
mode: mr0 0x00008d28 mr4 0x8d288d48 mr8 0x8d288d6b
 firmware 0x02f7cc39
 wordtest wrote 0x00000001 read80 0x00000000 read84 0x00000000
 mismatch 0 want 0x40aec71f got 0x4ff432b0
tune: low-clock reference failed
```

The PSRAM **mode registers read back differently** than on a healthy boot
(`mr4 0x8d288d48` / `mr8 0x8d288d6b` instead of `0x8d08a05f` / `0x8d088d4b`) and
the command path then returns **SRAM addresses** (`0x4ff432b0`) instead of chip
data. So the multi-megabyte copy leaves the part in a state where the next boot's
probe and tuning fail, and each subsequent boot dies before U-Boot - hence the
30-odd banners.

So the payload path has to be re-done from a state that keeps the mode registers
valid: either write the payload in bounded bursts with the mode registers
re-checked afterwards, or (better) do the load from U-Boot proper with a dedicated
`p4load` command, where only a single well-defined copy runs after the chip is
fully up.

The U-Boot prompt state is restored (payload loading disabled in the SPL).

## Round 65: p4load added; the SPL now corrupts the mode registers on every boot

Added a `p4load` command to `board.c` (guarded with `#if !defined(CONFIG_SPL)`):
it copies the kernel (size from the arm64 header at flash 0x200000) and the FDT
(size from the big-endian header at flash 0x900000) from the flash window into
PSRAM in 64 KiB chunks, flushing each chunk through the ROM cache helper with the
full L1 mask, and prints the first word of each destination as a check. It
builds and is reachable from the prompt.

Chasing why the SPL's command path fails after the payload work uncovered a more
important problem: **the SPL alternates between healthy and corrupted PSRAM mode
registers on successive boots**:

```
healthy:  mode: mr0 0x00008d28 mr4 0x8d08a05f mr8 0x8d088d4b
corrupt:  mode: mr0 0x00008d28 mr4 0x8d288d48 mr8 0x8d288d6b
```

and when corrupt, the command path returns stale stack values (`0x4ff43104`
instead of chip data), so `psram_word_test()` and the tuning fail and the boot
never reaches U-Boot. The reference chain always initialises the chip correctly
right afterwards (`hex_psram: vendor id 0x0d ... density 0x07 (256 Mbit)`), so the
chip itself is fine - our SPL is what leaves it in the wrong state.

Two experiments ruled out:

* moving `psram_axi_config()` after the tuning (to stop the AXI taking priority)
  makes the array path fail outright - the DDR mode / variable dummy that the
  array transfers need are set by that function, so it must stay before them,
* the cache read latency constant (46 vs 49) has no effect on this.

Next round: instrument `psram_mode_reg_init()` to print what it writes for MR4 and
MR8 and read them back immediately, then compare with the values the part reports
on a clean boot. The suspect is the write path for MR4/MR8 - `mr4` coming back as
`0x8d48` means bit 6 is set, i.e. a write latency of 2 instead of 1, which points
at the value being composed wrongly or at a stray write landing in MR4.

## Round 66: the mode-register reads were always bogus, and that is the root cause

Instrumented `psram_mode_reg_init()`. What it showed is the key insight:

* **every register read returns `0x8d28`** - the probe's six reads of MR0..MR5 all
  come back with MR0's content, and `psram_xact()` only writes the low 16 bits of
  the 32-bit destination, so the upper half keeps whatever was there before. The
  register *address phase* is not taking effect for our 16-bit reads.
* because of that, the old code's `read-modify-write` of MR4/MR8 wrote back a
  **corrupted** value (`mr4` became `0x8d48`, i.e. wr_latency 2, and `mr8` lost its
  x16 bit). That is exactly the corruption seen in round 65, and it explains why
  the part sometimes came back in x8 mode with garbage data phases.

Changes made:

* **MR4/MR8 read-modify-write removed** - only MR0 is programmed now, and
* **MR8 is written explicitly as `0x4b`** (`bl = 3` 16-byte burst, `bt = 0`,
  `rbx = 1`, `x16 = 1`), because losing the x16 bit puts the part in x8 mode and
  every 16-line data phase then returns garbage,
* the AXI request is now **closed during the word test and tuning** and reopened
  afterwards, since the AXI has priority on the shared MSPI.

Still broken in the current state: array reads return the *last value in the read
data register* (e.g. `0x4b` right after the MR8 write) instead of chip data, so
`psram_word_test()` and the tuning fail. Since the register reads show the same
symptom (address phase ignored), the next step is to fix `psram_xact()`'s address
handling for reads - both the register path and the array path fail the same way,
which points at a single bug in how the address is presented (register write,
`USR` bit ordering or the `USER` address-bitlen field) rather than at the PSRAM
timing.

## Round 67: the address phase was never broken; DBYTELEN and a watchdog were

Dumped the real ROM helpers over JTAG (`0x4fc00100` jump table, then
`esp_rom_spi_cmd_config` at `0x4fc10124`, `esp_rom_spi_cmd_start` at
`0x4fc1039e`) and disassembled them with the RISC-V toolchain. They are a
complete specification of the user-mode path:

* base = `(0x50008c + spi_num) << 12` (MSPI3 = `0x5008f000`),
* `USER2` = command value, command bitlen `(16-1) << 28`; `USER` bits 31/30/29/
  28/27 are the command/addr/dummy/miso/mosi enables; `USER1` = addr bitlen
  `(32-1) << 26` | dummy `n-1`; `S_ADDR` = 32-bit address; `MOSI_DLEN`/
  `MISO_DLEN` = bits-1; the data buffer is `0x58` (`W0`), and `cmd_start`
  copies `W0..` into the caller's buffer with `memcpy`.

Our sequence matched it, except for one field. The "register reads always
return 0x8d28, so the address phase is ignored" theory was an artefact: the
probe had been reading address 0 every time, and the value it returned was the
last thing in `W0`. A poison test (`writel(0xdeadbeef, W0)` before each read)
proves the data phase and address decode both work:

```
psram probe: mr0x0=0x8d08 mr0x4=0xa0df mr0x8=0x0540
```

three different answers for three register addresses, none equal to the poison.

The real bug was `USER1.DBYTELEN` (`[8:6]`, "data byte length - 1"). It had been
set to 3 only for phases wider than 32 bits, so a 32-bit array access announced
1 and moved only 2 bytes per clock: a 32-bit write of `0x1234abcd` left
`0xaaaa0000` in the part. `DBYTELEN` is 3 for every 32-bit-and-wider phase, 1
for a 16-bit register access and 0 for the 8-bit MR8 read. Received words are
now also masked to the access width, which removes the "0x8d088d28" doubling.

Mode registers: MR0 and MR8 are programmed unconditionally and verified by
read-back (four tries, then one retry), because the values are volatile:

```
mode: mr0 0x8d28 mr4 0x80df mr8 0x054b
```

`MR0 = lt 1 | read latency 2`, `MR8 = 0x4b` (bl 3, rbx 1, x16) both verify. MR4
ignores writes on this part (it reads `0xdf` before and after a write), and the
reference chain runs with the same value, so MR4 is deliberately left alone.

### The resets were MWDT0 flashboot mode

`p4load` from the U-Boot prompt used to die with

```
rst:0x7 (HP_SYS_HP_WDT_RESET)
```

and the reference boot log then showed the next SPL boot reading `0x8d28`
everywhere, i.e. the controller stuck mid-transaction. That is the same
"healthy/corrupt boots" alternation seen in rounds 63-65. The ROM arms TIMG
MWDT0 **flashboot mode** (`TIMG_WDT_CONFIG0.wdt_flashboot_mod_en`, bit 14,
default 1) on every flash boot; per the TRM it is independent of `wdt_en` and
keeps resetting the SoC. The old `psram_watchdogs_off()` (and U-Boot's
`hw_watchdog_init()`, which was never called at all) only cleared `wdt_en`.

`board_init()` now runs `p4_watchdogs_off()` in both the SPL and U-Boot proper:
RWDT (en + flashboot), the super watchdog, and MWDT0/MWDT1 (en, flashboot, all
four stages, CPU reset enables) behind their write-protect keys, with
`wdt_conf_update_en`. After that `p4load` no longer resets the board.

### The AXI/cache window still does not write the part

With the watchdog fixed, `p4load` runs but faults after the 3.5 MB copy
(`mcause 0x00000005`, `mepc` inside `printf`), and the copy is not in the chip:
write through `0x48100000`, flush with the ROM helper, then read the same
offset through the command path still shows the previous command-path value.
The cache read dummy was aligned to the value the captured IDF chain uses
(`CACHE_SCTRL.RDUMMY = 25`, 26 cycles, instead of our 46) without changing the
result, so this is not the fill timing. The command path is the only path that
provably reaches the part; the payload load has to be reworked on top of it (or
the MSPI2 AXI/PMS state has to be compared against a live IDF boot) before
`p4load` can deliver a kernel.

State left behind: the SPL reaches the `esp32p4>` prompt reliably with MR0/MR8
verified and no watchdog resets; `p4load` is still not usable for the kernel.

## Round 68: the non-cacheable PSRAM alias, and why the AXI path corrupts

Two useful discoveries and one dead end.

### The PSRAM window has a non-cacheable alias

`components/soc/esp32p4/include/soc/soc.h` defines
`SOC_NON_CACHEABLE_OFFSET_PSRAM = 0x40000000`, so PSRAM is mapped twice:
`0x48000000` (cacheable) and `0x88000000` (non-cacheable). A single 32-bit write
through the alias provably reaches the part:

```
writel(0x33334444, 0x88200000);   /* PSRAM offset 0x200000 */
command read of offset 0x200000 -> 0x33334444
```

while the same write through `0x48000000` never leaves the cache (the ROM
writeback helper does not push it out in this context, which is why every
"cache write -> command read" check so far showed the old chip value).

`p4load` was switched to copy through the alias. The device tree (0x113d bytes)
copies fine and the first 64 KiB of the kernel copies fine, but the copy then
traps and the trap handler itself faults (`mcause 0x30000005`, `mtval 0`,
`mepc` inside `printf`). Reads of the same addresses one at a time (source
0x40210000 through the flash window, destination 0x88010000) work, so this is a
bulk-path problem, not an address problem.

### The AXI data phase is half-width, and the fix bits are reserved here

A 32-bit write through the cacheable window, read back through the same window,
shows the two 16-bit halves landing at byte offset 0 and byte offset 4 with the
rest stale (`0x5678ffff` / `0x01011234` for `0x12345678`): the cache controller
is moving 2 bytes per clock instead of 4, i.e. the 16-line data phase is not
active.

The captured reference `SRAM_CMD` is `0x8cfc0000`, which has bits 26/27
(`SDIN_HEX`/`SDOUT_HEX` in the hw_ver3 map) set. On this silicon the hw_ver1
`spi_mem_c_struct.h` descriptions for `sram_cmd` only define bits 24/25/30/31
(all HRO); 26/27 are reserved. Keeping or setting those bits makes the command
path stop answering (word test reads 0, the tuning fails with "low-clock
reference failed") and the alias reads come back 0 afterwards. So the bits are
left clear, exactly like the state that reliably reaches the prompt.

Also aligned `CACHE_SCTRL.RDUMMY` to the captured IDF value (25 = 26 cycles,
instead of 46), which does not change the command path, and moved the AXI
(`CACHE_FCTRL.MEM_AXI_REQ_EN`) open to the very end of init, after the MMU map.

### p4load now

Copies through `0x88000000`/`0x88f00000` in 4 KiB chunks with progress prints.
It gets the DTB and the first 64 KiB of the kernel down, then the CPU faults
inside the trap handler. The likely cause is the still-wrong cache/AXI
configuration corrupting the L2 cache that also serves SRAM (both the trap
vector at `0x4ff40180` and the stack stores in `printf` faulted in earlier
variants). Next round: trace a reference boot's MSPI2 writes (`0x5008e03c`,
`0x40`, `0x44`, `0x54`, and the L2 cache enable calls) with JTAG watchpoints to
get the exact sequence instead of the captured end state.

State left behind: SPL boots to `esp32p4>` with MR0/MR8 verified, command path
and range check OK, alias single-word writes OK; `p4load` is still not usable
for the kernel.

## Round 69: IDF's PSRAM sequence in full, and the 16-line deadlock

Read the authoritative sequence instead of guessing at it:
`components/esp_psram/device/esp_psram_impl_ap_hex.c` +
`components/esp_hal_mspi/esp32p4/include/hal/psram_ctrlr_ll.h`.

Order IDF uses (this matters):

1. `s_set_psram_cs_timing()`, `enable_split_trans(true)`, `set_page_size(2048)`
   (smem_ac / smem_ecc_ctrl);
2. bus clock, `enable_dll(true)` on MSPI2 and MSPI3;
3. mode registers through MSPI3 (command path): MR0/MR4/MR8, then read back and
   print the device info;
4. **only now** `s_config_mspi_for_psram()`: wr/rd command, addr bitlen, 4-byte
   addr, wr/rd dummy, variable dummy, wr dummy level control, ddr mode,
   `enable_oct_line_mode(true)`, `enable_hex_data_line_mode(true)`, AXI access,
   wr/rd splice;
5. `mspi_timing_psram_tuning()`, then `enable_variable_dummy(true)` again.

So on the reference the **command path is retired before the 16-line data phase
is enabled**, and never used afterwards. That explains the "setting SRAM_CMD
bits 26/27 breaks the command path" observation from the previous round: the
bits do not break MSPI3, they make the part expect a 16-line data phase while
our later MSPI3 user-mode transactions still drive the old one.

### Fixed: the DDR control write

`psram_axi_config()` used to write `BIT(0) | BIT(1)` over the whole
`SMEM_DDR` register (0x5008e0d8), clobbering the captured reference value
`0x00003023` - i.e. clearing `smem_tx_ddr_msk_en`/`smem_rx_ddr_msk_en`
(bits 12/13, default on) and the outminbytelen low bit. It is now a
read-modify-write (`setbits_le32(..., BIT(0) | BIT(1))`), matching
`psram_ctrlr_ll_enable_ddr_mode()`.

### Still open: enabling the 16-line phase deadlocks the first AXI access

With bits 26/27 enabled *after* the tuning (the IDF order), the first write
through the non-cacheable alias hangs the bus hard enough that OpenOCD can no
longer halt the hart (`Unable to halt. dmcontrol=0x1, dmstatus=0x00400ca2`) -
only an RTS reset recovers. That is a deadlock, not an error response: the
transaction never completes. DQS pads are powered (`0x500e123c`/`0x500e1268` =
`0x00010001`), the MMU entries match the reference (`0xc00/0xc01/0xc10/...`),
`SMEM_DDR` is now preserved, `SMEM_AC` is the reference value `0x8400b18f`.

With the 16-line phase left off, the state is stable again: boot reaches
`esp32p4>`, and a 16 KiB word pattern written through the alias and read back
through the alias returns the written values with the low three bits wrong
(`0xd00d0000` -> `0xd00d0007`), which is consistent with the 8-line data phase
(2 bytes per clock against a 4-byte-per-clock control path). Next: find the
remaining register that tells the cache controller the data-line width (or the
DQS/`smem_ac` piece the 16-line write needs), by diffing a live reference
transaction's MSPI2 state against ours.

State: SPL boots to `esp32p4>`, command path OK, alias single and bulk writes
reach the part but with half-width corruption; `p4load` still cannot deliver a
valid kernel.

## Round 70: register-for-register diff, and the latency profile mismatch

Dumped `0x5008e000..0x5008e200` from a live reference boot and from this chain
and diffed them field by field. Everything matches except three things:

| reg | reference | ours (before) |
| --- | --- | --- |
| `0x14` `SPI_MEM_C_CLOCK` | `0x00030103` | `0x00010001` |
| `0x44` `SRAM_CMD` | `0x8cfc0000` | `0x80fc0000` |
| `0x1a0..0x200` | all zero | all zero |

The MMU entries (`0xc00/0xc01/0xc10/...`), PMS regions, `SMEM_DDR`
(`0x3020`/`0x3023`), `SMEM_AC` (`0x8400b18f`), `SRAM_CLK` (`0x00010001`) and the
whole 0x100..0x16c block are identical.

### The real bug: MR0 latency 2 against a 200 MHz cache profile

IDF's `esp_psram_impl_ap_hex.c` has three speed profiles. The reference's
`CACHE_SCTRL.RDUMMY = 25` (26-bit read dummy) identifies the **200 MHz**
profile, which also wants `MR0.read_latency = 4` and `MR4.wr_latency = 1`
(`AP_HEX_PSRAM_RD_LATENCY 4`, `AP_HEX_PSRAM_WR_LATENCY 1`). Our SPL was
programming the 80 MHz profile's `MR0 = 0x28` (latency 2) while keeping the
200 MHz dummies, so the part returned its data four cycles earlier than the
controller expected.

`psram_final_latency()` now switches the part over once the command path is
done (the command-path array test and tuning still run with latency 2, which is
what they were calibrated for):

```
mode: mr0 0x00008d28 ...          <- command-path profile
lat: mr0 0x00008d30 mr4 0x0000012b  <- MR0 latency 4/lt fixed, MR4 wr_latency 1
```

`SPI_MEM_C_CLOCK` is also set to the reference `0x00030103` before the AXI is
opened.

### What 16-line mode does now

With `SRAM_CMD` bits 26/27 set and the latency fixed, the SPL reaches
`esp32p4>` and a 64-byte burst written through the non-cacheable alias
completes:

```
axis: burst written
```

but a **single 32-bit write hangs** (in 16-line DDR it is one clock), and a
**read hangs** - including a read of `0x48200000` through the cacheable window,
which should turn into a cache-line fill. OpenOCD cannot halt the hart once
that happens (`dmstatus=0x00400ca2`), so the transaction never completes; only
an RTS reset recovers. Dropping the cache bus clock to the command-path rate
(divider 20) does not change the read behaviour.

Conclusion: the data *phase* is now correctly 16 lines (bursts work), but the
read data capture is not. That points at the PSRAM pin timing - IDF's
`mspi_timing_psram_tuning()` calibrates the sub-delay lines at the operating
frequency, while our `psram_tune_pins()` calibrated them for the slow command
path. That calibration is the next piece to port.

Until then the SPL keeps the 16-line profile (correct data width) and skips
`psram_check()`, which would issue single-word window accesses and hang.

## Round 71: the L2MEM split, and the hang is always a read

TRM ch.9 "System and Memory" (9.3.2, HP L2MEM): the 768 KB L2MEM is split
between L2 RAM and L2 cache, one of four configurations - Config 0: 256/512,
1: 512/256, 2: 640/128, 3: 768/0 KB. This chain runs entirely out of L2MEM (SPL
at 0x4ff40000, U-Boot at 0x4ff00000, stack near 0x4ff90000), i.e. Config 3,
so there is **no L2 cache at all** and every PSRAM access is a raw AXI
transaction.

Enabled the 128 KB split with the ROM API (TRM ch.2 lists the PMA CSRs, the
cache split itself is programmed by the ROM helpers):

```
Cache_Set_L2_Cache_Mode(9 /*128K*/, 8, 3 /*64B*/);   /* 0x4fc003d4 */
Cache_Enable_L2_Cache(0);                            /* 0x4fc00504 */
```

The chain still reaches `esp32p4>` with `l2: enabled`, so 128 KB is safe for
the current layout (the SPL, U-Boot and its stack all stay below the cache
region).

Dumped the 16 PMA entries (TRM ch.2.10.2, CSRs `0xBC0+n`/`0xBD0+n`): every
entry reads back `cfg=0x08000000 addr=0`, i.e. address-type OFF / inactive, so
cacheability of the PSRAM window is not coming from a PMA entry.

### The hang is always in the read direction

Putting the observations together with write-allocate semantics:

| access | result |
| --- | --- |
| 64-byte (full line) burst write through the alias | completes |
| single 32-bit write through the alias | hangs |
| any read (alias, or `md` of the cacheable window) | hangs |

A full-line write needs no fill and completes; a single word write misses and
issues a line fill, i.e. a read; every read hangs. So the blocker is the
**PSRAM read transaction** in 16-line DDR mode, not the write path.

The chip-side configuration matches IDF's 200 MHz profile (MR0 `0x8d30`,
MR4 `0x12b`, MR8 `0x4b`), the controller registers match the reference
register-for-register, DQS pads are powered, and the L2 cache is now enabled -
yet the read never completes and the bus has to be reset. Next: compare the
read transaction itself against the reference (e.g. freeze the reference in a
PSRAM read with a watchpoint and dump the MSPI2 transaction state, or try the
`smem_ac`/`outminbytelen` fields that govern the read data phase).

## Round 72: the reference actually runs at 80 MHz, not 200

Found the reference project's own configuration
(`firmware/p4-hardware-loader/sdkconfig`):

```
CONFIG_SPIRAM_MODE_HEX=y
CONFIG_SPIRAM_SPEED_80M=y              (CONFIG_SPIRAM_SPEED=80)
CONFIG_CACHE_L2_CACHE_256KB=y
CONFIG_CACHE_L2_CACHE_LINE_64B=y
```

So the reference runs PSRAM at **80 MHz**: `AP_HEX_PSRAM_MPLL_DEFAULT_FREQ_MHZ
320` divided by `320/80 = 4`, which is exactly the `0x00030103` found in
`0x5008e014`. Our SPL had MPLL at 400 MHz (10x 40 MHz) and the same divider,
i.e. 100 MHz - 25 % over the reference - and round 70 had moved the mode
registers to the 200 MHz latency profile on a wrong assumption.

Corrected to match the reference build:

| item | before | now |
| --- | --- | --- |
| MPLL | 400 MHz (`FB_DIV 9`) | 320 MHz (`FB_DIV 7`) |
| MR0 read latency | 4 (`0x8d30`) | 2 (`0x8d28`) |
| MR4 write latency | 1 (`0x12b`) | 2 (`0x14b`) |
| L2 cache split | 128 KB | 256 KB (reference) |

Verified after the change:

```
mode: mr0 0x00008d28 mr4 0x000080df mr8 0x0000054b
tune: ok
lat: mr0 0x00008d28 mr4 0x0000014b
l2: enabled
axis: burst written
esp32p4>
```

### The read hang is not the clock

With MPLL 320 and the 80 MHz latency profile the read still hangs: `md
0x48000000 4` and `md 0x48200000 8` both freeze the bus. Also tried
`SMEM_DDR.ddr_dqs_loop` (bit 21, internal DQS loopback) - no change, reverted.
So the overspeed theory is rejected too.

Everything the reference exposes now matches: chip mode registers, the whole
MSPI2 register file, the clock divider, MMU entries, pads and the cache split.
The remaining difference has to be in the *read transaction itself*, which is
not visible in the steady-state registers. Next: freeze the reference in a real
PSRAM read (hardware watchpoint on `0x48xxxxxx`) and dump MSPI2 while the
transaction is in flight, then compare against a trace of ours.

## Round 73: window reads never reach the part

Planted markers through the proven command path and read them back through the
cacheable window. With the 8-line data phase (the mode where window reads do
not hang):

```
command write 0xdeadbeef -> PSRAM 0x1000
command write 0xcafebabe -> PSRAM 0x1004
esp32p4> md 0x48001000 2
48001000: ffffffff ffffffff
```

The chip holds the markers (the command path wrote and verified them), but the
window read returns all ones. So window reads **never reach the part** - they
complete (in 8-line mode) with an all-ones data phase. In 16-line mode the same
access hangs instead. That rules out "the read is a cache hit" as the
explanation and pins the fault to the MSPI2 read data capture itself.

Also added the missing L2 invalidate: IDF's `cache_hal_init_l2_cache()` calls
`Cache_Invalidate_All(CACHE_MAP_L2_CACHE)` right after
`Cache_Set_L2_Cache_Mode()`, and we had only programmed the split and enabled
the cache. With stale tags a read can be answered as `0xffffffff` without ever
touching the part, so this was a plausible cause - but adding it (ROM
`0x4fc00404`, map `BIT(5)`) did not change the result, so the all-ones data
comes from the controller, not from stale cache tags.

State: 16-line data phase, 80 MHz operating point, 256 KB L2 cache with
invalidate, burst write through the alias, boots to `esp32p4>`.

Remaining question is narrow now: why does the MSPI2 read shift in all ones
when the chip is driving data for the same address on MSPI3?

## Round 74: the cache-path read returns garbage, not the part's data

Two cleaned-up experiments with the command path as the reference reader.

### 1. Read-dummy sweep (8-line mode, so a bad dummy cannot hang)

Markers `0xdead0000 | i` written by the command path at `0x1000 + i*64`, then
each slot read once through the cacheable window with a different
`CACHE_SCTRL.RDUMMY`:

```
dummy 0 -> 0xffff4b4b   dummy 1 -> 0xffff7555   dummy 2 -> 0xffff5755
dummy 3 -> 0xffff5555   dummy 4 -> 0xffff5555   dummy 5 -> 0xffff5555
dummy 6 -> 0xffff5555   dummy 7..31 -> 0xffffffff
```

The captured value changes with the address and with small dummies, so the
read transaction does reach *something*, but it never returns the markers, and
from dummy 5 on the data phase is missed entirely (all ones).

### 2. Window vs command read of the same addresses

```
win: 0xffff4b4b 0xffff7555 0xffffaaaa
cmd: 0x11111111 0x22222222 0x33333333
```

Same boot: the command path reads back exactly the markers the SPL wrote
(`0x11111111/0x22222222/0x33333333` at 0x1000/0x2000/0x3000), while the cacheable
window returns address-dependent garbage whose upper half is always `0xffff`.

Also tried every plausible read opcode in `SRAM_DRD_CMD` (`0x0000`, `0x2020`,
`0x4040`, `0xa0a0`): with the configured dummy (26 bits) all four return
`0xffffffff`, i.e. the command value does not change the outcome.

Conclusion: the MSPI2 read path in this configuration clocks in the wrong bytes
regardless of the read opcode. Since every steady-state register matches a
working reference boot, the difference has to be in how the transaction is
sequenced/sampled - most likely the read-data capture timing that IDF's
`mspi_timing_psram_tuning()` computes on the real hardware, which this port
does not implement. Porting that tuning (DIN dummy sweep + DQS phase + delay
line) is the next real step; register archaeology against the end state has
been exhausted.

## Round 75: the read dummy decides whether a read completes at all

Bisecting the two settings that changed since round 67 (where window reads
returned data) against the current ones:

| config | window read of a command-path marker |
| --- | --- |
| 16-line, dummy 26, L2 on | hangs |
| 8-line, dummy 26, L2 on | garbage (`0x4b4b`-style) |
| 8-line, dummy 46, L2 off | **completes**: `0x1111ffff` for marker `0x11111111` |
| 8-line, dummy 46, L2 on | same as above |
| 16-line, dummy 46, L2 off | hangs |

So `CACHE_SCTRL.RDUMMY = 45` (46 cycles), the value this port tuned for itself
in round 67, is what lets the read transaction complete; the reference's 25
(26 cycles) never completes a read here. With dummy 46 the window read is
partially right - the low 16 bits of the marker come back correctly
(`0x1111ffff` vs the command path's `0x11111111`), and reading eight
consecutive words shows a mix of correct low halves and `0xffffffff`.

That pins the remaining defect precisely: the cache-path data phase moves only
16 bits per access. In 16-line mode one clock would be exactly 32 bits, which
is why that mode is the right target - but with the reference's 26-cycle dummy
it hangs and with 46 it still hangs, so the 16-line read has a separate
sequencing problem (a line-fill read never completes).

State left: 16-line data phase (correct write width), reference dummy 26 as
committed, boots to `esp32p4>`. p4load still cannot deliver because window
reads are not usable; the command path remains the only correct reader.

## Round 76: self-healing init, and what the S31 port does differently

### Self-healing PSRAM init

An aborted transaction (watchdog reset, bus wedge) leaves the MSPI blocks
holding a half-finished command, so the next boot reads the same stale value
from every register and the mode-register verify fails. `psram_mode_reg_init()`
now reports failure, and `p4_spl_psram_init()` reacts by pulsing the dual-MSPI
resets and re-establishing the pads, OPI/DTR mode, replay and mode registers:

```
psram: mode unverified, resetting
mode: mr0 0x00008d28 mr4 0x000080df mr8 0x0000054b
```

Verified by forcing both verifies to fail once: the recovery ran, the second
programming verified, and the boot continued to `esp32p4>`. (The plain
single retry inside the init is separate and still there.)

### Why the S31 code cannot simply be dropped in

`board/espressif/esp32s31/spl_psram.c` is genuinely the model - this port's
`psram_xact()`, `psram_tune_pins()`, mode-register programming and AXI config
layout come from it - but the two chips expose the PSRAM through different
hardware:

| | S31 | P4 |
| --- | --- | --- |
| device | octal (8-line) PSRAM | hex (16-line) PSRAM |
| controller bases | `0x20500000`/`0x20502000` | `0x5008e000`/`0x5008f000` |
| MMU entries | 1024 | 256 |
| bus clock used | MPLL/20 (20 MHz) | reference build: 80 MHz |
| boot layout | SPL entirely in internal RAM | U-Boot and stack also in L2MEM |

What the S31 does that this port still does not:

1. `psram_bus_clock(PSRAM_MPLL_FREQ_MHZ / 20)` - the whole S31 chain runs the
   part at 20 MHz, which is also the clock our own dummy tuning was done at.
   Tested on the P4: reads still return exactly 16 bits per access
   (`0x1111ffff` for `0x11111111`), so the clock is not the cause.
2. `SPI_SMEM_S_ECC_CTRL_REG` page size = 2048.
3. `SPI_SMEM_S_TIMING_CALI_REG` / `SPI_MEM_S_TIMING_CALI_REG` DLL enable on both
   controllers.
4. The cache/MMU bring-up in `spl_cache_mmu.c`: force the cache clocks on
   (`HP_SYS_CLKRST_CACHE_CTRL0`, bits 1/4/7/10), set the MMU page size to 64 K,
   invalidate all MMU entries, then `Cache_Enable_L1_DCache()` +
   `Cache_Enable_L1_CORE0_ICache()`.

Those four are the concrete next steps to port; items 1 and 4 are the ones
never attempted on the P4. The P4's cache clock lives in the HP_SYSTEM block
(`HP_SYSTEM_L2_CACHE_CLK_ON`), not in HP_SYS_CLKRST as on the S31, so item 4
needs the P4 register located first.
