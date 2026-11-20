#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Retain the known initramfs/ABI while rebuilding the patched Linux source.
set -euo pipefail
if [[ $# != 3 ]]; then
    echo "usage: $0 PATCHED_LINUX_TREE ORIGINAL_IMAGE OUT" >&2; exit 2
fi
linux=$(realpath "$1")
original=$(realpath "$2")
out=$(realpath -m "$3")
[[ "$out" != "$linux" ]]
mkdir -p "$out"
echo "ea467ee22c4500040c7d27eaf8fc5dbc1c2df05cd0086d71f7207b4c9b6a6c7f  $original" | sha256sum -c -
"$linux/scripts/extract-ikconfig" "$original" > "$out/.config"
python3 - "$original" "$out/initramfs.cpio" <<'PY'
import hashlib, sys, zlib
from pathlib import Path
data = Path(sys.argv[1]).read_bytes()
z = zlib.decompressobj(31)
# The pinned original Image has its only gzip-compressed newc stream here.
rootfs = z.decompress(data[0x1ae250:])
assert z.eof and rootfs.startswith(b"070701")
assert hashlib.sha256(rootfs).hexdigest() == "36292013b7ce70f890726422b9f823860be32fcc211a725e04fdb3b47b8aabe4"
Path(sys.argv[2]).write_bytes(rootfs)
PY
"$linux/scripts/config" --file "$out/.config" --set-str INITRAMFS_SOURCE "$out/initramfs.cpio"
# The current Linux source includes the framebuffer's DSI functions in a
# cooperating object; the old payload predated these required build options.
"$linux/scripts/config" --file "$out/.config" --enable I2C --enable FB_ESP32P4_DSI
export ARCH=riscv CROSS_COMPILE="${CROSS_COMPILE:-riscv32-esp-elf-}"
export LC_ALL=C KBUILD_BUILD_USER=p4 KBUILD_BUILD_HOST=reproducible \
       KBUILD_BUILD_TIMESTAMP="2026-11-19 00:00:00 UTC" KBUILD_BUILD_VERSION=1
make -C "$linux" O="$out" olddefconfig
make -C "$linux" O="$out" -j"${JOBS:-8}" Image
