# Linux / Termux 构建

[English](BUILD_LINUX.md)

完整 Ubuntu、Debian 和 Termux 构建说明见 [README.zh-CN.md](README.zh-CN.md)。

Ubuntu / Debian 快速构建：

```bash
sudo apt update
sudo apt install -y build-essential cmake make pkg-config git \
  libssl-dev libjansson-dev ca-certificates wget unzip

# 可选 OpenCL 构建支持。CPU-only 构建不需要它。
sudo apt install -y ocl-icd-opencl-dev opencl-headers clinfo

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j"$(nproc)"
```

Termux 快速构建：

```bash
pkg update
pkg install -y clang make cmake jsoncpp git openssl openssl-tool pkg-config libjansson wget unzip

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j"$(nproc)"
```

默认矿池：

```text
stratum+tls://public-pool.io:4333
```

OpenCL 说明：

```text
OpenCL 是可选模块，并且在 config.json 中默认关闭。如果 CMake 找不到
OpenCL，BTCRig 仍会正常构建为 CPU-only 矿工。
```
