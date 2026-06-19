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
agent: BTCRig/1.0
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

## 常用参数

```text
-o, --url URL              指定矿池地址
-u, --user USER            指定钱包或用户名
-p, --pass PASS            指定密码
-d, --suggest-diff N       建议初始难度
-t, --threads N            指定 CPU 线程数，0 表示自动
--stats N                  每 N 秒输出统计
--runtime N                运行 N 秒，0 表示不限时
--no-mine                  只测试连接，不启动挖矿
--cpu-info                 显示 CPU 信息
--self-test                运行 Stratum 解析自测
```

网络断开后程序会持续重连，等待时间从 `retry-pause` 开始递增，最多 60 秒一次。

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

可以用环境变量指定：

```bash
BTC_MINER_SHA_BACKEND=auto ./build/btc_bench -t "$(nproc)" -s 10
BTC_MINER_SHA_BACKEND=openssl ./build/btc_bench -t "$(nproc)" -s 10
BTC_MINER_SHA_BACKEND=portable ./build/btc_bench -t "$(nproc)" -s 10
```

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
  "print-time": 10,
  "runtime": 0
}
```
