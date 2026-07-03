# Windows 构建

[English](BUILD_WINDOWS.md)

完整 Windows / MSYS2 UCRT64 构建和打包说明见 [README.zh-CN.md](README.zh-CN.md)。

GitHub Actions 已经支持 Windows UCRT64 构建。`dev` 构建会把 zip 保存在 Actions artifact，`master` 构建会把 zip 直接发布到 GitHub Releases。

Windows 构建可以包含 CPU、OpenCL 和 CUDA 后端。CUDA 通过运行时 NVIDIA 驱动加载，目标机器不需要安装 CUDA Toolkit。

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
./build/btc_bench.exe --cuda-info
./build/btc_bench.exe --cuda-self-test --cuda-device 0
```

默认矿池：

```text
stratum+tls://public-pool.io:14333
```

OpenCL 说明：

```text
OpenCL 支持是构建时可选模块。如果要让 CMake 编译 compat10 OpenCL worker，
需要安装带 OpenCL runtime 的显卡驱动和 OpenCL 开发头文件。找不到 OpenCL 时，
CPU-only 构建不受影响。CUDA 通过运行时驱动加载编译进程序，只有设置
`cuda.enabled=true` 或传入 `--cuda` 才会启用；运行时需要 NVIDIA 显卡驱动，
不需要 CUDA Toolkit。第一次正常挖矿启动时，`autotune.enabled=true` 会让
`btc_stratum` 离线自检 CPU/GPU 模式，把最快模式和 CPU/GPU 调优完成标记写回
config.json。可以用 `btc_stratum --opencl-self-test` 或
`btc_stratum --cuda-self-test` 在不连接矿池的情况下验证 GPU kernel，或用
`btc_stratum --autotune` 在更换驱动/硬件后重新测试。
```
