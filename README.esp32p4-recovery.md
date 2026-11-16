# ESP32-P4 U-Boot: recovered changes, incomplete source

This branch preserves the exact U-Boot changes that remain recoverable as of
2026-11-16. It is **not a complete, buildable ESP32-P4 port**, and it does not
claim to reproduce the previously tested P4 binary.

The unchanged source base is
`d692a84ea0bf80f319512bb8e892da0bbb93a4b4` from
`GrieferPig/u-boot-esp32-s31` (the pinned S31 U-Boot base used for the P4 work).
The P4 changes below are retained as unapplied patches under
`doc/esp32p4-recovery/`, so the original S31 implementation is not altered.

## Included material

- `0002-add-p4-idf-hardware-handoff.patch`: complete retained patch text,
  5933 bytes, 201 lines, nine hunks.
  SHA-256: `c853cbab54d5833bbfaa63c726a1f5f998d658cbc43058a96a2b8677233ba66a`.
  It adds the hardware-only IDF cache callback contract, preserves t6 across
  relocation cache callbacks, and adds the revision-103 hardware defconfig
  and DTS. It contains no temporary P4_TRACE diagnostics.
- `0001-timer-fix.patch-diff`: the exact retained Git diff of the original
  Buildroot patch file, 749 bytes, 13 lines, one hunk.
  SHA-256: `6105e8f8a531a52eaf2e74bff1c4ef6f195ae9b4140c8590ce719e211d954c67`.
  This is a diff **of a patch file**, not the complete board patch and not a
  patch that can be applied directly to this U-Boot source tree.

The timer correction changes SYSTIMER APB gating from clock/reset offset
`0x30` to `0x1c`, bit 23. The old address altered the live PSRAM divider.
The correction belongs to the missing first P4 board patch, not patch 0002.

## Source gap

The complete prerequisite
`0001-add-esp32p4-mmode-emulator-board.patch` has not been recovered. It supplied
the base P4 board, timer, device tree and target integration that patch 0002
expects. Applying 0002 directly to the pinned S31 base fails because those
P4 files and DTS integration are absent. The missing code has not been
guessed, rewritten or inferred from the binary.

The original integration checkout is not currently accessible. This is not
evidence that it was deleted. Its previously recorded integration checkpoint
was `ecd0854b6f785e48545a4eed50c2960fdd7e2cf5`; that is not a commit available
or restored by this branch.

The retained, separately built P4 hardware U-Boot binary is 163032 bytes,
SHA-256 `42456cf005c32af2fb18ece17d4e10dda071ea6d0db0f6afc6556c72111e0c04`.
Its historical physical-board validation does not validate a new build from
this incomplete source branch. The binary is intentionally not committed
here and is not a substitute for the missing source.

## Checks performed for this publication

- Verified authenticated ownership/push permission on this fork and the exact
  pinned base commit.
- Verified both retained patch texts parse with `git apply --numstat` and all
  hunk line counts match.
- Verified the retained binary hash, without changing it.
- Confirmed `git apply --check` of 0002 against the base fails at its missing
  P4 prerequisites; no successful P4 compilation is claimed.
- Existing local U-Boot checkouts and existing remote branches were not changed.

Before this branch can become a reproducible P4 implementation, obtain the
complete first patch (or undertake an explicitly new port), apply the
documented timer correction and hardware patch, rebuild, and repeat emulator
and physical-board validation. Do not flash a build of the S31 base onto P4.

The separately published main-project work is at:
https://github.com/GrieferPig/esp32-s31-linux/commit/44ec8f433ddc30f57e778893ada6e1fe8cb611ea
