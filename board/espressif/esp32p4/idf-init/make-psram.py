#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Relink only the pinned IDF AP/Hex driver and timing dependency closure."""
import argparse
import json
from pathlib import Path
import re
import shlex
import struct
import subprocess

p = argparse.ArgumentParser()
p.add_argument("profile", choices=("stock", "perf"))
p.add_argument("app_build", type=Path)
p.add_argument("boot_hal", type=Path)
p.add_argument("out", type=Path)
a = p.parse_args()
b = a.app_build.resolve()
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
src = Path(__file__).resolve().parent
project = json.loads((b / "project_description.json").read_text())
pin = "a602e67b0bf9ee0806dc4e1df7afc9affedf5c33"
if (project["target"] != "esp32p4"
    or subprocess.check_output(["git", "-C", project["idf_path"], "rev-parse",
                                "HEAD"], text=True).strip() != pin):
    p.error("expected the pinned IDF ESP32-P4 reference")
cfg = json.loads((b / "config/sdkconfig.json").read_text())
speed = 80 if a.profile == "stock" else 200
if (cfg.get("SPIRAM_SPEED") != speed or cfg.get("SPIRAM_USE_8LINE_MODE", False)
    or cfg.get("SPIRAM_ECC_ENABLE", False) or not cfg.get("ESP32P4_SELECTS_REV_LESS_V3")
    or cfg.get("SECURE_BOOT", False) or cfg.get("SECURE_FLASH_ENC_ENABLED", False)):
    p.error("incompatible PSRAM/board profile")
unit = next(x for x in json.loads((b / "compile_commands.json").read_text())
            if x["file"].endswith("/esp_psram_impl_ap_hex.c"))
cc = shlex.split(unit["command"])
gcc = cc[0]
subprocess.run(cc[:cc.index("-o")] + ["-c", str(src / "psram-adapter.c"),
                                     "-o", str(out / "psram-adapter.o")],
               cwd=unit["directory"], check=True)

# The minimal IDF project exists only to produce standard driver objects and
# cite the resolved build. Never install or run its app or its bootloader.
name = project["project_name"]
rule = (b / "build.ninja").read_text().split(f"build {name}.elf:", 1)[1].split("\n\n", 1)[0]
def field(key):
    return shlex.split(re.search(r"(?m)^  " + key + r" = (.*)$", rule)[1])

flags = field("LINK_FLAGS")
scripts = [flags[i + 1] for i, v in enumerate(flags)
           if v == "-T" and flags[i + 1] not in ("memory.ld", "sections.ld")]
hardware = {"esp_psram", "esp_hw_support", "esp_hal_mspi", "esp_hal_clock",
            "esp_hal_regi2c", "esp_hal_pmu", "esp_hal_rtc", "esp_hal_wdt",
            "esp_hal_ldo", "esp_hal_misc", "esp_hal_cache", "esp_libc",
            "esp_rom", "hal", "soc", "spi_flash", "esp_mm"}
archives = list(dict.fromkeys(
    x for x in field("LINK_LIBRARIES") if x.startswith("esp-idf/")
    and x.endswith(".a") and x.split("/")[1] in hardware))
elf = out / "p4-psram.elf"
link = [gcc, "@" + str(b / "toolchain/cflags"), "-nostartfiles",
        "-Wl,--gc-sections,--no-relax", "-Wl,-Map=" + str(out / "p4-psram.map"),
        *field("LINK_PATH"), *[x for script in scripts for x in ("-T", script)],
        "-T", str(src / "psram.ld"), str(out / "psram-adapter.o"),
        "-Wl,--start-group", *archives, "-lgcc", "-lc", "-Wl,--end-group", "-o", str(elf)]
subprocess.run(link, cwd=b, check=True)
syms = subprocess.check_output([gcc.removesuffix("gcc") + "nm", "-n", elf], text=True)
for required in ("esp_psram_impl_enable", "mspi_timing_psram_tuning",
                 "mspi_timing_psram_set_best_tuning_delayline"):
    if not re.search(r"\b" + required + r"$", syms, re.M):
        p.error("missing hardware function: " + required)
if re.search(r" (?:call_start_cpu[01]|app_main|[xv]Task\w+|bootloader_utility_\w+|esp_efuse_\w+write\w*)$", syms, re.M):
    p.error("application, loader, scheduler or eFuse writer in hardware closure")
part = out / "p4-psram.bin"
subprocess.run([gcc.removesuffix("gcc") + "objcopy", "-O", "binary", elf, part], check=True)
base, ext = a.boot_hal.read_bytes(), part.read_bytes()
if len(base) > 0x4000 or len(ext) < 12 or len(ext) >= 0xc000:
    p.error("hardware segments do not fit in the reserved SRAM")
if ext[:8] != struct.pack("<II", 0x50535231, speed):
    p.error("missing PSRAM ABI or incorrect actual sidecar profile")
(out / "p4-hw-combined.bin").write_bytes(base.ljust(0x4000, b"\0") + ext)
