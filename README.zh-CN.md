<div align="center">

# BTCRig

**轻量的 Bitcoin SHA256d CPU 矿工、基准测试与 Stratum 代理。**

[English](README.md) · [下载版本](https://github.com/lxzcl/BTCRig/releases)

![Release](https://img.shields.io/github/v/release/lxzcl/BTCRig?style=for-the-badge&color=00b894)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20Termux-00b894?style=for-the-badge)
![SHA256d](https://img.shields.io/badge/SHA256d-CPU-00b894?style=for-the-badge)
![Donation](https://img.shields.io/badge/default%20donation-0%25-00b894?style=for-the-badge)

</div>

BTCRig 将 Windows、Linux、Android/Termux、x86 PC 和 ARM 开发板上的闲置 CPU 资源转化为算力。

## 程序组成

| 程序 | 用途 |
| --- | --- |
| `btc_stratum` | 支持 Stratum V1、TCP/TLS、断线重连和交互统计的 CPU 矿工 |
| `btc_bench` | 可选择 SHA 后端的本机 SHA256d 基准测试工具 |
| `btc_proxy` | 支持 TCP/TLS 自动识别的多客户端 Stratum 代理 |

## 主要特性

- 自动选择 x86 SHA-NI、ARMv8 SHA2、OpenSSL 或普通 C 后端。
- 面向异构闲置设备，让原本未利用的 CPU 算力重新发挥作用。
- x86 SHA-NI 双路交错扫描，ARMv8 SHA2 专用 range scan。
- 默认使用全部逻辑 CPU，也可以手动指定线程数。
- 网络断开后持续重连，等待时间最高限制为 60 秒。
- 支持普通 TCP、证书验证 TLS 和兼容模式 TLS。
- 算力单位自动显示，并支持查看每个线程的实时算力。
- 保留可选捐助调度，但默认值为 **`0%`**；关闭时不会进入哈希工作线程。

## 快速开始

### 下载版本

Windows 构建包位于 [Releases 页面](https://github.com/lxzcl/BTCRig/releases)。解压 zip、修改 `config.json` 后运行：

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

### 从源码构建

```bash
git clone https://github.com/lxzcl/BTCRig.git
cd BTCRig
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j"$(nproc)"
./build/btc_stratum --self-test
./build/btc_stratum
```

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

## 算力参考

下面是项目开发过程中记录的实际结果，不是严格控制变量的横向评测。编译器、频率限制、散热和后台负载都会明显影响结果。

| 平台 | 环境 | 后端 | 线程 | 记录算力 |
| --- | --- | --- | ---: | ---: |
| 骁龙 8 Elite | Termux | ARMv8 SHA2 | 8 | 约 150 MH/s |
| NanoPi Fire3 | Linux ARM64 | ARMv8 SHA2 | 8 | 约 34.4 MH/s |
| NanoPi M3 | Linux ARM64 | ARMv8 SHA2 | 8 | 约 12.3 MH/s |
| RockPi-S | Linux ARM64 | ARMv8 SHA2 | 4 | 约 6 MH/s |

比较不同构建时，应在同一台机器上运行相同命令：

```bash
./build/btc_bench -t "$(nproc)" -s 10
```

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
--donate-level N           捐助比例，默认 0
--no-mine                  只测试连接，不启动挖矿
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
  "cpu": {
    "enabled": true,
    "threads": 0
  },
  "pools": [
    {
      "url": "stratum+tls://public-pool.io:4333",
      "user": "bc1q_example_wallet.worker",
      "pass": "x",
      "diff": 0.001
    }
  ],
  "retries": -1,
  "retry-pause": 2,
  "donate-level": 0,
  "print-time": 10,
  "runtime": 0
}
```

`diff` 只是初始建议值，实际 share 难度由矿池通过 `mining.set_difficulty` 下发。

## SHA 后端

| 后端 | 使用条件 | 说明 |
| --- | --- | --- |
| `x86-sha-ni` | 支持 SHA 扩展的 x86 CPU | 首选 x86 路径，双路交错 nonce 扫描 |
| `arm-sha2` | 支持 SHA2 扩展的 ARMv8 CPU | ARM 专用 range scan |
| `openssl` | 所有支持的平台 | 通用库回退路径 |
| `fast-c` | 所有支持的平台 | 普通 C 回退路径 |

可以用 `BTC_MINER_SHA_BACKEND` 覆盖自动选择，例如：

```bash
BTC_MINER_SHA_BACKEND=openssl ./build/btc_bench -t "$(nproc)" -s 10
```

## 文档

- [代理说明](PROXY.zh-CN.md)
- [English README](README.md)
- [版本下载](https://github.com/lxzcl/BTCRig/releases)

## 许可证

BTCRig 使用 [GNU General Public License v3.0](LICENSE)。
