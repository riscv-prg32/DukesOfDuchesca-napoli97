#!/usr/bin/env bash
# Host checks: generated files up to date, the map unit tests, and the game
# harness (an autopilot that wins the game, traffic, fuzzing; both display
# back ends compared on every frame). Needs a PRG32 checkout for its public
# headers: PRG32_REPO=/path/to/PRG32 ./test.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${PRG32_REPO:-${PRG32_ROOT:-}}"
if [[ -z "$ROOT" && -f "$HERE/../PRG32/components/prg32/include/prg32.h" ]]; then ROOT="$HERE/../PRG32"; fi
if [[ -z "$ROOT" ]]; then echo "Set PRG32_REPO to a checkout of riscv-prg32/PRG32" >&2; exit 2; fi
CC="${CC:-cc}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
INC=(-I"$ROOT/components/prg32/include" -I"$ROOT/components/prg32_audio/include" -I"$HERE")

python3 "$HERE/scripts/gen_music.py" --check
"$CC" -std=c11 -Wall -Wextra -Werror "$HERE/citymap.c" "$HERE/tests/test_citymap.c" -o "$TMP/test_citymap"
"$TMP/test_citymap"
# The cartridge source against the real public headers, as the firmware sees it.
"$CC" -std=c11 -Wall -Wextra -Werror "${INC[@]}" -fsyntax-only "$HERE/game.c"
"$CC" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -DDUKE_HOST_TEST \
  "${INC[@]}" "$HERE/game.c" "$HERE/tests/prg32_stub.c" "$HERE/tests/host_harness.c" -o "$TMP/harness" 2>&1 \
  | { grep -v "reducing alignment of section" || true; }
"$TMP/harness"
echo "OK: all host checks passed"
