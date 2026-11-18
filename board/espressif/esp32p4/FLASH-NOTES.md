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
