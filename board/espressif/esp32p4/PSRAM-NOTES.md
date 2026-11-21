# ESP32-P4 PSRAM bring-up notes (SPL)

Working notes for the in-package 32 MB AP hex (16-line) PSRAM on MSPI2/MSPI3.
Everything below was verified against this board (revision 103) and the
ESP-IDF P4 sources.

## Blocks

| Block | Base | Purpose |
|---|---|---|
| MSPI2 (`spi_mem_s`) | `0x5008e000` | system PSRAM controller, AXI window at `0x48000000` |
| MSPI3 (`spi1_mem_s`) | `0x5008f000` | peripheral PSRAM controller, user transactions |
| HP_SYS_CLKRST | `0x500e6000` | PSRAM clock source / core clock / dual-MSPI reset |
| PMU | `0x50115000` | MSPI PHY power, perif I2C power |
| LP_CLKRST | `0x50111000` | MPLL clock gate (`+0x40`, bit 28) |
| LPPERI | `0x50120000` | analog I2C master clock (`+0x00`, bit 27) |
| LP_I2C_ANA_MST | `0x50124000` | analog I2C master (regi2c) |

## Clock

MPLL is an analog PLL: it can only be programmed through regi2c.

1. Enable the analog I2C master clock: `LPPERI + 0x00 |= BIT(27)`.
2. Select its 160 MHz reference: `ANA_MST + 0x34 |= BIT(0)`.
3. Power the peripheral I2C: `PMU_RF_PWC (0x5011515c)` clear `BIT(26)`,
   `udelay(1)`, set `BIT(27)`, set `BIT(26)`.
4. Write MPLL dividers: block `0x63`, register `2`,
   `ref_div` in bits [2:0], `fb_div` in bits [7:3],
   `freq = 40 MHz * (fb_div + 1) / (ref_div + 1)`.
   400 MHz = `ref_div 0`, `fb_div 9` -> value `0x48`.
5. Power the PLL: `PMU_RF_PWC |= BIT(24)` (MSPI PHY XPD) and
   `LP_CLKRST + 0x40 |= BIT(28)`.
6. Route it to the PSRAM controllers: `HP_SYS_CLKRST + 0x30`
   (`PERI_CLK_CTRL00`): source select bits [13:12] (1 = MPLL),
   core clock enable bit 15, divider bits [23:16].
7. Release the dual MSPI reset: `HP_SYS_CLKRST + 0xc0` clear
   bits 23 (AXI) and 25 (APB).

After step 6/7 the MSPI2/MSPI3 register blocks are readable **and writable**
from the SPL. Before the reset release, writes fault with a store access fault
(`TVAL = 0x5008e00c`).

## Register maps (offsets, verified)

MSPI2 `spi_mem_s`:

```
0x00 CMD   0x04 ADDR  0x08 CTRL  0x0c CTRL1  0x10 CTRL2  0x14 CLOCK
0x18 USER  0x1c USER1 0x20 USER2 0x34 MISC   0x3c CACHE_FCTRL
0x40 CACHE_SCTRL  0x44 SRAM_CMD  0x48 SRAM_DRD_CMD  0x4c SRAM_DWR_CMD
0x50 SRAM_CLK     0x54 FSM      0xc0 INT_ENA       0xd4 DDR  0xd8 SMEM_DDR
0x100.. FMEM_PMS0..3 ATTR  0x110.. ADDR  0x120.. SIZE
0x130.. SMEM_PMS0..3 ATTR  0x140.. ADDR  0x150.. SIZE
0x178 SMEM_AXI_ADDR_CTRL  0x180 TIMING_CALI  0x184 DIN_MODE
0x188 DIN_NUM             0x18c DOUT_MODE    0x190 SMEM_TIMING_CALI
```

MSPI3 `spi1_mem_s`:

```
0x00 CMD   0x04 ADDR  0x08 CTRL  0x0c CTRL1  0x10 CTRL2  0x14 CLOCK
0x18 USER  0x1c USER1 0x20 USER2 0x34 MISC
0x58..0x94 W0..W15 (data buffers)
```

Important: the two blocks do **not** share the classic SPI layout used by the
S31 port, so replayed `0x08/0x0c/...` values land in the wrong registers.

User-command fields (MSPI3):

```
USER  = 1<<31 (command) | 1<<30 (addr) | 1<<29 (dummy) | 1<<28 (miso) | 1<<27 (mosi)
USER1 = addr_bitlen << 26 | dummy_cycles
USER2 = cmd_bitlen << 28 | command_value ; mosi bit length in USR_MOSI_BITLEN
MISC  = CS hold / keep-active bits
```

System-path fields (MSPI2):

```
SMEM_DDR (0xd8):        DDR_EN bit0, RDAT_SWP bit2, WDAT_SWP bit3, CMD_DIS bit4,
                        DQS_LOOP bit21
CTRL   (0x08):          SMEM_SPLIT_TRANS_EN bit31, SMEM_PAGE_SIZE bits[19:18]
SMEM_PMS0_ATTR (0x130): RD_ATTR bit0, WR_ATTR bit1 (e.g. 3 = read+write)
SMEM_PMS0_ADDR (0x140): 27-bit start address field
SMEM_PMS0_SIZE (0x150): 15-bit size field
```

## Device mode registers

The AP hex PSRAM needs MR0 (drive strength, read latency, latency type),
MR4 (write latency) and MR8 (burst length/type, x16) programmed over
OPI-DTR commands before the controller timing is valid:

```
REG_READ  = 0x4040, REG_WRITE = 0xc0c0 (16-bit command, 32-bit address)
MR0/MR1 at addr 0x0, MR4/MR8 at addr 0x4, MR8 also at addr 0x8 (8-bit data)
```

Timing constants per speed (from the vendor driver): 200 MHz uses read dummy
`2*(18-1)`, read latency 6, write latency 3; 80 MHz uses `2*(14-1)`, 4 and 1.

## Status and known pitfall

- Clock bring-up (through MPLL) works from the SPL today.
- The mask ROM's OPI helper `esp_rom_spi_cmd_start` (0x4fc0010c) **never
  returns** when called from the SPL (with or without prior controller
  configuration), so the mode-register path must be implemented locally with
  the MSPI3 user-command registers above.
- The capture used for comparison: `psram-ref.txt` (post-init state from a
  working ESP-IDF boot) and the SPL's own pre-init dump.
