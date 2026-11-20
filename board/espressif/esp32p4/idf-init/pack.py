#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pack U-Boot SPL and hardware-only init as one two-segment ROM image."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

parser = argparse.ArgumentParser()
for arg in ("spl", "hal", "reference", "out"):
    parser.add_argument(arg, type=Path)
a = parser.parse_args()
spl, hal, ref, out = [p.resolve() for p in (a.spl, a.hal, a.reference, a.out)]
# Space up to 0x4ff5c000 precedes SPL stack and BSS at 0x4ff60000.
if spl.stat().st_size >= 0x1c000 or hal.stat().st_size >= 0x10000:
    parser.error("image exceeds reserved SRAM")
hw = hal.read_bytes()
if hw[:4] != struct.pack("<I", 0x50444931) or hw[0x4000:0x4004] != struct.pack("<I", 0x50535231):
    parser.error("missing P4 boot H/W or AP hex H/W ABI")
header = ref.read_bytes()[:24]
if (len(header) != 24 or header[0] != 0xe9 or header[2] not in (2, 3)
        or header[3] not in (0x40, 0x4f) or header[12:14] != b"\x12\0"
        or header[15:19] != bytes.fromhex("64 00 c7 00")):
    parser.error("expected P4, rev1.0..1.99, 16MB, DIO/DOUT 40/80MHz ROM header")
mode = {2: "dio", 3: "dout"}[header[2]]
freq = {0: "40m", 15: "80m"}[header[3] & 15]
with tempfile.TemporaryDirectory(dir=out.parent) as temp:
    d = Path(temp)
    oc = "riscv32-esp-elf-objcopy"
    subprocess.run([oc, "-I", "binary", "-O", "elf32-littleriscv", "-B", "riscv",
                    spl, d / "raw.elf"], check=True)
    subprocess.run([oc, "--change-section-address", ".data=0x4ff40000", "--rename-section",
                    ".data=.text,alloc,load,readonly,code,contents", "--add-section",
                    ".p4init=" + str(hal), "--set-section-flags", ".p4init=alloc,load,code,contents",
                    "--change-section-address", ".p4init=0x4ff70000", "--set-start", "0x4ff40000",
                    d / "raw.elf", d / "body.elf"], check=True)
    subprocess.run(["esptool", "--chip", "esp32p4", "elf2image", "--flash-mode", mode,
                    "--flash-freq", freq, "--flash-size", "16MB", "--min-rev-full", "100",
                    "--max-rev-full", "199", "-o", out, d / "body.elf"], check=True)
image = out.read_bytes()
if image[:8] != b"\xe9\x02" + header[2:4] + struct.pack("<I", 0x4ff40000):
    parser.error("packer did not preserve header parameters and U-Boot entry")
pos = 24
for expected_addr, payload in ((0x4ff40000, spl), (0x4ff70000, hal)):
    addr, length = struct.unpack_from("<II", image, pos)
    pos += 8
    data = payload.read_bytes()
    if addr != expected_addr or not len(data) <= length <= len(data) + 3 or image[pos:pos+len(data)] != data:
        parser.error("segment contents or layout mismatch")
    pos += length
