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
  git make

# 可选 OpenCL 构建支持。安装后 CMake 会默认编译 compat10 GPU 后端。
pacman -S --needed \
  mingw-w64-ucrt-x86_64-opencl-headers \
  mingw-w64-ucrt-x86_64-opencl-icd

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j$(nproc)
```

默认矿池：

```text
stratum+tls://public-pool.io:4333
```

OpenCL 说明：

```text
OpenCL 支持是构建时可选模块。如果要让 CMake 编译 compat10 OpenCL worker，
需要安装带 OpenCL runtime 的显卡驱动和 OpenCL 开发头文件。找不到 OpenCL 时，
CPU-only 构建不受影响。第一次正常挖矿启动时，`autotune.enabled=true` 会让
`btc_stratum` 离线自检 CPU/GPU 模式，把最快模式写回 config.json，并把
`autotune.self-test` 设为 true。可以用 `btc_stratum --opencl-self-test` 在不
连接矿池的情况下验证 kernel，或用 `btc_stratum --autotune` 在更换驱动/硬件后
重新测试。
```
