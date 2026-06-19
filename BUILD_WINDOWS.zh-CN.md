# Windows 构建

[English](BUILD_WINDOWS.md)

完整 Windows / MSYS2 UCRT64 构建和打包说明见 [README.zh-CN.md](README.zh-CN.md)。

GitHub Actions 已经支持 Windows UCRT64 构建。`dev` 构建会把 zip 保存在 Actions artifact，`master` 构建会把 zip 直接发布到 GitHub Releases。

Windows 性能优化最先提上日程的是 x86 SHA-NI / AVX2 多路并行 SHA256d。

快速构建：

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
