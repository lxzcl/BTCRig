# Windows 构建

完整 Windows / MSYS2 UCRT64 构建和打包说明已经合并到 `README.md`。

快速命令：

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-jansson \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-ninja \
  git make

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j$(nproc)
```

默认矿池：

```text
stratum+tls://public-pool.io:4333
```
