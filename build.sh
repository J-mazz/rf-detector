#!/usr/bin/env bash
# build.sh — canonical build for rfdet.
#
#   ./build.sh                     release build with the default g++
#   CXX=g++-16 ./build.sh          pick a compiler
#   ./build.sh --tsan              ThreadSanitizer build (tests)
#   ./build.sh --march cortex-a76  Pi 5; use cortex-a78ae for Orin, native for dev boxes
#   ./build.sh --out build-arm64   output directory
#
# GCC >= 16: -std=c++26 -fmodules --compile-std-module, `import std;`
# GCC 15   : -std=c++23 -fmodules, std module built from bits/std.cc
# GCC 14   : -std=c++23 -fmodules-ts, headers in the global module fragment (CI fallback only)
# Modules are compiled in dependency order; CMIs land in $OUT/gcm.cache.
set -euo pipefail

CXX="${CXX:-g++}"
OUT="build"
TSAN=0
MARCH=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --tsan) TSAN=1; shift ;;
    --out) OUT="$2"; shift 2 ;;
    --march) MARCH="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

MAJOR="$("$CXX" -dumpfullversion -dumpversion | cut -d. -f1)"
SRC="$(cd "$(dirname "$0")" && pwd)/src"
mkdir -p "$OUT"
cd "$OUT"

COMMON=(-Wall -Wextra -Wpedantic -Werror=return-type -ffp-contract=off -pthread -fno-exceptions -fno-rtti)
# -fno-exceptions/-fno-rtti: the dataplane has neither; if a control-plane
# unit needs them later, compile that unit separately.
if [[ $TSAN -eq 1 ]]; then
  COMMON+=(-O1 -g -fsanitize=thread)
else
  COMMON+=(-O2 -g)
fi
[[ -n "$MARCH" ]] && COMMON+=("-mcpu=$MARCH")

STD_IMPORT=0
if [[ "$MAJOR" -ge 16 ]]; then
  FLAGS=(-std=c++26 -fmodules -DRF_IMPORT_STD=1)
  STD_IMPORT=1
elif [[ "$MAJOR" -eq 15 ]]; then
  FLAGS=(-std=c++23 -fmodules -DRF_IMPORT_STD=1)
  STD_IMPORT=1
else
  FLAGS=(-std=c++23 -fmodules-ts)
fi

MODULES=(core memory runtime signal simd dsp acquire pipeline)
OBJS=()

STD_STEP=()
if [[ $STD_IMPORT -eq 1 ]]; then
  if [[ "$MAJOR" -ge 16 ]]; then
    # GCC 16: --compile-std-module builds the std / std.compat CMIs (and the
    # libstdc++ aggregate header unit) with the SAME flags, before the first
    # source on the command line — so it rides on the rf.core compile.
    STD_STEP=(--compile-std-module)
  else
    "$CXX" "${FLAGS[@]}" "${COMMON[@]}" -fsearch-include-path -c bits/std.cc -o std.o
    OBJS+=(std.o)
  fi
fi

for m in "${MODULES[@]}"; do
  echo "  module rf.$m"
  "$CXX" "${FLAGS[@]}" "${COMMON[@]}" "${STD_STEP[@]}" -x c++ -c "$SRC/rf/$m.cppm" -o "rf.$m.o"
  STD_STEP=()
  OBJS+=("rf.$m.o")
done

link() {  # name source...
  local name="$1"; shift
  echo "  link $name"
  "$CXX" "${FLAGS[@]}" "${COMMON[@]}" "$@" "${OBJS[@]}" -o "$name"
}
link rfdet "$SRC/main.cpp"
link test_memory "$SRC/../tests/test_memory.cpp"
link test_simd "$SRC/../tests/test_simd.cpp"
link test_pipeline "$SRC/../tests/test_pipeline.cpp"
echo "built in $OUT with $CXX $("$CXX" -dumpfullversion) (import std: $STD_IMPORT, tsan: $TSAN)"
