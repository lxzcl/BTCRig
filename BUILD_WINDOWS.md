# Windows Build

[简体中文](BUILD_WINDOWS.zh-CN.md)

The full Windows / MSYS2 UCRT64 build and packaging instructions are in [README.md](README.md).

GitHub Actions already builds the Windows UCRT64 package and uploads a zip artifact containing `btc_stratum.exe`, `btc_proxy.exe`, `btc_bench.exe`, `config.json`, and required DLLs.

Next Windows performance priority: x86 SHA-NI / AVX2 multi-lane parallel SHA256d.

Quick build:

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

Default pool:

```text
stratum+tls://public-pool.io:4333
```
