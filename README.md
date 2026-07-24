<div align="center">

# BTCRig

**A compact SHA256d benchmark, Stratum V1 client, and mining proxy for learning, testing, and heterogeneous compute experiments.**

[简体中文](README.zh-CN.md) · [Releases](https://github.com/lxzcl/BTCRig/releases)

![Release](https://img.shields.io/github/v/release/lxzcl/BTCRig?style=for-the-badge&color=00b894)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20Termux-00b894?style=for-the-badge)
![SHA256d](https://img.shields.io/badge/SHA256d-CPU%20%7C%20OpenCL%20%7C%20CUDA-00b894?style=for-the-badge)

</div>

BTCRig is a cross-platform C project that provides a SHA256d benchmark, a Stratum V1 mining client, and a TCP/TLS Stratum proxy, with CPU, OpenCL, and NVIDIA CUDA backends. It runs on Windows, Linux, Android/Termux, x86 PCs, ARM boards, and GPU-equipped devices, delivering the strongest possible SHA256d mining performance on general CPU/GPU platforms.

## Programs

| Program | Purpose |
| --- | --- |
| `btc_stratum` | Stratum V1 client with CPU/OpenCL/CUDA workers, TCP/TLS, reconnect, and interactive statistics |
| `btc_bench` | Local SHA256d benchmark with selectable backends |
| `btc_proxy` | Multi-client Stratum proxy with TCP/TLS auto-detection |

## Performance Snapshots

These are observed project measurements, not controlled cross-platform benchmarks. Compiler versions, clock limits, cooling, and background load can materially change the result.

| Platform | Environment | Backend | Threads | Observed SHA256d |
| --- | --- | --- | ---: | ---: |
| NVIDIA GeForce RTX 2050 Laptop GPU | Windows 11 / MSYS2 UCRT64 | CUDA driver API | 1 GPU | ~589 MH/s |
| NVIDIA GeForce RTX 2050 Laptop GPU | Windows 11 / MSYS2 UCRT64 | OpenCL modern-unrolled | 1 GPU | ~536 MH/s |
| AMD 7945HX | Windows 11 | x86-SHA-NI | 32 | ~600 MH/s |
| Snapdragon 8 Elite | Termux | ARMv8 SHA2 | 8 | ~150 MH/s |
| NanoPi Fire3 | Linux ARM64 | ARMv8 SHA2 | 8 | ~46.4 MH/s |
| NanoPi M3 | Linux ARM64 | ARMv8 SHA2 | 8 | ~46.3 MH/s |
| RockPi-S | Linux ARM64 | ARMv8 SHA2 | 4 | ~8 MH/s |
| Allwinner H3 Series | Linux Cortex-A7 | Openssl | 4 | ~1.2 MH/s |

Run the same local benchmark when comparing builds:

```bash
./build/btc_bench -t "$(nproc)" -s 10
./build/btc_bench --opencl --opencl-platform 0 --opencl-device 0 -s 10
./build/btc_bench --cuda --cuda-device 0 -s 10
./build/btc_bench --cuda-autotune --cuda-device 0 -s 2
```

## Highlights

- Automatic backend selection: x86 SHA-NI, ARMv8 SHA2, OpenSSL, or portable C.
- Optional OpenCL GPU path with compat10 fallback and OpenCL 1.2+ modern fixed-npi/register-heavy candidates; enabled by the packaged config and safely skipped when no runtime/device is available.
- Optional CUDA GPU path using the NVIDIA driver API and embedded PTX, with standard, dual nonce, fixed-npt, lop3, and fixed-lop3 kernel variants; disabled by default at runtime and does not require the CUDA Toolkit on the target machine.
- Mixed CPU/GPU nonce scheduler: GPU workers keep large dispatch batches while CPU workers use smaller chunks for faster job turnover.
- Two-lane interleaved x86 SHA-NI scanning and dedicated ARMv8 SHA2 range scanning.
- Uses every logical CPU by default; thread count remains configurable.
- Continuous network reconnect with bounded backoff.
- Plain TCP and verified or compatible TLS pool connections.
- Human-readable hashrate units and per-thread runtime statistics.

## Architecture

| Area | Files |
| --- | --- |
| SHA256d backends | `src/sha256d.c`, `src/sha256d_x86_sha_ni.c`, `src/sha256d_arm_sha2.c` |
| Worker scheduler | `src/miner.c`, `src/miner.h` |
| OpenCL worker | `src/opencl_miner.c`, `src/opencl_miner.h` |
| CUDA worker | `src/cuda_miner.c`, `src/cuda_miner.h`, `src/cuda_sha256d_kernel.cu`, `src/cuda_sha256d_ptx.h` |
| Stratum client | `src/stratum.c`, `src/stratum.h`, `src/stratum_main.c` |
| Proxy | `src/proxy_main.c` |
| Benchmark | `src/main.c` |
| Platform helpers | `src/cpu_info.c`, `src/console.c` |

The project intentionally stays close to C11, CMake, OpenSSL, pthreads, Jansson, optional OpenCL, and the dynamically loaded CUDA driver API. The goal is to keep the code easy to inspect and portable across desktop Linux, Windows/MSYS2, and Termux-style environments.

## Quick Start

### Download a release

Windows and Linux x86_64 packages are published on the [Releases page](https://github.com/lxzcl/BTCRig/releases).

On Windows, extract the zip, edit `config.json`, then run:

```powershell
.\btc_stratum.exe
```

On Linux, extract the tarball, edit `config.json`, then run:

```bash
./btc_stratum
```

### Ubuntu / Debian

```bash
wget -O ubuntu.sh https://raw.githubusercontent.com/lxzcl/BTCRig/master/ubuntu.sh
chmod +x ubuntu.sh
./ubuntu.sh
```

### Termux

```bash
pkg update
pkg install -y wget
wget -O termux.sh https://raw.githubusercontent.com/lxzcl/BTCRig/master/termux.sh
chmod +x termux.sh
./termux.sh
```

### Build from source

Ubuntu/Debian dependencies:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libssl-dev libjansson-dev git
```

CPU-only build:

```bash
git clone https://github.com/lxzcl/BTCRig.git
cd BTCRig
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=OFF -DBTCRIG_OPENCL=OFF -DBTCRIG_CUDA=OFF
cmake --build build -j"$(nproc)"
./build/btc_stratum --self-test
./build/btc_stratum
```

OpenCL-capable build:

```bash
sudo apt install -y ocl-icd-opencl-dev opencl-headers clinfo
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=OFF -DBTCRIG_OPENCL=ON -DBTCRIG_CUDA=ON
cmake --build build -j"$(nproc)"
./build/btc_stratum --opencl-self-test
./build/btc_stratum --cuda-self-test
./build/btc_stratum --opencl
./build/btc_stratum --cuda
```

OpenCL is enabled by default in the packaged `config.json`. Building with `-DBTCRIG_OPENCL=ON` only includes the GPU worker; the OpenCL runtime is loaded dynamically, so CPU-only startup still works when `OpenCL.dll` or `libOpenCL.so.1` is missing.

CUDA is also disabled by default in `config.json`. Building with `-DBTCRIG_CUDA=ON` includes the CUDA worker and `btc_bench --cuda`; runtime only needs an NVIDIA driver that provides `nvcuda.dll` on Windows or `libcuda.so.1` on Linux. The CUDA Toolkit is only needed if you want to regenerate `src/cuda_sha256d_ptx.h` with `tools/generate_cuda_ptx.sh`. Use `tools/analyze_cuda_ptx.sh sm_86` to inspect PTX-level register and instruction counts for the embedded CUDA kernels.

Termux should keep `BTC_MINER_NATIVE=OFF`. The ARM SHA2 source is still compiled with its dedicated crypto flags and selected through runtime feature detection; disabling global native tuning avoids illegal instructions on heterogeneous Android CPU clusters.

### Windows / MSYS2 UCRT64 build

Open the **MSYS2 UCRT64** terminal and run `echo $MSYSTEM`; the output must be `UCRT64`. Install the dependencies:

```bash
pacman -Syu
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-jansson \
  mingw-w64-ucrt-x86_64-pkgconf \
  git make
```

Optional OpenCL build support:

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-opencl-headers
```

CUDA support does not need extra MSYS2 packages at runtime. Install the NVIDIA display driver, then verify it with:

```bash
./build/btc_bench.exe --cuda-info
./build/btc_bench.exe --cuda-self-test --cuda-device 0
```

If `pacman -Syu` asks you to close the terminal, reopen the UCRT64 terminal before continuing. Build and test all three programs:

```bash
git clone https://github.com/lxzcl/BTCRig.git
cd BTCRig
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=OFF
cmake --build build -j"$(nproc)"
./build/btc_stratum.exe --self-test
./build/btc_proxy.exe --version
./build/btc_bench.exe -t 1 -s 1
```

Package the executables, configuration files, and required DLLs:

```bash
rm -rf dist
mkdir -p dist
cp build/btc_stratum.exe build/btc_proxy.exe build/btc_bench.exe config.json proxy.json dist/

ldd build/btc_stratum.exe build/btc_proxy.exe build/btc_bench.exe \
  | awk '{for (i=1; i<=NF; i++) if ($i ~ /^\/ucrt64\/bin\//) print $i}' \
  | sort -u \
  | xargs -r -I{} cp -u "{}" dist/
```

Copy the complete `dist/` directory when running BTCRig on another Windows machine.

## Runtime

The miner reports every usable SHA backend and the selected path at startup:

```text
[SHA] available=x86-sha-ni,openssl,fast-c
[SHA] selected=x86-sha-ni mode=auto
```

Common commands:

```text
-o, --url URL              pool URL
-u, --user USER            wallet or username
-p, --pass PASS            password
-d, --suggest-diff N       suggested initial difficulty
-t, --threads N            CPU thread count, 0 means auto
--stats N                  statistics interval in seconds
--runtime N                runtime limit, 0 means unlimited
--retries N                reconnect attempts, -1 means infinite, 0 disables reconnect
--donate-level N           donation percentage, compiled default 1, minimum 1
--no-mine                  test the connection without mining
--no-cpu                   disable CPU workers
--opencl                   enable OpenCL workers on all GPU devices by default
--opencl-all               use all OpenCL GPU devices
--opencl-platform N        OpenCL platform index
--opencl-device N          OpenCL device index
--opencl-batch N           nonce batch size per OpenCL dispatch
--opencl-local N           OpenCL local work size, 0 means automatic
--opencl-npi N             nonces scanned by each OpenCL work-item
--opencl-backend NAME      OpenCL backend: auto, compat10, or modern
--opencl-kernel NAME       OpenCL kernel variant: auto, compact, unrolled, fixed-npi1, fixed-npi2, fixed-npi4, or register-heavy
--opencl-self-test         verify the compiled OpenCL kernel without connecting to a pool
--cuda                     enable the CUDA worker
--cuda-autotune            benchmark CUDA kernel/batch/block/npt candidates without connecting to a pool
--cuda-device N            CUDA device index
--cuda-batch N             nonce batch size per CUDA dispatch
--cuda-block N             CUDA threads per block
--cuda-npt N               nonces scanned by each CUDA thread
--cuda-kernel NAME         CUDA kernel variant: standard, dual, fixed-npt1, fixed-npt2, fixed-npt4, lop3, fixed-lop3-npt1, fixed-lop3-npt2, or fixed-lop3-npt4
--cuda-self-test           verify the embedded CUDA PTX without connecting to a pool
--autotune                 force first-run CPU/GPU benchmark and update config
--no-autotune              skip automatic first-run benchmark
--autotune-seconds N       seconds per benchmark mode, default 1.5
--cpu-info                 print CPU topology
--self-test                run the Stratum parser self-test
```

Interactive keys while mining:

| Key | Action |
| --- | --- |
| `h` | Per-thread hashrate |
| `p` | Pause mining |
| `r` | Resume mining |
| `s` | Share results |
| `c` | Connection information |

## Configuration

`btc_stratum` reads `config.json` from the current directory. Replace the example wallet before mining for yourself.

```json
{
  "autosave": true,
  "autotune": {
    "enabled": true,
    "cpu-self-test": false,
    "gpu-self-test": false,
    "seconds": 1.5
  },
  "cpu": {
    "enabled": true,
    "threads": 0
  },
  "opencl": {
    "enabled": true,
    "all-devices": true,
    "platform": 0,
    "device": 0,
    "batch-size": 1048576,
    "local-work-size": 0,
    "nonces-per-work-item": 1,
    "backend": "auto",
    "kernel": "auto",
    "max-results": 256
  },
  "cuda": {
    "enabled": false,
    "device": 0,
    "batch-size": 4194304,
    "threads-per-block": 256,
    "nonces-per-thread": 1,
    "kernel": "standard",
    "max-results": 256
  },
  "pools": [
    {
      "url": "stratum+tls://public-pool.io:14333",
      "user": "bc1q_example_wallet.worker",
      "pass": "x",
      "diff": 0.001
    }
  ],
  "retries": -1,
  "retry-pause": 2,
  "donate-level": 1,
  "print-time": 10,
  "runtime": 0
}
```

The pool controls the effective share difficulty through `mining.set_difficulty`; `diff` is only an initial suggestion.

`retries` defaults to `-1` for unattended infinite reconnects. Set it to `0` to try once without reconnecting, or to a positive number to cap reconnect attempts.

OpenCL is enabled by default in the packaged config, while CUDA remains opt-in at runtime. If the build machine has OpenCL headers, `btc_stratum` includes the OpenCL worker by default; otherwise that path is skipped. OpenCL and CUDA are loaded through runtime driver loading, so enabling either GPU backend without a usable runtime/device prints a warning and keeps the CPU path available. When OpenCL is enabled and no specific device list is configured, all OpenCL GPU devices are used; CUDA currently uses one selected NVIDIA device.

CPU, OpenCL, and CUDA workers share one nonce allocator, so ranges do not overlap. In CPU-only mode CPU workers use larger nonce chunks; when any GPU worker is active, CPU chunks are reduced while GPU workers keep their configured `batch-size`. New jobs, pause/resume, and shutdown wake waiting workers directly instead of relying on periodic polling.

With the default `autotune.enabled=true`, `autotune.cpu-self-test=false`, and `autotune.gpu-self-test=false`, the first normal mining run performs an offline self-test and benchmark before connecting to the pool. CPU and GPU completion flags are tracked separately: a CPU-only run only sets `cpu-self-test=true`, so enabling OpenCL or CUDA later still triggers GPU tuning while preserving the CPU result. GPU modes are benchmarked when `opencl.enabled=true`, `cuda.enabled=true`, or the matching command-line option is passed; the packaged config enables OpenCL by default. If both GPU backends are disabled, autotune stays CPU-only and preserves both as disabled. If OpenCL is enabled, it first tunes each OpenCL GPU with staged `backend`, `kernel`, `local-work-size`, `nonces-per-work-item`, and `batch-size` probes, then measures CPU-only, all-GPU, CPU+all-GPU, half-CPU+all-GPU, each single GPU, CPU+each single GPU, and for systems with more than two GPUs the "all GPUs except one" cases. If CUDA is enabled, it warms the selected NVIDIA device, tunes `kernel`, `threads-per-block`, `nonces-per-thread`, and `batch-size`, then measures CUDA-only, CPU+CUDA, and half-CPU+CUDA. The fastest mode is written back to `config.json` together with the measured hashrates. Legacy `autotune.self-test`, `self_test`, `done`, and `completed` fields are still accepted as CPU completion flags for older configs.

This deliberately avoids trying every possible CPU/GPU subset. The high-value modes catch the common cases: a discrete GPU plus an integrated GPU, CPU contention with the GPU driver, and one slow or unstable GPU dragging down the group. Use `--autotune` to rerun the benchmark after changing drivers, clocks, hardware, OpenCL batch/local/npi settings, or CUDA kernel/batch/block/npt settings.

## Backends

| Backend | Availability | Notes |
| --- | --- | --- |
| `x86-sha-ni` | x86 CPU with SHA extensions | Preferred x86 path, two interleaved nonce lanes |
| `arm-sha2` | ARMv8 CPU with SHA2 extensions | Dedicated ARM range scanner |
| `openssl` | All supported builds | Library fallback |
| `fast-c` | All supported builds | Portable C fallback |
| `opencl` | Optional `btc_stratum` worker | Runtime-loaded OpenCL GPU worker with `compat10` fallback and `modern` OpenCL 1.2+ fixed-npi/register-heavy candidate selection |
| `cuda` | Optional `btc_stratum` worker and `btc_bench --cuda` | NVIDIA GPU worker using runtime CUDA driver loading and embedded PTX |

Override automatic selection with `BTC_MINER_SHA_BACKEND`, for example:

```bash
BTC_MINER_SHA_BACKEND=openssl ./build/btc_bench -t "$(nproc)" -s 10
```

OpenCL can be enabled from `config.json` or from the command line:

```bash
./build/btc_stratum --opencl
./build/btc_stratum --no-cpu --opencl --opencl-platform 0 --opencl-device 0
./build/btc_stratum --opencl-self-test --opencl-platform 0 --opencl-device 0
```

CUDA can be enabled from `config.json` or from the command line:

```bash
./build/btc_bench --cuda-info
./build/btc_bench --cuda-autotune --cuda-device 0 -s 2
./build/btc_bench --cuda --cuda-device 0 -s 10
./build/btc_bench --cuda --cuda-device 0 --cuda-kernel lop3 -s 10
./build/btc_bench --cuda --cuda-device 0 --cuda-kernel fixed-lop3-npt1 -s 10
./build/btc_bench --cuda --cuda-device 0 --cuda-kernel fixed-npt1 -s 10
./build/btc_bench --cuda --cuda-device 0 --cuda-kernel dual --cuda-npt 2 -s 10
./build/btc_stratum --no-cpu --cuda --cuda-device 0
./build/btc_stratum --cuda-self-test --cuda-device 0
```

`cuda.kernel` accepts `standard`, `dual`, `fixed-npt1`, `fixed-npt2`, `fixed-npt4`, `lop3`, `fixed-lop3-npt1`, `fixed-lop3-npt2`, or `fixed-lop3-npt4`. The default is `standard`; `--cuda-autotune` tests the available variants and keeps the fastest stable result for the selected GPU. Fixed-npt kernels force their matching `nonces-per-thread` value at runtime, `lop3` uses inline PTX ternary-logic instructions for SHA256 `CH` and `MAJ`, and fixed-lop3 combines both choices.

Multiple OpenCL GPUs can be selected explicitly:

```json
"opencl": {
  "enabled": true,
  "all-devices": false,
  "devices": [
    { "platform": 0, "device": 0, "backend": "modern", "batch-size": 1048576, "local-work-size": 256, "nonces-per-work-item": 1, "kernel": "fixed-npi1" },
    { "platform": 1, "device": 0, "backend": "compat10", "batch-size": 524288, "local-work-size": 128, "nonces-per-work-item": 2, "kernel": "compact" }
  ]
}
```

Each OpenCL device and the selected CUDA device run a self-test before mining starts. Devices that fail the self-test are skipped while any working CPU or GPU workers continue.

`backend=compat10` avoids OpenCL 2.x APIs and uses only OpenCL 1.0 host APIs. OpenCL 1.0 devices need `cl_khr_global_int32_base_atomics`; OpenCL 1.1+ devices can use core global int32 atomics. `backend=modern` is the OpenCL 1.2+ candidate path. `backend=auto` benchmarks compat10 and modern when the device supports both. `kernel=unrolled` is the high-throughput SHA256d path, while `kernel=compact` keeps a smaller loop-based compressor for older drivers or devices with lower register capacity. `kernel=fixed-npi1`, `fixed-npi2`, and `fixed-npi4` are modern-only kernels with the nonce count per work-item fixed at compile selection time. `kernel=register-heavy` is a modern-only two-nonce vector-register candidate with npi fixed to 2. Autotune tests modern-only kernels only with `backend=modern`; `kernel=auto` benchmarks compact/unrolled and the modern candidates, then keeps the fastest stable result.

## Documentation

- [Proxy guide](PROXY.md)
- [Chinese README](README.zh-CN.md)
- [Release downloads](https://github.com/lxzcl/BTCRig/releases)

## Responsible Use

- Mining and benchmarking can keep CPUs and GPUs under sustained load. Watch cooling, power, and battery conditions.
- Release builds do not hide background services; run the binaries from a terminal and review `config.json`.
- OpenCL and CUDA devices run a self-test before mining. Devices that fail are skipped while remaining CPU/GPU workers can continue.
- GitHub Actions builds packages but intentionally avoids running miner benchmarks on hosted runners.

## Developers
The software includes a 1% developer donation by default (approximately 1 minute donated out of every 100), which applies to all mining modes. Currently, there is no way to automatically distinguish between PPLNS pool mining and solo mode in code. Pool addresses use PPLNS by default. If you are using solo mode, please note: there is an extremely small chance that a block is found during a donation interval, resulting in the entire block reward being donated. To modify the donation percentage, edit the donation parameter in the source code and recompile.
BTC:bc1qqz0wutk9kk5mmaf7fu4dm5w4fq4fhaah9hpzr3

## License

BTCRig is distributed under the [GNU General Public License v3.0](LICENSE).
