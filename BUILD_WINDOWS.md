# Windows Build

[简体中文](BUILD_WINDOWS.zh-CN.md)

The full Windows / MSYS2 UCRT64 build and packaging instructions are in [README.md](README.md).

GitHub Actions already builds the Windows UCRT64 package. `dev` builds keep the zip as an Actions artifact, while `master` builds publish the zip directly to GitHub Releases.

Next Windows performance priority: x86 SHA-NI / AVX2 multi-lane parallel SHA256d.

Quick build:

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-jansson \
  mingw-w64-ucrt-x86_64-pkgconf \
  git make

# Optional OpenCL build support.
pacman -S --needed \
  mingw-w64-ucrt-x86_64-opencl-headers \
  mingw-w64-ucrt-x86_64-opencl-icd

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j$(nproc)
```

Default pool:

```text
stratum+tls://public-pool.io:4333
```

OpenCL note:

```text
OpenCL support is optional and disabled in config.json by default. Install a GPU
driver with OpenCL runtime and OpenCL development headers if you want CMake to
compile the experimental OpenCL worker. CPU-only builds are unaffected when
OpenCL is not found.
```
