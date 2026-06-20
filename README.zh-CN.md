# BTCRig

[English](README.md)

BTCRig 是一个用于学习、测试和实验的 BTC SHA256d CPU 矿工与 Stratum 代理项目。

项目包含三个程序：

- `btc_stratum`：连接 Stratum 矿池，并使用 CPU 计算 SHA256d。
- `btc_bench`：测试本机 SHA256d 算力。
- `btc_proxy`：在矿工和上游矿池之间转发 Stratum 流量，支持 TCP 和 TLS。

默认配置：

```text
pool: stratum+tls://public-pool.io:4333
user: bc1qqz0wutk9kk5mmaf7fu4dm5w4fq4fhaah9hpzr3
pass: x
suggest difficulty: 0.001
agent: BTCRig/v0.2.0
```

如果要使用自己的钱包，请运行时加 `-u`，或者修改 `config.json`。

## Ubuntu / Debian 自动安装

```bash
wget -O ubuntu.sh https://raw.githubusercontent.com/lxzcl/BTCRig/master/ubuntu.sh
chmod +x ubuntu.sh
./ubuntu.sh
```

脚本会安装依赖、下载源码、构建并运行 `btc_stratum`。默认安装目录：

```text
~/BTCRig
```

再次运行：

```bash
cd ~/BTCRig
./build/btc_stratum
```

## Ubuntu / Debian 手动构建

```bash
sudo apt update
sudo apt install -y build-essential cmake make pkg-config git \
  libssl-dev libjansson-dev ca-certificates wget unzip

git clone https://github.com/lxzcl/BTCRig.git
cd BTCRig
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j"$(nproc)"
```

测试：

```bash
./build/btc_stratum --self-test
./build/btc_stratum --cpu-info
./build/btc_bench -t "$(nproc)" -s 10
```

运行：

```bash
./build/btc_stratum
```

指定矿池或钱包：

```bash
./build/btc_stratum -o stratum+tls://public-pool.io:4333
./build/btc_stratum -u bc1q_example_wallet.worker
```

## Termux 自动安装

```bash
pkg update
pkg install -y wget
wget -O termux.sh https://raw.githubusercontent.com/lxzcl/BTCRig/master/termux.sh
chmod +x termux.sh
./termux.sh
```

如果 Termux 的 `cmake` 因 `jsoncpp` 动态库不匹配而无法启动，脚本会先尝试修复；如果仍然失败，会自动改用 `clang` 直接构建。

手动修复命令：

```bash
pkg update
pkg upgrade -y
pkg reinstall -y cmake jsoncpp
```

## Termux 手动构建

```bash
pkg update
pkg install -y clang make cmake jsoncpp git openssl openssl-tool pkg-config libjansson wget unzip

git clone https://github.com/lxzcl/BTCRig.git
cd BTCRig
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j"$(nproc)"
./build/btc_stratum
```

## Windows / MSYS2 UCRT64 构建

请打开 MSYS2 UCRT64 终端，不要使用普通 MSYS、MINGW64 或 Windows PowerShell。

确认环境：

```bash
echo $MSYSTEM
```

应输出：

```text
UCRT64
```

安装依赖：

```bash
pacman -Syu
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-jansson \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-ninja \
  git make
```

构建：

```bash
git clone https://github.com/lxzcl/BTCRig.git
cd BTCRig
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j$(nproc)
```

生成文件：

```text
build/btc_stratum.exe
build/btc_proxy.exe
build/btc_bench.exe
```

测试：

```bash
./build/btc_stratum.exe --self-test
./build/btc_stratum.exe --cpu-info
./build/btc_bench.exe -t 1 -s 1
```

## Windows 打包 DLL

只打包矿工主程序：

```bash
rm -rf dist
mkdir -p dist
cp build/btc_stratum.exe config.json dist/

ldd build/btc_stratum.exe \
  | awk '{for (i=1; i<=NF; i++) if ($i ~ /^\/ucrt64\/bin\//) print $i}' \
  | sort -u \
  | xargs -r -I{} cp -u "{}" dist/
```

打包全部程序：

```bash
rm -rf dist
mkdir -p dist
cp build/btc_stratum.exe build/btc_proxy.exe build/btc_bench.exe config.json proxy.json dist/

ldd build/btc_stratum.exe build/btc_proxy.exe build/btc_bench.exe \
  | awk '{for (i=1; i<=NF; i++) if ($i ~ /^\/ucrt64\/bin\//) print $i}' \
  | sort -u \
  | xargs -r -I{} cp -u "{}" dist/
```

把整个 `dist/` 目录复制到其他 Windows 机器即可。

## GitHub Actions Windows 构建

仓库包含 Windows UCRT64 workflow：

```text
.github/workflows/main.yml
```

它有两种触发方式：

- 在 GitHub Actions 页面手动运行 `workflow_dispatch`。
- `master` 或 `dev` 上涉及源码、CMake、配置、版本号或 workflow 文件的代码变动推送。

workflow 会构建三个程序，并生成 zip 产物：

```text
BTCRig-v0.2.0-windows-ucrt64.zip
```

zip 内包含：

```text
btc_stratum.exe
btc_proxy.exe
btc_bench.exe
config.json
proxy.json
必要 DLL 文件
README 文件
LICENSE
VERSION
```

Release 行为：

- 推送到 `dev` 时会构建，并把 zip 保存在 GitHub Actions artifact。
- 推送到 `master` 时会构建，并把 zip 直接发布到 [GitHub Releases](https://github.com/lxzcl/BTCRig/releases)。
- 手动运行 workflow 时，也可以通过 `publish_release` 选项发布到 Releases。

## 版本控制

BTCRig 使用根目录 `VERSION` 文件作为唯一版本来源。当前开发版本：

```text
0.2.0
```

CMake 会读取这个文件并生成运行时版本宏。矿工上报的 Stratum user agent 是：

```text
BTCRig/v0.2.0
```

常用命令：

```bash
./build/btc_stratum --version
./build/btc_proxy --version
./build/btc_bench --version
```

建议发布流程：

```bash
git switch dev
# 开发和测试
git switch master
git merge --ff-only dev
git tag -a v0.2.0 -m "BTCRig v0.2.0"
git push origin master v0.2.0
```

## 常用参数

```text
-o, --url URL              指定矿池地址
-u, --user USER            指定钱包或用户名
-p, --pass PASS            指定密码
-d, --suggest-diff N       建议初始难度
-t, --threads N            指定 CPU 线程数，0 表示自动
--stats N                  每 N 秒输出统计
--runtime N                运行 N 秒，0 表示不限时
--donate-level N           捐助比例，默认 1，设为 0 可关闭
--no-mine                  只测试连接，不启动挖矿
--cpu-info                 显示 CPU 信息
--self-test                运行 Stratum 解析自测
```

网络断开后程序会持续重连，等待时间从 `retry-pause` 开始递增，最多 60 秒一次。

## 开发者捐助

BTCRig 默认启用透明的 1% 开发者捐助，调度方式参考 XMRig。默认情况下，程序使用配置的钱包挖矿 99 分钟，然后为开发者地址挖矿 1 分钟。第一次捐助会在累计 49.5 至 148.5 分钟的有效用户挖矿时间后随机触发，避免大量客户端同时切换。

捐助会话沿用当前用户矿池的地址、密码和建议难度，只替换钱包地址。只有连接完成授权并收到有效任务后才开始计算捐助时间。如果捐助连接失败，程序会立即返回用户挖矿。控制台会明确显示当前模式、地址、矿池和调度时间。可以在配置文件中设置 `"donate-level": 0`，或运行时使用 `--donate-level 0` 关闭。

## 交互按键

运行期间可以按：

```text
h  显示各线程算力
p  暂停挖矿
r  恢复挖矿
s  显示提交统计
c  显示连接信息
```

## Stratum 代理

默认运行：

```bash
./build/btc_proxy
```

默认监听：

```text
listen: 0.0.0.0:4333
mode: auto
upstream: stratum+tls://public-pool.io:4333
```

`auto` 模式会在同一个端口上自动识别客户端是 TCP 还是 TLS。每个客户端连接都会建立一个独立的上游矿池连接。

代理没有配置证书时，会在当前工作目录自动生成自签名证书：

```text
cert.pem
cert_key.pem
```

更多说明见 [PROXY.zh-CN.md](PROXY.zh-CN.md)。

## 难度说明

BTCRig 默认发送：

```text
mining.suggest_difficulty = 0.001
```

这是建议值，不是强制值。实际提交难度以矿池下发的 `mining.set_difficulty` 为准，并从下一次 `mining.notify` 开始生效。

## SHA 后端

默认使用自动选择：

- 普通 C 实现
- OpenSSL SHA256
- ARMv8 SHA2 专用路径，编译器和 CPU 支持时启用
- x86 SHA-NI 专用路径，编译器和 CPU 支持时启用

可以用环境变量指定：

```bash
BTC_MINER_SHA_BACKEND=auto ./build/btc_bench -t "$(nproc)" -s 10
BTC_MINER_SHA_BACKEND=openssl ./build/btc_bench -t "$(nproc)" -s 10
BTC_MINER_SHA_BACKEND=fast-c ./build/btc_bench -t "$(nproc)" -s 10
BTC_MINER_SHA_BACKEND=x86-sha-ni ./build/btc_bench -t "$(nproc)" -s 10
BTC_MINER_SHA_BACKEND=arm-sha2 ./build/btc_bench -t "$(nproc)" -s 10
```

在 x86 CPU 上，`auto` 会优先选择可用的 `x86-sha-ni`，否则回退到 OpenSSL。
当前 x86 后端已经使用 SHA-NI 的 SHA256 轮函数和消息调度指令。
AVX2 多路 nonce 批处理属于下一阶段优化。

## 配置文件

默认会读取当前目录的 `config.json`。最小示例：

```json
{
  "cpu": {
    "enabled": true,
    "threads": 0
  },
  "pools": [
    {
      "url": "stratum+tls://public-pool.io:4333",
      "user": "bc1qqz0wutk9kk5mmaf7fu4dm5w4fq4fhaah9hpzr3",
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

## 已完成工作

- CPU SHA256d Stratum 矿工，支持 TCP 和 TLS 矿池。
- 配置文件支持和默认配置。
- 无限重连，并限制最大重试等待时间。
- 运行时交互按键：算力、暂停、恢复、提交统计、连接信息。
- Benchmark 工具，支持选择 SHA 后端。
- 透明 Stratum 代理，支持自动识别 TCP/TLS 客户端。
- 代理自动生成自签名证书。
- Windows、Ubuntu/Debian、Termux 构建文档。
- 英文默认文档和中文翻译文档。
- 从 `v0.1.0` 开始的统一 `VERSION` 版本控制。
- GitHub Actions Windows UCRT64 构建和 zip 产物打包。
- GitHub Actions 在 `master` 和手动发布运行中自动把 Windows zip 推送到 Releases。
- x86 SHA-NI SHA256d 后端，并保留 OpenSSL 回退。
- 透明的 XMRig 风格定时开发者捐助，默认比例为 1%。

## 后续计划

- 在 SHA-NI 后端之上继续做 x86 AVX2 多路 nonce 批处理。
- Linux 和 Termux 的 GitHub Actions 构建。
- 使用 Git tag 的版本化 GitHub Release。
- 增加 benchmark 模式，方便比较 OpenSSL、普通 C、ARM SHA2、x86 SHA-NI 和未来 AVX2 批处理。
- 更完整的 Windows 和 Linux 发布包。
- 增强重复 share、重连、矿池难度变化等挖矿稳定性诊断。
