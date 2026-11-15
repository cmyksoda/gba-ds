#!/bin/bash
# libmgba for the 3DS: the commit and options every GBA DS build uses (scripts/mgba-options.sh), with
# mGBA's own 3DS port's toolchain. Run inside the devkitpro/devkitarm image (../Makefile's `libmgba`).
#
#   build-libmgba.sh [DEPS]    DEPS/mgba-3ds/build/libmgba.a (DEPS: the repo's deps/ by default)
# rom-copy-size.patch: a copy of a ROM that a copy of the game writes to (patches, a cartridge clock or
# rumble) is the ROM's size, not 32 MiB. memory-map-hook.patch: mGBA's anonymousMemoryMap is GBA DS's
# (3ds/ctr/emu_ctr.c), so those copies come from the top of the heap's range, as the ROMs do.
# video-thread-core.patch: the core of mGBA's drawing thread is gbadsVideoCore (3ds/ctr/host.c), core 2 unless
# video_core in gba-ds.cfg says another.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT="$PWD"
DEPS="${1:-$ROOT/deps}"
SRC="$DEPS/mgba-3ds"
. scripts/mgba-options.sh

if [ ! -d "$SRC/.git" ]; then
	git clone -q https://github.com/mgba-emu/mgba.git "$SRC"
fi
git -C "$SRC" checkout -q -f "$MGBA_COMMIT"
# GBA DS works on 32-bit pixels and runs several copies of a game at once: not mGBA's 16-bit colour
# or its one fixed ROM buffer, nor the 32 MiB that buffer's start-up code takes.
sed -i -e 's/ COLOR_16_BIT COLOR_5_6_5 FIXED_ROM_BUFFER / /' \
	-e '/ctru-heap.c/d' "$SRC/src/platform/3ds/CMakeLists.txt"
grep -q 'COLOR_16_BIT' "$SRC/src/platform/3ds/CMakeLists.txt" && { echo "mGBA's 3DS defines not removed"; exit 1; }
git -C "$SRC" apply "$ROOT/3ds/mgba/rom-copy-size.patch" "$ROOT/3ds/mgba/memory-map-hook.patch" \
	"$ROOT/3ds/mgba/video-thread-core.patch"

cmake -S "$SRC" -B "$SRC/build" -DCMAKE_TOOLCHAIN_FILE="$SRC/src/platform/3ds/CMakeToolchain.txt" \
	"${MGBA_OPTS[@]}" -DUSE_LZMA=OFF -DUSE_DEBUGGERS=OFF > "$SRC/build.log"
make -C "$SRC/build" -j"$(nproc)" mgba >> "$SRC/build.log"
# mGBA's own files never include the flags.h it writes for frontends, so the library is built without
# MINIMAL_CORE, which flags.h still claims on consoles: struct mCore's layout must match the library's.
sed -i 's|^#define MINIMAL_CORE ON$|/* (MINIMAL_CORE: the library is not built with it) */|' "$SRC/build/include/mgba/flags.h"
grep -q '^#define MINIMAL_CORE' "$SRC/build/include/mgba/flags.h" && { echo "flags.h still says MINIMAL_CORE"; exit 1; }
echo "built $SRC/build/libmgba.a"
