#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Relink the vendor bootloader_init dependency closure as a callable SRAM segment."""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("idf_build", type=Path)
parser.add_argument("out", type=Path)
args = parser.parse_args()
build = args.idf_build.resolve()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
src = Path(__file__).resolve().parent
project = json.loads((build / "project_description.json").read_text())
if project["target"] != "esp32p4" or project["project_name"] != "bootloader":
    parser.error("not a P4 second-stage bootloader build")
commands = json.loads((build / "compile_commands.json").read_text())
unit = next(x for x in commands if x["file"].endswith("/src/esp32p4/bootloader_esp32p4.c"))
compile_cmd = shlex.split(unit["command"])
gcc = compile_cmd[0]
objcopy = gcc.removesuffix("gcc") + "objcopy"
nm = gcc.removesuffix("gcc") + "nm"
compile_cmd = compile_cmd[:compile_cmd.index("-o")] + [
    "-fno-common", "-c", str(src / "adapter.c"), "-o", str(out / "adapter.o")
]
subprocess.run(compile_cmd, cwd=unit["directory"], check=True)

# Use the selected board's own compiler, ROM imports and archive objects, not a
# copied register table. Fail if the pinned Ninja link-rule format changes.
rule = (build / "build.ninja").read_text().split("build bootloader.elf:", 1)[1].split("\n\n", 1)[0]
def field(key):
    value = re.search(r"(?m)^  " + key + r" = (.*)$", rule)
    if not value:
        parser.error("missing ELF link field: " + key)
    return shlex.split(value[1])

flags = field("LINK_FLAGS")
scripts = [flags[i + 1] for i, v in enumerate(flags) if v == "-T"
           and flags[i + 1] not in ("bootloader.memory.ld", "bootloader.sections.ld")]
link = [gcc, "@" + str(build / "toolchain/cflags"), "-nostartfiles", "-Wl,--gc-sections,--no-relax",
        "-Wl,-Map=" + str(out / "p4-hal.map"), *field("LINK_PATH")]
for script in scripts:
    link.extend(["-T", script])
link.extend(["-T", str(src / "hw.ld"), str(out / "adapter.o"), "-Wl,--start-group"])
archives = sorted({x for x in field("LINK_LIBRARIES") if x.startswith("esp-idf/")
                   and x.endswith(".a") and not x.startswith("esp-idf/main/")})
elf = out / "p4-hal.elf"
link.extend([*archives, "-lgcc", "-lc", "-Wl,--end-group", "-o", str(elf)])
subprocess.run(link, cwd=build, check=True)
symbols = subprocess.check_output([nm, "-n", elf], text=True)
for forbidden in ("call_start_cpu0", "bootloader_utility_load_boot_image",
                  "esp_efuse_batch_write_commit", "esp_efuse_burn_new_values"):
    if re.search(r"\b" + forbidden + r"$", symbols, re.M):
        parser.error("unexpected policy/loader symbol: " + forbidden)
subprocess.run([objcopy, "-O", "binary", elf, out / "p4-hal.bin"], check=True)
