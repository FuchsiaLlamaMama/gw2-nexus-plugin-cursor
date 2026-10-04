#!/usr/bin/env bash
# Off-game build+run of the cursor-core doctest suite on macOS, without cmake.
#
# Hand-compiles the same sources CMakeLists builds for `cursor-core-tests` and
# runs them, exiting 0 (all green) / non-zero (red). It mirrors the CMake recipe:
#   cursor-core  = core/{cursor_store,marker,clip_freeze,visibility}.cpp
#   shared-core  = persistence/atomic_file.cpp
#   test driver  = tests/test_cursor_core.cpp   (doctest, WITH_MAIN)
#
# The CommandLineTools toolchain ships an incomplete top-level libc++, so we
# -nostdinc++ and point at the SDK's complete c++/v1.
#
# Optional selector: this runner builds exactly ONE test file, so the only valid
# selector is that file's path. Any other selector (or more than one) is
# refused, never run as a whole-suite stand-in; the message starts with
# "no test found" and exits 2 so a calling tool can tell it apart from a red run.
set -euo pipefail

if [ "$#" -gt 1 ]; then
  echo "no test found: run_offgame.sh takes at most one selector (got $#)" >&2
  exit 2
fi
if [ "$#" -eq 1 ] && [ -n "$1" ]; then
  case "$1" in
    tests/test_cursor_core.cpp|*/tests/test_cursor_core.cpp) ;;
    *) echo "no test found: run_offgame.sh only builds tests/test_cursor_core.cpp (got '$1')" >&2
       exit 2 ;;
  esac
fi

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cursor="$(cd "$here/.." && pwd)"       # tests -> addon root
# Dependencies sit inside the addon root when it is built on its own (shared/ and
# vendor/ are submodules there), or one level up when it is a subdirectory of a
# larger build that provides them.
if [ -d "$cursor/shared/persistence" ]; then deps="$cursor"; else deps="$(cd "$cursor/.." && pwd)"; fi
sdk="$(xcrun --show-sdk-path)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
out="$work/cursor-core-tests"

xcrun clang++ -std=c++17 -O0 -g \
  -nostdinc++ -isysroot "$sdk" -isystem "$sdk/usr/include/c++/v1" \
  -I"$cursor" -I"$deps/shared" -I"$deps/vendor" \
  "$cursor/tests/test_cursor_core.cpp" \
  "$cursor/core/cursor_store.cpp" \
  "$cursor/core/marker.cpp" \
  "$cursor/core/clip_freeze.cpp" \
  "$cursor/core/visibility.cpp" \
  "$deps/shared/persistence/atomic_file.cpp" \
  -o "$out"

# Run under the EXIT trap (not exec) so the scratch dir is cleaned up, and
# propagate the suite's exit code (0 green / non-zero red) as ours.
status=0
"$out" || status=$?
exit "$status"
