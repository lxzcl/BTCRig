# Linux / Termux 构建

完整 Ubuntu、Debian 和 Termux 构建说明已经合并到 `README.md`。

Ubuntu / Debian 快速命令：

```bash
sudo apt update
sudo apt install -y build-essential cmake make pkg-config git \
  libssl-dev libjansson-dev ca-certificates wget unzip

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j"$(nproc)"
```

Termux 快速命令：

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
