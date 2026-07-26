# Linux / Termux Build

[简体中文](BUILD_LINUX.zh-CN.md)

The full Ubuntu, Debian, and Termux build instructions are in [README.md](README.md).

Ubuntu / Debian quick build:

```bash
sudo apt update
sudo apt install -y build-essential cmake make pkg-config git \
  libssl-dev libjansson-dev ca-certificates wget unzip

# Optional OpenCL build support. CPU-only builds do not require it.
# CMake builds the GPU backend when headers are present.
sudo apt install -y ocl-icd-opencl-dev opencl-headers clinfo

# CUDA support uses runtime libcuda loading. Install the NVIDIA driver on CUDA hosts.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON -DBTCRIG_CUDA=ON
cmake --build build -j"$(nproc)"
```

Ubuntu / Debian one-step installer:

```bash
wget -O ubuntu.sh https://raw.githubusercontent.com/lxzcl/BTCRig/master/ubuntu.sh
chmod +x ubuntu.sh
./ubuntu.sh
```

Termux quick build:

```bash
pkg update
pkg install -y clang make cmake jsoncpp git openssl openssl-tool pkg-config libjansson wget unzip

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=OFF -DBTCRIG_OPENCL=OFF -DBTCRIG_CUDA=OFF
cmake --build build -j"$(nproc)"
```

Termux one-step installer:

```bash
wget -O termux.sh https://raw.githubusercontent.com/lxzcl/BTCRig/master/termux.sh
chmod +x termux.sh
./termux.sh
```

Installer environment variables:

```text
BTC_URL         source zip URL, defaults to the master branch archive
INSTALL_DIR     install directory, defaults to ~/BTCRig
BTCRIG_NATIVE   ON/OFF native CPU tuning, Ubuntu default ON, Termux default OFF
BTCRIG_OPENCL   ON/OFF OpenCL backend, Ubuntu default ON, Termux default OFF
BTCRIG_CUDA     ON/OFF CUDA driver backend, Ubuntu default ON, Termux default OFF
BTCRIG_RUN      1 runs btc_stratum after build, 0 only builds
```

Default pool:

```text
stratum+tls://public-pool.io:14333
```

OpenCL note:

```text
OpenCL is optional at build time. If CMake cannot find OpenCL headers, BTCRig
still builds normally as a CPU-only miner. OpenCL and CUDA are loaded dynamically
from GPU drivers and stay disabled until `opencl.enabled=true`, `cuda.enabled=true`,
`--opencl`, or `--cuda` is used. On the first normal mining run,
`autotune.enabled=true` makes `btc_stratum` self-test CPU/GPU modes, save the
fastest mode and CPU/GPU autotune completion flags to config.json. Use
`btc_stratum --opencl-self-test` or `btc_stratum --cuda-self-test` to verify GPU
kernels without connecting to a pool, or `btc_stratum --autotune` to rerun the
benchmark after changing drivers or hardware.
Set `BTCRIG_OPENCL_LIBRARY=/path/to/libOpenCL.so` when the runtime is installed
under a non-standard name. On Termux, `BTCRIG_OPENCL=ON ./termux.sh` installs the
headers and OpenCL ICD/vendor bridge packages.
```
