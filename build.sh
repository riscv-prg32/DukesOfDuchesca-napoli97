#!/usr/bin/env sh
# Builds the DukesOfDuchesca-napoli97 PRG32 cartridge.
#
# This repo is a standalone cartridge, not a checkout of riscv-prg32/PRG32
# itself, so point PRG32_REPO at a checkout of that SDK (or pass it as the
# first argument). ARCH is "esp32c6" (real hardware) or "qemu" (emulator);
# building either target requires ESP-IDF to be sourced first
# (`source $IDF_PATH/export.sh`) -- see PRG32/docs/usage/getting_started.md.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${PRG32_REPO:-${1:-"$HERE/../PRG32"}}
ARCH=${2:-${PRG32_ARCHITECTURE:-qemu}}

if [ ! -d "$ROOT/components/prg32" ]; then
  echo "error: PRG32 SDK checkout not found at '$ROOT'." >&2
  echo "       set PRG32_REPO=/path/to/PRG32 or pass it as the first argument." >&2
  exit 1
fi

mkdir -p "$HERE/dist" "$HERE/build"
cd "$ROOT"

python3 tools/prg32audio_pack.py "$HERE/audio.json" \
  --out "$HERE/build/dukesofduchesca-napoli97-audio.block"

python3 -m prg32 cartridge build "$HERE/game.c" \
  --portable --entry-prefix dukes --name "dukesofduchesca-napoli97" \
  --architecture "$ARCH" \
  --audio-block "$HERE/build/dukesofduchesca-napoli97-audio.block" \
  --out "$HERE/dist/dukesofduchesca-napoli97-$ARCH-core.prg32"

python3 -m prg32 store attach-metadata "$HERE/dist/dukesofduchesca-napoli97-$ARCH-core.prg32" \
  --metadata "$HERE/metadata.json" --icon "$HERE/assets/icon.png" \
  --screenshot "$HERE/assets/screenshot.png" --colophon "$HERE/colophon.json" \
  --architecture "$ARCH" \
  --out "$HERE/dist/dukesofduchesca-napoli97-$ARCH.prg32"

echo "Built $HERE/dist/dukesofduchesca-napoli97-$ARCH.prg32"
