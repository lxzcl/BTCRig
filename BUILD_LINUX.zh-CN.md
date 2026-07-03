# Linux / Termux 构建

[English](BUILD_LINUX.md)

完整 Ubuntu、Debian 和 Termux 构建说明见 [README.zh-CN.md](README.zh-CN.md)。

Ubuntu / Debian 快速构建：

```bash
sudo apt update
sudo apt install -y build-essential cmake make pkg-config git \
  libssl-dev libjansson-dev ca-certificates wget unzip

# 可选 OpenCL 构建支持。CPU-only 构建不需要它。
# 安装后 CMake 会默认编译 compat10 GPU 后端。
sudo apt install -y ocl-icd-opencl-dev opencl-headers clinfo

# CUDA 通过运行时 libcuda 加载；CUDA 主机需要安装 NVIDIA 驱动。
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON -DBTCRIG_CUDA=ON
cmake --build build -j"$(nproc)"
```

Termux 快速构建：

```bash
pkg update
pkg install -y clang make cmake jsoncpp git openssl openssl-tool pkg-config libjansson wget unzip

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=OFF -DBTCRIG_OPENCL=OFF -DBTCRIG_CUDA=OFF
cmake --build build -j"$(nproc)"
```

默认矿池：

```text
stratum+tls://public-pool.io:14333
```

OpenCL 说明：

```text
OpenCL 是构建时可选模块。如果 CMake 找不到 OpenCL，BTCRig 仍会正常构建为
CPU-only 矿工。CUDA 会从 NVIDIA 驱动动态加载，只有设置 `cuda.enabled=true`
或传入 `--cuda` 才会启用。第一次正常挖矿启动时，`autotune.enabled=true` 会让
`btc_stratum` 离线自检 CPU/GPU 模式，把最快模式和 CPU/GPU 调优完成标记写回
config.json。可以用 `btc_stratum --opencl-self-test` 或
`btc_stratum --cuda-self-test` 在不连接矿池的情况下验证 GPU kernel，或用
`btc_stratum --autotune` 在更换驱动/硬件后重新测试。
```
