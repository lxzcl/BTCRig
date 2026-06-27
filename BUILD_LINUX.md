# Linux / Termux Build

[简体中文](BUILD_LINUX.zh-CN.md)

The full Ubuntu, Debian, and Termux build instructions are in [README.md](README.md).

Ubuntu / Debian quick build:

```bash
sudo apt update
sudo apt install -y build-essential cmake make pkg-config git \
  libssl-dev libjansson-dev ca-certificates wget unzip

# Optional OpenCL build support. CPU-only builds do not require it.
# CMake builds the compat10 GPU backend by default when these are present.
sudo apt install -y ocl-icd-opencl-dev opencl-headers clinfo

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j"$(nproc)"
```

Termux quick build:

```bash
pkg update
pkg install -y clang make cmake jsoncpp git openssl openssl-tool pkg-config libjansson wget unzip

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j"$(nproc)"
```

Default pool:

```text
stratum+tls://public-pool.io:4333
```

OpenCL note:

```text
OpenCL is optional at build time. If CMake cannot find OpenCL, BTCRig still
builds normally as a CPU-only miner. On the first normal mining run,
`autotune.enabled=true` makes `btc_stratum` self-test CPU/GPU modes, save the
fastest mode to config.json, and set `autotune.self-test=true`. Use
`btc_stratum --opencl-self-test` to verify the compiled kernel without
connecting to a pool, or `btc_stratum --autotune` to rerun the benchmark after
changing drivers or hardware.
```
