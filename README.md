# BTCRig

[简体中文](README.zh-CN.md)

BTCRig is an experimental Bitcoin SHA256d CPU miner and Stratum proxy for learning, testing, and low-power mining experiments.

The project builds three programs:

- `btc_stratum`: connects to a Stratum pool and mines with the CPU.
- `btc_bench`: benchmarks local SHA256d performance.
- `btc_proxy`: forwards Stratum traffic between miners and an upstream pool, with TCP and TLS support.

Default configuration:

```text
pool: stratum+tls://public-pool.io:4333
user: bc1qqz0wutk9kk5mmaf7fu4dm5w4fq4fhaah9hpzr3
pass: x
suggest difficulty: 0.001
agent: BTCRig/v0.2.0
```

Use `-u` or edit `config.json` to mine with your own wallet.

## Ubuntu / Debian Auto Install

```bash
wget -O ubuntu.sh https://raw.githubusercontent.com/lxzcl/BTCRig/master/ubuntu.sh
chmod +x ubuntu.sh
./ubuntu.sh
```

The script installs dependencies, downloads the source, builds BTCRig, and starts `btc_stratum`.

Default install directory:

```text
~/BTCRig
```

Run again later:

```bash
cd ~/BTCRig
./build/btc_stratum
```

## Ubuntu / Debian Manual Build

```bash
sudo apt update
sudo apt install -y build-essential cmake make pkg-config git \
  libssl-dev libjansson-dev ca-certificates wget unzip

git clone https://github.com/lxzcl/BTCRig.git
cd BTCRig
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j"$(nproc)"
```

Basic checks:

```bash
./build/btc_stratum --self-test
./build/btc_stratum --cpu-info
./build/btc_bench -t "$(nproc)" -s 10
```

Run:

```bash
./build/btc_stratum
```

Override pool or wallet:

```bash
./build/btc_stratum -o stratum+tls://public-pool.io:4333
./build/btc_stratum -u bc1q_example_wallet.worker
```

## Termux Auto Install

```bash
pkg update
pkg install -y wget
wget -O termux.sh https://raw.githubusercontent.com/lxzcl/BTCRig/master/termux.sh
chmod +x termux.sh
./termux.sh
```

If Termux `cmake` cannot start because of a `jsoncpp` library mismatch, the script tries to repair it first. If that still fails, it falls back to a direct `clang` build.

Manual repair:

```bash
pkg update
pkg upgrade -y
pkg reinstall -y cmake jsoncpp
```

## Termux Manual Build

```bash
pkg update
pkg install -y clang make cmake jsoncpp git openssl openssl-tool pkg-config libjansson wget unzip

git clone https://github.com/lxzcl/BTCRig.git
cd BTCRig
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j"$(nproc)"
./build/btc_stratum
```

## Windows / MSYS2 UCRT64 Build

Open the MSYS2 UCRT64 terminal. Do not use the plain MSYS, MINGW64, or Windows PowerShell environment for this build.

Check the environment:

```bash
echo $MSYSTEM
```

Expected output:

```text
UCRT64
```

Install dependencies:

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

Build:

```bash
git clone https://github.com/lxzcl/BTCRig.git
cd BTCRig
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j$(nproc)
```

Generated files:

```text
build/btc_stratum.exe
build/btc_proxy.exe
build/btc_bench.exe
```

Test:

```bash
./build/btc_stratum.exe --self-test
./build/btc_stratum.exe --cpu-info
./build/btc_bench.exe -t 1 -s 1
```

## Windows DLL Packaging

Package only the miner:

```bash
rm -rf dist
mkdir -p dist
cp build/btc_stratum.exe config.json dist/

ldd build/btc_stratum.exe \
  | awk '{for (i=1; i<=NF; i++) if ($i ~ /^\/ucrt64\/bin\//) print $i}' \
  | sort -u \
  | xargs -r -I{} cp -u "{}" dist/
```

Package all executables:

```bash
rm -rf dist
mkdir -p dist
cp build/btc_stratum.exe build/btc_proxy.exe build/btc_bench.exe config.json proxy.json dist/

ldd build/btc_stratum.exe build/btc_proxy.exe build/btc_bench.exe \
  | awk '{for (i=1; i<=NF; i++) if ($i ~ /^\/ucrt64\/bin\//) print $i}' \
  | sort -u \
  | xargs -r -I{} cp -u "{}" dist/
```

Copy the whole `dist/` directory to another Windows machine.

## GitHub Actions Windows Build

The repository includes a Windows UCRT64 workflow at `.github/workflows/main.yml`.

It runs in two cases:

- Manual run through GitHub Actions `workflow_dispatch`.
- Code changes pushed to `master` or `dev` that touch source, CMake, config, version, or the workflow file.

The workflow builds all three programs and creates this zip package:

```text
BTCRig-v0.2.0-windows-ucrt64.zip
```

The zip contains:

```text
btc_stratum.exe
btc_proxy.exe
btc_bench.exe
config.json
proxy.json
required DLL files
README files
LICENSE
VERSION
```

Release behavior:

- Pushes to `dev` build and keep the package as a GitHub Actions artifact.
- Pushes to `master` build and publish the zip directly to [GitHub Releases](https://github.com/lxzcl/BTCRig/releases).
- Manual workflow runs can also publish to Releases through the `publish_release` input.

## Versioning

BTCRig uses a single `VERSION` file as the project version source. The current development version is:

```text
0.2.0
```

CMake reads this file and generates the runtime version macros. The miner reports the Stratum user agent as:

```text
BTCRig/v0.2.0
```

Useful commands:

```bash
./build/btc_stratum --version
./build/btc_proxy --version
./build/btc_bench --version
```

Recommended release flow:

```bash
git switch dev
# develop and test
git switch master
git merge --ff-only dev
git tag -a v0.2.0 -m "BTCRig v0.2.0"
git push origin master v0.2.0
```

## Common Options

```text
-o, --url URL              pool URL
-u, --user USER            wallet or username
-p, --pass PASS            password
-d, --suggest-diff N       suggested initial difficulty
-t, --threads N            CPU thread count, 0 means auto
--stats N                  print stats every N seconds
--runtime N                run for N seconds, 0 means unlimited
--donate-level N           donation percentage, default 1, 0 disables it
--no-mine                  test the connection without mining
--cpu-info                 print CPU information
--self-test                run Stratum parser self-test
```

Network reconnects are continuous. The reconnect delay starts at `retry-pause` and backs off up to 60 seconds.

## Developer Donation

BTCRig defaults to a transparent 1% developer donation, following XMRig's time-based model. For the default level, the miner works for the configured user for 99 minutes and then mines for the developer address for 1 minute. The first donation is randomized between 49.5 and 148.5 minutes of active user mining so clients do not all switch at once.

The donation session uses the currently selected user pool, including its URL, password, and suggested difficulty; only the wallet address changes. Donation time starts only after the connection is authorized and has received a job. If that connection fails, BTCRig immediately returns to user mining. The current mode, address, pool, and schedule are printed in the console. Set `"donate-level": 0` or use `--donate-level 0` to disable it.

## Interactive Keys

During mining:

```text
h  per-thread hashrate
p  pause mining
r  resume mining
s  submit statistics
c  connection information
```

## Stratum Proxy

Run:

```bash
./build/btc_proxy
```

Default listener:

```text
listen: 0.0.0.0:4333
mode: auto
upstream: stratum+tls://public-pool.io:4333
```

`auto` mode detects plain TCP and TLS clients on the same port. Each client gets an independent upstream pool connection.

If no certificate is configured, the proxy generates a self-signed certificate in the current working directory:

```text
cert.pem
cert_key.pem
```

See [PROXY.md](PROXY.md) for details.

## Difficulty

BTCRig sends this suggested difficulty by default:

```text
mining.suggest_difficulty = 0.001
```

This is only a suggestion. The actual share difficulty is controlled by the pool through `mining.set_difficulty`, and it takes effect from the next `mining.notify` job.

## SHA Backend

BTCRig selects a SHA256d backend automatically:

- portable C implementation
- OpenSSL SHA256
- ARMv8 SHA2 path, when both compiler and CPU support it
- x86 SHA-NI path, when both compiler and CPU support it

You can override the backend:

```bash
BTC_MINER_SHA_BACKEND=auto ./build/btc_bench -t "$(nproc)" -s 10
BTC_MINER_SHA_BACKEND=openssl ./build/btc_bench -t "$(nproc)" -s 10
BTC_MINER_SHA_BACKEND=fast-c ./build/btc_bench -t "$(nproc)" -s 10
BTC_MINER_SHA_BACKEND=x86-sha-ni ./build/btc_bench -t "$(nproc)" -s 10
BTC_MINER_SHA_BACKEND=arm-sha2 ./build/btc_bench -t "$(nproc)" -s 10
```

On x86 CPUs, `auto` prefers `x86-sha-ni` when available and falls back to OpenSSL otherwise.
The first x86 backend uses SHA-NI for the SHA256 round and message-schedule instructions.
AVX2 multi-lane nonce batching is a separate future optimization.

## Configuration

`btc_stratum` loads `config.json` from the current directory by default.

Minimal example:

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

## Completed Work

- CPU SHA256d Stratum miner with TCP and TLS pool support.
- Config file support with sensible defaults.
- Infinite reconnect with bounded retry backoff.
- Interactive runtime keys for hashrate, pause, resume, results, and connection status.
- Benchmark tool with selectable SHA backend.
- Transparent Stratum proxy with auto TCP/TLS client detection.
- Automatic self-signed proxy certificate generation.
- Windows, Ubuntu/Debian, and Termux build documentation.
- English default docs with Chinese translations.
- Unified `VERSION`-based project versioning starting at `v0.1.0`.
- GitHub Actions Windows UCRT64 build and zip artifact packaging.
- GitHub Actions release publishing for Windows zip packages on `master` and manual release runs.
- x86 SHA-NI SHA256d backend with OpenSSL fallback.
- Transparent XMRig-style time-based developer donation, defaulting to 1%.

## Roadmap

- x86 AVX2 multi-lane nonce batching on top of the SHA-NI backend.
- Linux and Termux GitHub Actions builds.
- Versioned GitHub Releases with tagged artifacts.
- More benchmark modes for comparing OpenSSL, portable C, ARM SHA2, x86 SHA-NI, and future AVX2 batching.
- Cleaner release packaging for Windows and Linux.
- Additional mining stability diagnostics for duplicate shares, reconnects, and pool difficulty changes.
