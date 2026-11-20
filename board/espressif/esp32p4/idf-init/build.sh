#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Package ONE second-stage U-Boot image after the ordinary split make builds.
set -euo pipefail
if [[ $# != 4 || ! ${IDF_PATH:-} ]]; then
	echo "Usage (with ESP-IDF environment active): $0 stock|perf SDKCONFIG.REFERENCE U-BOOT-SPL-DTB.BIN OUTPUT-DIR" >&2
	exit 2
fi
board=$(cd "$(dirname "$0")" && pwd)
out=$(realpath -m "$4")
mkdir -p "$out"
ref="$out/reference"
sha=a602e67b0bf9ee0806dc4e1df7afc9affedf5c33
[[ $(git -C "$IDF_PATH" rev-parse HEAD) == "$sha" ]] || { echo "Expected ESP-IDF $sha" >&2; exit 1; }
riscv32-esp-elf-gcc --version | head -1 | grep -F '15.2.0' >/dev/null
python "$board/profile.py" "$1" "$2" "$out/sdkconfig"
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1795046400}
cmake -S "$IDF_PATH/components/bootloader/subproject" -B "$ref" -G Ninja \
	-DIDF_TARGET=esp32p4 -DIDF_PATH="$IDF_PATH" \
	-DSDKCONFIG="$out/sdkconfig" -DPYTHON="$(command -v python)"
cmake --build "$ref" --parallel "${JOBS:-8}"
python "$board/make-hal.py" "$ref" "$out"
bash "$board/build-psram.sh" "$1" "$2" "$out/p4-hal.bin" "$out"
python "$board/pack.py" "$3" "$out/p4-hw-combined.bin" "$ref/bootloader.bin" "$out/spl-p4.bin"
sha256sum "$ref/bootloader.bin" "$out/p4-hal.bin" "$out/p4-psram.bin" "$out/spl-p4.bin"
