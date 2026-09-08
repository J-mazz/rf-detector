# rfdet

Vehicular RF sensor dataplane (Pi 5 + Orin) — see `DESIGN.md`.

## Build

```sh
# Pi 5 / Fedora 44 (GCC 16.1.1): C++26 modules, import std
./build.sh --march cortex-a76

# dev box
./build.sh                 # default g++; GCC 16+: import std; GCC 14/15: header fallback
./build.sh --tsan          # ThreadSanitizer build of the same sources
CXX=aarch64-linux-gnu-g++-14 ./build.sh --out build-arm64 --march cortex-a76   # cross

# CMake (secondary; header fallback, Ninja required)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
```

Outputs land in `build/` (or `--out`): `rfdet`, `test_memory`, `test_simd`, `test_pipeline`.

## Run

```sh
./build/rfdet --seconds 10            # mock source, paced at 30.72 MS/s, locked memory
./build/rfdet --seconds 10 --no-lock  # without mlock (dev)
./build/rfdet --unpaced               # throughput mode
```

`rfdet` refuses to start with `memlock_budget` when `RLIMIT_MEMLOCK` (8 MiB
default) is below the 11.8 MB the pools need. Fix it in the unit, not with
`ulimit` in a shell:

```ini
# /etc/systemd/system/rfdet.service
[Service]
ExecStart=/opt/rfdet/rfdet
LimitMEMLOCK=64M
LimitRTPRIO=50
CPUAffinity=1 2 3
```

with `isolcpus=1-3 nohz_full=1-3` on the kernel command line and
`acquisition_cpu/dsp_cpu/sink_cpu` set in `PipelineConfig`.

## Tests

| Test | What it proves | Validated on |
|---|---|---|
| `test_memory` | page-rounded pinned regions, memlock accounting, free-list exhaustion/double-release detection, queue capacity/wrap, 300k cross-thread slot handoffs with payload verification | x86-64 (GCC 14, TSan clean), aarch64 (qemu) |
| `test_simd` | exact int16→float conversion, |x|² and sum against double reference; NEON == scalar **bitwise** for tails 0..63 and full frames | x86-64, aarch64 (qemu, NEON emitted: `ld2 sxtl scvtf fmul fadd faddp`, no `fmla`) |
| `test_pipeline` | three-stage mock run: partial reads honored, retained-slot backpressure, `processed == captured` after stop, all slots returned, zero protocol violations, floor ≈ −46 dBFS, burst SNR ≈ 22 dB | x86-64 (TSan clean), aarch64 (qemu) |

Not yet validated here: the `import std;` path itself (needs GCC 16; the
sources are identical, only the module preamble differs).

## Layout

```
src/rf/*.cppm   module interface units (dependency order in build.sh)
src/main.cpp    pipeline host
tests/          three executables, plain asserts, exit code is the verdict
build.sh        canonical build
CMakeLists.txt  secondary build
DESIGN.md       sensor plan, evidence model, invariants, toolchain topology
```
