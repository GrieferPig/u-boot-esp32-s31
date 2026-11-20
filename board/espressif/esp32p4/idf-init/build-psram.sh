#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
if [[ $# != 4 || ! ${IDF_PATH:-} ]]; then
    echo "usage: $0 stock|perf board.sdkconfig p4-hal.bin out" >&2; exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
out=$(realpath -m "$4")
mkdir -p "$out"
ref="$out/psram-objects"
# The archive dependencies are identified from the actual IDF project files.
# Optional reuse is guarded by make-psram.py's resolved Kconfig and IDF pin.
if [[ -n ${P4_IDF_PSRAM_BUILD:-} ]]; then
    ref=$(realpath "$P4_IDF_PSRAM_BUILD")
else
    mkdir -p "$ref"
    python "$here/profile.py" "$1" "$2" "$ref/sdkconfig"
    cmake -S "$here/psram-reference" -B "$ref" -G Ninja -DIDF_TARGET=esp32p4 \
        -DSDKCONFIG="$ref/sdkconfig" -DPYTHON="$(command -v python)"
    cmake --build "$ref" --parallel "${JOBS:-8}"
fi
python "$here/make-psram.py" "$1" "$ref" "$3" "$out"
