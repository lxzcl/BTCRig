<div align="center">

# BTCRig

**轻量的 SHA256d 基准测试、Stratum V1 客户端与挖矿代理，适合学习、测试和异构算力实验。**

[English](README.md) · [下载版本](https://github.com/lxzcl/BTCRig/releases)

![Release](https://img.shields.io/github/v/release/lxzcl/BTCRig?style=for-the-badge&color=00b894)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20Termux-00b894?style=for-the-badge)
![SHA256d](https://img.shields.io/badge/SHA256d-CPU%20%7C%20OpenCL%20%7C%20CUDA-00b894?style=for-the-badge)

</div>

BTCRig 是一个跨平台 C 项目，提供 SHA256d 基准、Stratum V1 挖矿客户端和 TCP/TLS Stratum 代理，支持 CPU、OpenCL 和 NVIDIA CUDA 后端。它可运行在 Windows、Linux、Android/Termux、x86 PC、ARM 开发板以及带 GPU 的设备上，在通用 CPU/GPU 平台上提供尽可能强的 SHA256d 挖矿表现。

## 程序组成

| 程序 | 用途 |
| --- | --- |
| `btc_stratum` | 支持 CPU/OpenCL/CUDA worker、Stratum V1、TCP/TLS、断线重连和交互统计的客户端 |
| `btc_bench` | 可选择 SHA 后端的本机 SHA256d 基准测试工具 |
| `btc_proxy` | 支持 TCP/TLS 自动识别的多客户端 Stratum 代理 |

## 算力参考

下面是项目开发过程中记录的实际结果，不是严格控制变量的横向评测。编译器、频率限制、散热和后台负载都会明显影响结果。

| 平台 | 环境 | 后端 | 线程 | 记录算力 |
| --- | --- | --- | ---: | ---: |
| NVIDIA GeForce RTX 2050 Laptop GPU | Windows 11 / MSYS2 UCRT64 | CUDA Driver API | 1 GPU | 约 589 MH/s |
| NVIDIA GeForce RTX 2050 Laptop GPU | Windows 11 / MSYS2 UCRT64 | OpenCL modern-unrolled | 1 GPU | 约 536 MH/s |
| AMD 7945HX | Windows 11 | x86-SHA-NI | 32 | 约 600 MH/s |
| 骁龙 8 Elite | Termux | ARMv8 SHA2 | 8 | 约 150 MH/s |
| NanoPi Fire3 | Linux ARM64 | ARMv8 SHA2 | 8 | 约46.4 MH/s |
| NanoPi M3 | Linux ARM64 | ARMv8 SHA2 | 8 | 约46.3 MH/s |
| RockPi-S | Linux ARM64 | ARMv8 SHA2 | 4 | 约8 MH/s |
| Allwinner H3 Series | Linux Cortex-A7 | Openssl | 4 | 约1.2 MH/s |

比较不同构建时，应在同一台机器上运行相同命令：

```bash
./build/btc_bench -t "$(nproc)" -s 10
./build/btc_bench --opencl --opencl-platform 0 --opencl-device 0 -s 10
./build/btc_bench --cuda --cuda-device 0 -s 10
```

## 主要特性

- 自动选择 x86 SHA-NI、ARMv8 SHA2、OpenSSL 或普通 C 后端。
- 可选 OpenCL GPU 路径，包含 compat10 兜底和 OpenCL 1.2+ modern fixed-npi/register-heavy 候选，运行时默认关闭。
- 可选 CUDA GPU 路径，使用 NVIDIA Driver API 和内嵌 PTX；运行时默认关闭，目标机器不需要安装 CUDA Toolkit。
- CPU/GPU 混合 nonce 调度：GPU worker 保持较大的调度 batch，CPU worker 在混合模式下自动使用较小块，降低新 job 到来后的旧任务滞留。
- x86 SHA-NI 双路交错扫描，ARMv8 SHA2 专用 range scan。
- 默认使用全部逻辑 CPU，也可以手动指定线程数。
- 网络断开后持续重连，等待时间最高限制为 60 秒。
- 支持普通 TCP、证书验证 TLS 和兼容模式 TLS。
- 算力单位自动显示，并支持查看每个线程的实时算力。

## 架构概览

| 模块 | 文件 |
| --- | --- |
| SHA256d 后端 | `src/sha256d.c`、`src/sha256d_x86_sha_ni.c`、`src/sha256d_arm_sha2.c` |
| Worker 调度 | `src/miner.c`、`src/miner.h` |
| OpenCL worker | `src/opencl_miner.c`、`src/opencl_miner.h` |
| CUDA worker | `src/cuda_miner.c`、`src/cuda_miner.h`、`src/cuda_sha256d_kernel.cu`、`src/cuda_sha256d_ptx.h` |
| Stratum 客户端 | `src/stratum.c`、`src/stratum.h`、`src/stratum_main.c` |
| 代理 | `src/proxy_main.c` |
| 基准测试 | `src/main.c` |
| 平台辅助 | `src/cpu_info.c`、`src/console.c` |

项目刻意保持在 C11、CMake、OpenSSL、pthreads、Jansson、可选 OpenCL 和动态加载的 CUDA Driver API 这些基础组件上。目标是让代码容易审查，并能在桌面 Linux、Windows/MSYS2 和 Termux 这类环境中保持可移植。

## 快速开始

### 下载版本

Windows 和 Linux x86_64 构建包位于 [Releases 页面](https://github.com/lxzcl/BTCRig/releases)。

Windows 解压 zip、修改 `config.json` 后运行：

```powershell
.\btc_stratum.exe
```

Linux 解压 tarball、修改 `config.json` 后运行：

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

### 从源码构建

Ubuntu / Debian 依赖：

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libssl-dev libjansson-dev git
```

CPU-only 构建：

```bash
git clone https://github.com/lxzcl/BTCRig.git
cd BTCRig
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=OFF -DBTCRIG_OPENCL=OFF -DBTCRIG_CUDA=OFF
cmake --build build -j"$(nproc)"
./build/btc_stratum --self-test
./build/btc_stratum
```

带 OpenCL 支持的构建：

```bash
sudo apt install -y ocl-icd-opencl-dev opencl-headers clinfo
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=OFF -DBTCRIG_OPENCL=ON -DBTCRIG_CUDA=ON
cmake --build build -j"$(nproc)"
./build/btc_stratum --opencl-self-test
./build/btc_stratum --cuda-self-test
./build/btc_stratum --opencl
./build/btc_stratum --cuda
```

`config.json` 默认仍然关闭 OpenCL。`-DBTCRIG_OPENCL=ON` 只表示把 GPU worker 编译进程序；实际运行时需要手动设置 `opencl.enabled=true`，或传入 `--opencl` / `--opencl-all` 才会使用显卡。

`config.json` 默认也关闭 CUDA。`-DBTCRIG_CUDA=ON` 会编译 CUDA worker 和 `btc_bench --cuda`；运行时只需要 NVIDIA 驱动提供 Windows 上的 `nvcuda.dll` 或 Linux 上的 `libcuda.so.1`。只有需要用 `tools/generate_cuda_ptx.sh` 重新生成 `src/cuda_sha256d_ptx.h` 时，才需要 CUDA Toolkit 或能编译 CUDA device code 的 clang。

Termux 应保持 `BTC_MINER_NATIVE=OFF`。ARM SHA2 源文件仍会使用独立的加密扩展参数编译，并通过运行时特性检测选择；关闭全局 native 调优可以避免 Android 异构 CPU 集群上的非法指令。

### Windows / MSYS2 UCRT64 构建

打开 **MSYS2 UCRT64** 终端，运行 `echo $MSYSTEM`，确认输出为 `UCRT64`。然后安装依赖：

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

可选 OpenCL 构建支持：

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-opencl-headers \
  mingw-w64-ucrt-x86_64-opencl-icd
```

CUDA 运行时不需要额外 MSYS2 包。安装 NVIDIA 显卡驱动后，可以这样验证：

```bash
./build/btc_bench.exe --cuda-info
./build/btc_bench.exe --cuda-self-test --cuda-device 0
```

如果 `pacman -Syu` 要求关闭终端，请重新打开 UCRT64 终端，再执行后续命令。构建并测试三个程序：

```bash
git clone https://github.com/lxzcl/BTCRig.git
cd BTCRig
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j"$(nproc)"
./build/btc_stratum.exe --self-test
./build/btc_proxy.exe --version
./build/btc_bench.exe -t 1 -s 1
```

打包 EXE、配置文件及运行所需 DLL：

```bash
rm -rf dist
mkdir -p dist
cp build/btc_stratum.exe build/btc_proxy.exe build/btc_bench.exe config.json proxy.json dist/

ldd build/btc_stratum.exe build/btc_proxy.exe build/btc_bench.exe \
  | awk '{for (i=1; i<=NF; i++) if ($i ~ /^\/ucrt64\/bin\//) print $i}' \
  | sort -u \
  | xargs -r -I{} cp -u "{}" dist/
```

在其他 Windows 电脑上运行时，应复制整个 `dist/` 目录。

## 运行与控制

启动时会显示当前构建可用的 SHA 后端和实际选择的后端：

```text
[SHA] available=x86-sha-ni,openssl,fast-c
[SHA] selected=x86-sha-ni mode=auto
```

常用参数：

```text
-o, --url URL              指定矿池地址
-u, --user USER            指定钱包或用户名
-p, --pass PASS            指定密码
-d, --suggest-diff N       建议初始难度
-t, --threads N            CPU 线程数，0 表示自动
--stats N                  每 N 秒输出统计
--runtime N                运行 N 秒，0 表示不限时
--donate-level N           捐助比例，编译默认 1，最低 1
--no-mine                  只测试连接，不启动挖矿
--no-cpu                   关闭 CPU worker
--opencl                   启用 OpenCL worker，默认使用全部 GPU 设备
--opencl-all               使用全部 OpenCL GPU 设备
--opencl-platform N        OpenCL 平台编号
--opencl-device N          OpenCL 设备编号
--opencl-batch N           每次 OpenCL 调度扫描的 nonce 数
--opencl-local N           OpenCL local work size，0 表示自动
--opencl-npi N             每个 OpenCL work-item 扫描的 nonce 数
--opencl-backend NAME      OpenCL 后端：auto、compat10 或 modern
--opencl-kernel NAME       OpenCL kernel 变体：auto、compact、unrolled、fixed-npi1、fixed-npi2、fixed-npi4 或 register-heavy
--opencl-self-test         不连接矿池，验证编译后的 OpenCL kernel
--cuda                     启用 CUDA worker
--cuda-device N            CUDA 设备编号
--cuda-batch N             每次 CUDA 调度扫描的 nonce 数
--cuda-block N             CUDA threads per block
--cuda-npt N               每个 CUDA thread 扫描的 nonce 数
--cuda-self-test           不连接矿池，验证内嵌 CUDA PTX
--autotune                 强制重新运行首次 CPU/GPU 调优并更新配置
--no-autotune              跳过自动首次调优
--autotune-seconds N       每种模式测试 N 秒，默认 1.5
--cpu-info                 显示 CPU 拓扑
--self-test                运行 Stratum 解析自检
```

挖矿期间可用按键：

| 按键 | 功能 |
| --- | --- |
| `h` | 显示每个线程的算力 |
| `p` | 暂停挖矿 |
| `r` | 恢复挖矿 |
| `s` | 显示 share 统计 |
| `c` | 显示连接状态 |

## 配置文件

`btc_stratum` 默认读取当前目录的 `config.json`。请先把示例钱包替换成自己的地址。

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
  "cuda": {
    "enabled": false,
    "device": 0,
    "batch-size": 4194304,
    "threads-per-block": 256,
    "nonces-per-thread": 1,
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

`diff` 只是初始建议值，实际 share 难度由矿池通过 `mining.set_difficulty` 下发。

OpenCL 和 CUDA 都是运行时可选模块。构建机器如果有 OpenCL 头文件和库，`btc_stratum` 默认会包含 OpenCL worker；如果没有，则跳过这条路径。CUDA 默认通过运行时驱动加载编译进程序，但只有设置 `cuda.enabled=true` 或传入 `--cuda` 才会启用。开启任意 GPU 后端但找不到可用设备时，程序会输出警告，并继续保留 CPU 路径。启用 OpenCL 且没有配置具体设备列表时，会默认使用全部 OpenCL GPU 设备；CUDA 当前使用一张指定的 NVIDIA 设备。

CPU、OpenCL 和 CUDA worker 共用一个 nonce 分配器，因此不会扫描重叠 nonce。纯 CPU 模式下 CPU worker 使用较大的 nonce 块；只要存在 GPU worker，CPU worker 会自动切换为较小块，而 GPU worker 继续使用配置中的 `batch-size`。新 job、暂停/恢复和停止会直接唤醒等待中的 worker，不再只依赖周期性轮询。

默认配置中 `autotune.enabled=true`、`autotune.cpu-self-test=false` 且 `autotune.gpu-self-test=false`。第一次正常挖矿启动时，程序会先离线运行 self-test 和基准测试，再连接矿池。CPU 和 GPU 的完成标记会分开记录：只运行 CPU 时只会把 `cpu-self-test` 改成 `true`，之后再启用 OpenCL 或 CUDA 仍会触发 GPU 调优，并保留已有 CPU 结果。只有当 `config.json` 中 `opencl.enabled=true`、`cuda.enabled=true`，或者命令行传入对应 GPU 参数时，自动调优才会测试 GPU 模式；默认 GPU 后端关闭时只测试 CPU，并保持 GPU 后端关闭。如果 OpenCL 已启用，它会先对每张 OpenCL GPU 分阶段测试 `backend`、`kernel`、`local-work-size`、`nonces-per-work-item` 和 `batch-size`，然后测试 CPU-only、全部 GPU、CPU+全部 GPU、半数 CPU+全部 GPU、每张单独 GPU、CPU+每张单独 GPU；如果机器有超过两张 GPU，还会测试“全部 GPU 去掉其中一张”的组合。如果 CUDA 已启用，会先预热选中的 NVIDIA 设备，调优 `threads-per-block`、`nonces-per-thread` 和 `batch-size`，再测试 CUDA-only、CPU+CUDA 和半数 CPU+CUDA。测试结束后会把最快模式和每种模式的算力写回 `config.json`。旧版 `autotune.self-test`、`self_test`、`done` 和 `completed` 字段仍会作为 CPU 完成标记兼容读取。

这里故意不穷举所有 CPU/GPU 子集。高价值组合已经能覆盖常见情况：独显加核显、CPU 线程和 GPU 驱动抢资源、某张慢卡或不稳定卡拖累整体。换驱动、改频率、换硬件，或调整 OpenCL batch/local/npi、CUDA batch/block/npt 后，可以用 `--autotune` 重新测试。

## SHA 后端

| 后端 | 使用条件 | 说明 |
| --- | --- | --- |
| `x86-sha-ni` | 支持 SHA 扩展的 x86 CPU | 首选 x86 路径，双路交错 nonce 扫描 |
| `arm-sha2` | 支持 SHA2 扩展的 ARMv8 CPU | ARM 专用 range scan |
| `openssl` | 所有支持的平台 | 通用库回退路径 |
| `fast-c` | 所有支持的平台 | 普通 C 回退路径 |
| `opencl` | 可选 `btc_stratum` worker | OpenCL GPU worker，包含 `compat10` 兜底和 `modern` OpenCL 1.2+ fixed-npi/register-heavy 候选路径 |
| `cuda` | 可选 `btc_stratum` worker 和 `btc_bench --cuda` | 通过运行时 CUDA Driver API 加载 NVIDIA GPU 后端，并使用内嵌 PTX |

可以用 `BTC_MINER_SHA_BACKEND` 覆盖自动选择，例如：

```bash
BTC_MINER_SHA_BACKEND=openssl ./build/btc_bench -t "$(nproc)" -s 10
```

OpenCL 可以通过 `config.json` 或命令行启用：

```bash
./build/btc_stratum --opencl
./build/btc_stratum --no-cpu --opencl --opencl-platform 0 --opencl-device 0
./build/btc_stratum --opencl-self-test --opencl-platform 0 --opencl-device 0
```

CUDA 可以通过 `config.json` 或命令行启用：

```bash
./build/btc_bench --cuda-info
./build/btc_bench --cuda --cuda-device 0 -s 10
./build/btc_stratum --no-cpu --cuda --cuda-device 0
./build/btc_stratum --cuda-self-test --cuda-device 0
```

也可以显式指定多张 OpenCL GPU：

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

每个 OpenCL 设备和选中的 CUDA 设备都会在开始挖矿前先运行 self-test。某个设备自检失败时会被跳过，其他可用 CPU/GPU worker 会继续运行。

`backend=compat10` 路径避免使用 OpenCL 2.x API，主机端只使用 OpenCL 1.0 API。OpenCL 1.0 设备需要 `cl_khr_global_int32_base_atomics`，OpenCL 1.1+ 设备可以使用核心 global int32 atomic。`backend=modern` 是 OpenCL 1.2+ 候选路径；`backend=auto` 会在设备同时支持 compat10 和 modern 时交给 autotune 对比选择。`kernel=unrolled` 是偏高吞吐的 SHA256d 路径，`kernel=compact` 使用更小的循环压缩器，适合较老驱动或寄存器容量较紧的设备。`kernel=fixed-npi1`、`fixed-npi2` 和 `fixed-npi4` 是 modern-only kernel，会把每个 work-item 扫描的 nonce 数固定为 1、2 或 4。`kernel=register-heavy` 是 modern-only 的双 nonce 向量寄存器候选，npi 固定为 2。autotune 只会在 `backend=modern` 下测试 modern-only kernel；`kernel=auto` 会测试 compact/unrolled 和 modern 候选变体，然后保留最快且稳定的结果。

## 文档

- [代理说明](PROXY.zh-CN.md)
- [English README](README.md)
- [版本下载](https://github.com/lxzcl/BTCRig/releases)

## 负责任使用

- 挖矿和基准测试会让 CPU/GPU 长时间处于高负载，请注意散热、供电和电池状态。
- Release 构建不会隐藏后台服务；请从终端运行二进制，并先检查 `config.json`。
- OpenCL 和 CUDA 设备会在开始挖矿前运行 self-test；失败的设备会被跳过，其他 CPU/GPU worker 可以继续运行。
- GitHub Actions 只构建发布包，刻意不在托管 runner 上运行挖矿基准测试。

## 捐赠
软件默认包含 1% 开发者捐赠（约每 100 分钟捐赠 1 分钟），对所有挖矿模式生效。目前无法在代码层面中自动区分 PPLNS 矿池与 Solo 模式，矿池地址默认使用PPLNS，若使用 Solo 模式，请注意：存在极小概率在捐赠时段内找到区块，导致整个区块奖励被捐赠。如需修改捐赠比例，请编辑源码中的捐赠参数并重新编译。
BTC：bc1qqz0wutk9kk5mmaf7fu4dm5w4fq4fhaah9hpzr3

## 许可证

BTCRig 使用 [GNU General Public License v3.0](LICENSE)。
