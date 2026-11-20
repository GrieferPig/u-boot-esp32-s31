#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Derive board-specific Kconfig input without editing the user's SDKCONFIG."""
import argparse
import hashlib
from pathlib import Path
import re

p = argparse.ArgumentParser()
p.add_argument("mode", choices=("stock", "perf"))
p.add_argument("board_config", type=Path)
p.add_argument("output", type=Path)
a = p.parse_args()
raw = a.board_config.read_bytes()
if hashlib.sha256(raw).hexdigest() != "5e2bf5e862bc95b1b33d1b224a0d8841ca070849d87e363aa88d2a3fb32b9574":
    p.error("expected the board sdkconfig.reference from project d8fa26d7")
if a.board_config.resolve() == a.output.resolve():
    p.error("refusing to replace board reference")

options = {
    "BOOTLOADER_COMPILER_OPTIMIZATION_PERF": True,
    "BOOTLOADER_COMPILER_OPTIMIZATION_SIZE": False,
    "BOOTLOADER_LOG_LEVEL_ERROR": True,
    "BOOTLOADER_LOG_LEVEL_INFO": False,
    "IDF_EXPERIMENTAL_FEATURES": True,
    "ESPTOOLPY_FLASHMODE_QIO": True,
    "ESPTOOLPY_FLASHMODE_DIO": False,
    "ESPTOOLPY_FLASHFREQ_120M": True,
    "ESPTOOLPY_FLASHFREQ_40M": False,
    "BOOTLOADER_FLASH_DC_AWARE": True,
    "SPI_FLASH_HPM_ENA": True,
    "SPI_FLASH_HPM_DC_AUTO": True,
    "SPIRAM_SPEED_80M": False,
    "SPIRAM_SPEED_200M": True,
    "COMPILER_OPTIMIZATION_PERF": True,
    "COMPILER_OPTIMIZATION_DEBUG": False,
}
if a.mode == "perf":
    text = raw.decode()
    for key, enabled in options.items():
        key = "CONFIG_" + key
        text = re.sub(r"(?m)^(?:# )?" + key + r"(?:=.*| is not set)$", "", text)
        text += "\n" + (key + "=y" if enabled else "# " + key + " is not set") + "\n"
    raw = text.encode()
a.output.write_bytes(raw)
