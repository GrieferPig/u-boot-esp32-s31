#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build from one git tree; nothing here flashes a device or fetches dependencies.
set -euo pipefail
if [[ $# != 5 ]]; then
    echo "usage: $0 stock|perf board.sdkconfig Linux-Image board.dtb out" >&2; exit 2
fi
case "$1" in stock|perf) ;; *) exit 2;; esac
here=$(cd "$(dirname "$0")" && pwd)
tree=$(git -C "$here" rev-parse --show-toplevel)
out=$(realpath -m "$5"); mkdir -p "$out"
sdk=$(realpath "$2"); kernel=$(realpath "$3"); dtb=$(realpath "$4")
echo "5e2bf5e862bc95b1b33d1b224a0d8841ca070849d87e363aa88d2a3fb32b9574  $sdk" | sha256sum -c -
export CROSS_COMPILE="${CROSS_COMPILE:-riscv32-esp-elf-}" ARCH=riscv
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1795046400}"
v=UBOOTRELEASE=2024.07-p4-uart-audit
for c in sram spl; do
    make -C "$tree" O="$out/$c" "$v" "espressif_esp32p4_${c}_defconfig"
    make -C "$tree" O="$out/$c" "$v" -j"${JOBS:-8}"
done
# Existing *compiled* IDF reference may be reused, never installed as a stage.
# Verify the requested, board-derived profile before using any archives.
if [[ -n ${P4_IDF_BUILD:-} ]]; then
    [[ $(git -C "$IDF_PATH" rev-parse HEAD) == a602e67b0bf9ee0806dc4e1df7afc9affedf5c33 ]]
    python "$here/profile.py" "$1" "$sdk" "$out/sdkconfig.expected"
    python - "$P4_IDF_BUILD" "$out/sdkconfig.expected" "$1" "$IDF_PATH" <<'PY'
import json,sys
from pathlib import Path
b=Path(sys.argv[1]);p=json.loads((b/'project_description.json').read_text())
def enabled(file):
    return {k:v for l in Path(file).read_text().splitlines() if l.startswith('CONFIG_') for k,_,v in [l.partition('=')]}
if Path(p['idf_path']).resolve()!=Path(sys.argv[4]).resolve():
    raise SystemExit('Reference build is from another IDF source tree.')
actual=enabled(p['config_file']);requested=enabled(sys.argv[2])
resolved=json.loads((b/'config/sdkconfig.json').read_text());perf=sys.argv[3]=='perf'
for k,v in {'BOOTLOADER_COMPILER_OPTIMIZATION_PERF':perf,'ESPTOOLPY_FLASHMODE_QIO':perf,
 'BOOTLOADER_LOG_LEVEL':1 if perf else 3,'BOOTLOADER_WDT_ENABLE':True,'BOOTLOADER_WDT_TIME_MS':9000,
 'SECURE_BOOT':False,'SECURE_FLASH_ENC_ENABLED':False,'ESP32P4_SELECTS_REV_LESS_V3':True}.items():
    if resolved.get(k)!=v:raise SystemExit(f'Resolved Kconfig mismatch: {k}')
for k,v in requested.items():
    if actual.get(k)!=v:raise SystemExit(f'IDF profile mismatch: {k} expected {v}, actual {actual.get(k)}')
PY
    python "$here/make-hal.py" "$P4_IDF_BUILD" "$out"
    python "$here/pack.py" "$out/spl/spl/u-boot-spl-dtb.bin" "$out/p4-hal.bin" "$P4_IDF_BUILD/bootloader.bin" "$out/spl-p4.bin"
else
    bash "$here/build.sh" "$1" "$sdk" "$out/spl/spl/u-boot-spl-dtb.bin" "$out"
fi
python "$here/pack-fit.py" --mkimage "$out/sram/tools/mkimage" "$out/spl-p4.bin" "$out/sram/u-boot.bin" "$kernel" "$dtb" "$out"
