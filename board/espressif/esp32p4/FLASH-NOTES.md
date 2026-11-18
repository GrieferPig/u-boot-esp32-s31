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
