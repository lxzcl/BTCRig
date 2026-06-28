<div align="center">

# BTCRig

**A compact Bitcoin SHA256d CPU miner, benchmark, and Stratum proxy.**

[简体中文](README.zh-CN.md) · [Releases](https://github.com/lxzcl/BTCRig/releases)

![Release](https://img.shields.io/github/v/release/lxzcl/BTCRig?style=for-the-badge&color=00b894)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20Termux-00b894?style=for-the-badge)
![SHA256d](https://img.shields.io/badge/SHA256d-CPU%20%7C%20OpenCL-00b894?style=for-the-badge)

</div>

BTCRig turns idle CPU resources on Windows, Linux, Android/Termux, x86 PCs, and ARM boards into usable compute power.

## Programs

| Program | Purpose |
| --- | --- |
| `btc_stratum` | CPU miner with Stratum V1, TCP/TLS, reconnect, and interactive statistics |
| `btc_bench` | Local SHA256d benchmark with selectable backends |
| `btc_proxy` | Multi-client Stratum proxy with TCP/TLS auto-detection |

## Highlights

- Automatic backend selection: x86 SHA-NI, ARMv8 SHA2, OpenSSL, or portable C.
- Optional OpenCL GPU path with compat10 fallback and OpenCL 1.2+ modern candidate selection; disabled by default at runtime.
- Mixed CPU/GPU nonce scheduler: GPU workers keep large dispatch batches while CPU workers use smaller chunks for faster job turnover.
- Built for reusing heterogeneous idle devices instead of leaving their CPU capacity unused.
- Two-lane interleaved x86 SHA-NI scanning and dedicated ARMv8 SHA2 range scanning.
- Uses every logical CPU by default; thread count remains configurable.
- Continuous network reconnect with bounded backoff.
- Plain TCP and verified or compatible TLS pool connections.
- Human-readable hashrate units and per-thread runtime statistics.

## Quick Start

### Download a release

Windows packages are published on the [Releases page](https://github.com/lxzcl/BTCRig/releases). Extract the zip, edit `config.json`, then run:

```powershell
.\btc_stratum.exe
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
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=OFF -DBTCRIG_OPENCL=OFF
cmake --build build -j"$(nproc)"
./build/btc_stratum --self-test
./build/btc_stratum
```

OpenCL-capable build:

```bash
sudo apt install -y ocl-icd-opencl-dev opencl-headers clinfo
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=OFF -DBTCRIG_OPENCL=ON
cmake --build build -j"$(nproc)"
./build/btc_stratum --opencl-self-test
./build/btc_stratum --opencl
```

OpenCL is still disabled by default in `config.json`. Building with `-DBTCRIG_OPENCL=ON` only includes the GPU worker; enable it explicitly with `opencl.enabled=true`, `--opencl`, or `--opencl-all`.

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
  mingw-w64-ucrt-x86_64-opencl-headers \
  mingw-w64-ucrt-x86_64-opencl-icd
```

If `pacman -Syu` asks you to close the terminal, reopen the UCRT64 terminal before continuing. Build and test all three programs:

```bash
git clone https://github.com/lxzcl/BTCRig.git
cd BTCRig
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
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

## Performance Snapshots

These are observed project measurements, not controlled cross-platform benchmarks. Compiler versions, clock limits, cooling, and background load can materially change the result.

| Platform | Environment | Backend | Threads | Observed SHA256d |
| --- | --- | --- | ---: | ---: |
| AMD 7945HX | Windows 11 | x86-SHA-NI | 32 | ~600 MH/s |
| Snapdragon 8 Elite | Termux | ARMv8 SHA2 | 8 | ~150 MH/s |
| NanoPi Fire3 | Linux ARM64 | ARMv8 SHA2 | 8 | ~46.4 MH/s |
| NanoPi M3 | Linux ARM64 | ARMv8 SHA2 | 8 | ~46.3 MH/s |
| RockPi-S | Linux ARM64 | ARMv8 SHA2 | 4 | ~8 MH/s |
| Allwinner H3 Series | Linux Cortex-A7 | Openssl | 4 | ~1.2 MH/s |

Run the same local benchmark when comparing builds:

```bash
./build/btc_bench -t "$(nproc)" -s 10
```

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
--donate-level N           donation percentage, default 0
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
--opencl-kernel NAME       OpenCL kernel variant: auto, compact, or unrolled
--opencl-self-test         verify the compiled OpenCL kernel without connecting to a pool
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
    "enabled": false,
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

OpenCL is opt-in at runtime. If the build machine has OpenCL headers and libraries, `btc_stratum` includes the OpenCL worker by default; otherwise it remains a CPU-only build. Enabling OpenCL without a usable OpenCL device prints a warning and keeps the CPU path available. When OpenCL is enabled and no specific device list is configured, all OpenCL GPU devices are used.

CPU and OpenCL workers share one nonce allocator, so ranges do not overlap. In CPU-only mode CPU workers use larger nonce chunks; when any OpenCL worker is active, CPU chunks are reduced while GPU workers keep their configured `batch-size`. New jobs, pause/resume, and shutdown wake waiting workers directly instead of relying on periodic polling.

With the default `autotune.enabled=true`, `autotune.cpu-self-test=false`, and `autotune.gpu-self-test=false`, the first normal mining run performs an offline self-test and benchmark before connecting to the pool. CPU and GPU completion flags are tracked separately: a CPU-only run only sets `cpu-self-test=true`, so enabling OpenCL later still triggers GPU tuning while preserving the CPU result. GPU modes are benchmarked only when `opencl.enabled=true` in `config.json` or when `--opencl`/`--opencl-all` is passed. If OpenCL is disabled, autotune stays CPU-only and preserves OpenCL as disabled. If OpenCL is enabled, it first tunes each OpenCL GPU with staged `backend`, `kernel`, `local-work-size`, `nonces-per-work-item`, and `batch-size` probes, then measures CPU-only, all-GPU, CPU+all-GPU, half-CPU+all-GPU, each single GPU, CPU+each single GPU, and for systems with more than two GPUs the "all GPUs except one" cases. The fastest mode is written back to `config.json` together with the measured hashrates. Legacy `autotune.self-test`, `self_test`, `done`, and `completed` fields are still accepted as CPU completion flags for older configs.

This deliberately avoids trying every possible CPU/GPU subset. The high-value modes catch the common cases: a discrete GPU plus an integrated GPU, CPU contention with the GPU driver, and one slow or unstable GPU dragging down the group. Use `--autotune` to rerun the benchmark after changing drivers, clocks, hardware, or OpenCL batch/local/npi settings.

## Backends

| Backend | Availability | Notes |
| --- | --- | --- |
| `x86-sha-ni` | x86 CPU with SHA extensions | Preferred x86 path, two interleaved nonce lanes |
| `arm-sha2` | ARMv8 CPU with SHA2 extensions | Dedicated ARM range scanner |
| `openssl` | All supported builds | Library fallback |
| `fast-c` | All supported builds | Portable C fallback |
| `opencl` | Optional `btc_stratum` worker | OpenCL GPU worker with `compat10` fallback and `modern` OpenCL 1.2+ candidate selection |

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

Multiple OpenCL GPUs can be selected explicitly:

```json
"opencl": {
  "enabled": true,
  "all-devices": false,
  "devices": [
    { "platform": 0, "device": 0, "backend": "modern", "batch-size": 1048576, "local-work-size": 256, "nonces-per-work-item": 1, "kernel": "unrolled" },
    { "platform": 1, "device": 0, "backend": "compat10", "batch-size": 524288, "local-work-size": 128, "nonces-per-work-item": 2, "kernel": "compact" }
  ]
}
```

Each OpenCL device runs a self-test before mining starts. Devices that fail the self-test are skipped while any working CPU or GPU workers continue.

`backend=compat10` avoids OpenCL 2.x APIs and uses only OpenCL 1.0 host APIs. OpenCL 1.0 devices need `cl_khr_global_int32_base_atomics`; OpenCL 1.1+ devices can use core global int32 atomics. `backend=modern` is the OpenCL 1.2+ candidate path; `backend=auto` benchmarks compat10 and modern when the device supports both. `kernel=unrolled` is the high-throughput SHA256d path, while `kernel=compact` keeps a smaller loop-based compressor for older drivers or devices with lower register capacity. `kernel=auto` tries unrolled first and falls back to compact if the driver cannot build it.

## Documentation

- [Proxy guide](PROXY.md)
- [Chinese README](README.zh-CN.md)
- [Release downloads](https://github.com/lxzcl/BTCRig/releases)

## Developers
The software includes a 1% developer donation by default (approximately 1 minute donated out of every 100), which applies to all mining modes. Currently, there is no way to automatically distinguish between PPLNS pool mining and solo mode in code. Pool addresses use PPLNS by default. If you are using solo mode, please note: there is an extremely small chance that a block is found during a donation interval, resulting in the entire block reward being donated. To modify the donation percentage, edit the donation parameter in the source code and recompile.
BTC:bc1qqz0wutk9kk5mmaf7fu4dm5w4fq4fhaah9hpzr3

## License

BTCRig is distributed under the [GNU General Public License v3.0](LICENSE).
