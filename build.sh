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
SOAPY=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --soapy) SOAPY=1; shift ;;
    --tsan) TSAN=1; shift ;;
    --out) [[ $# -ge 2 ]] || { echo "--out needs a directory" >&2; exit 2; }; OUT="$2"; shift 2 ;;
    --march) [[ $# -ge 2 ]] || { echo "--march needs a CPU/architecture" >&2; exit 2; }; MARCH="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

MAJOR="$("$CXX" -dumpfullversion -dumpversion | cut -d. -f1)"
if [[ "$MAJOR" -lt 14 ]]; then
  echo "Native module build requires GCC >=14; use scripts/verify_portable.py for GCC13 body verification." >&2
  exit 2
fi
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
if [[ -n "$MARCH" ]]; then
  TARGET="$("$CXX" -dumpmachine)"
  case "$TARGET" in
    aarch64*|arm*) COMMON+=("-mcpu=$MARCH") ;;
    *) COMMON+=("-march=$MARCH") ;;
  esac
fi
LIBS=()
if [[ $SOAPY -eq 1 ]]; then
  COMMON+=(-DRF_WITH_SOAPY=1)
  LIBS+=(-lSoapySDR)
fi

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

MODULES=(core memory signal runtime health output simd fft dsp acquire pipeline)
[[ $SOAPY -eq 1 ]] && MODULES+=(soapy)
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
  "$CXX" "${FLAGS[@]}" "${COMMON[@]}" "$@" "${OBJS[@]}" "${LIBS[@]}" -o "$name"
}
link rfdet "$SRC/main.cpp"
link test_memory "$SRC/../tests/test_memory.cpp"
link test_simd "$SRC/../tests/test_simd.cpp"
link test_pipeline "$SRC/../tests/test_pipeline.cpp"
link test_fft "$SRC/../tests/test_fft.cpp"
link test_dsp "$SRC/../tests/test_dsp.cpp"
link test_acquire "$SRC/../tests/test_acquire.cpp"
link test_regression "$SRC/../tests/test_regression.cpp"
link test_health "$SRC/../tests/test_health.cpp"
link test_runtime "$SRC/../tests/test_runtime.cpp"
link test_output "$SRC/../tests/test_output.cpp"
link test_dsp_boundaries "$SRC/../tests/test_dsp_boundaries.cpp"
if [[ $SOAPY -eq 1 ]]; then
  link test_soapy "$SRC/../tests/test_soapy.cpp"
fi
echo "built in $OUT with $CXX $("$CXX" -dumpfullversion) (import std: $STD_IMPORT, tsan: $TSAN)"
