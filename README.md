<div align="center">

# BTCRig

**A compact Bitcoin SHA256d CPU miner, benchmark, and Stratum proxy.**

[简体中文](README.zh-CN.md) · [Releases](https://github.com/lxzcl/BTCRig/releases) · [Wiki](https://github.com/lxzcl/BTCRig/wiki)

![Release](https://img.shields.io/github/v/release/lxzcl/BTCRig?style=for-the-badge&color=00b894)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20Termux-00b894?style=for-the-badge)
![SHA256d](https://img.shields.io/badge/SHA256d-CPU-00b894?style=for-the-badge)
![Donation](https://img.shields.io/badge/default%20donation-0%25-00b894?style=for-the-badge)

</div>

BTCRig is an experimental project for learning, benchmarking, and low-power solo-mining experiments. It is not intended to compete with ASIC miners.

## Programs

| Program | Purpose |
| --- | --- |
| `btc_stratum` | CPU miner with Stratum V1, TCP/TLS, reconnect, and interactive statistics |
| `btc_bench` | Local SHA256d benchmark with selectable backends |
| `btc_proxy` | Multi-client Stratum proxy with TCP/TLS auto-detection |

## Highlights

- Automatic backend selection: x86 SHA-NI, ARMv8 SHA2, OpenSSL, or portable C.
- Two-lane interleaved x86 SHA-NI scanning and dedicated ARMv8 SHA2 range scanning.
- Uses every logical CPU by default; thread count remains configurable.
- Continuous network reconnect with bounded backoff.
- Plain TCP and verified or compatible TLS pool connections.
- Human-readable hashrate units and per-thread runtime statistics.
- Default developer donation is **disabled (`0%`)**.

## Quick Start

### Download a release

Windows packages are published on the [Releases page](https://github.com/lxzcl/BTCRig/releases). Extract the zip, edit `config.json`, then run:

```powershell
.\btc_stratum.exe
```

### Ubuntu / Debian

```bash
wget -O ubuntu.sh https://raw.githubusercontent.com/lxzcl/BTCRig/master/ubuntu.sh
chmod +x ubuntu.sh
./ubuntu.sh
```

### Termux

```bash
pkg update
pkg install -y wget
wget -O termux.sh https://raw.githubusercontent.com/lxzcl/BTCRig/master/termux.sh
chmod +x termux.sh
./termux.sh
```

### Build from source

```bash
git clone https://github.com/lxzcl/BTCRig.git
cd BTCRig
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j"$(nproc)"
./build/btc_stratum --self-test
./build/btc_stratum
```

Platform-specific dependencies, Windows DLL packaging, and CI details are kept in the [Build and Releases wiki page](https://github.com/lxzcl/BTCRig/wiki/Build-and-Releases).

## Performance Snapshots

These are observed project measurements, not controlled cross-platform benchmarks. Compiler versions, clock limits, cooling, and background load can materially change the result.

| Platform | Environment | Backend | Threads | Observed SHA256d |
| --- | --- | --- | ---: | ---: |
| Snapdragon 8 Elite | Termux | ARMv8 SHA2 | 8 | ~150 MH/s |
| NanoPi Fire3 | Linux ARM64 | ARMv8 SHA2 | 8 | ~34.4 MH/s |
| NanoPi M3 | Linux ARM64 | ARMv8 SHA2 | 8 | ~12.3 MH/s |
| RockPi-S | Linux ARM64 | ARMv8 SHA2 | 4 | ~6 MH/s |

Run the same local benchmark when comparing builds:

```bash
./build/btc_bench -t "$(nproc)" -s 10
```

## Runtime

The miner reports every usable SHA backend and the selected path at startup:

```text
[SHA] available=x86-sha-ni,openssl,fast-c
[SHA] selected=x86-sha-ni mode=auto
```

Common commands:

```text
-o, --url URL              pool URL
-u, --user USER            wallet or username
-p, --pass PASS            password
-d, --suggest-diff N       suggested initial difficulty
-t, --threads N            CPU thread count, 0 means auto
--stats N                  statistics interval in seconds
--runtime N                runtime limit, 0 means unlimited
--donate-level N           donation percentage, default 0
--no-mine                  test the connection without mining
--cpu-info                 print CPU topology
--self-test                run the Stratum parser self-test
```

Interactive keys while mining:

| Key | Action |
| --- | --- |
| `h` | Per-thread hashrate |
| `p` | Pause mining |
| `r` | Resume mining |
| `s` | Share results |
| `c` | Connection information |

## Configuration

`btc_stratum` reads `config.json` from the current directory. Replace the example wallet before mining for yourself.

```json
{
  "cpu": {
    "enabled": true,
    "threads": 0
  },
  "pools": [
    {
      "url": "stratum+tls://public-pool.io:4333",
      "user": "bc1q_example_wallet.worker",
      "pass": "x",
      "diff": 0.001
    }
  ],
  "retries": -1,
  "retry-pause": 2,
  "donate-level": 0,
  "print-time": 10,
  "runtime": 0
}
```

The pool controls the effective share difficulty through `mining.set_difficulty`; `diff` is only an initial suggestion.

## Backends

| Backend | Availability | Notes |
| --- | --- | --- |
| `x86-sha-ni` | x86 CPU with SHA extensions | Preferred x86 path, two interleaved nonce lanes |
| `arm-sha2` | ARMv8 CPU with SHA2 extensions | Dedicated ARM range scanner |
| `openssl` | All supported builds | Library fallback |
| `fast-c` | All supported builds | Portable C fallback |

Override automatic selection with `BTC_MINER_SHA_BACKEND`, for example:

```bash
BTC_MINER_SHA_BACKEND=openssl ./build/btc_bench -t "$(nproc)" -s 10
```

## Documentation

- [Build and Releases](https://github.com/lxzcl/BTCRig/wiki/Build-and-Releases)
- [Proxy guide](PROXY.md)
- [Chinese README](README.zh-CN.md)
- [Release downloads](https://github.com/lxzcl/BTCRig/releases)

## License

BTCRig is distributed under the [GNU General Public License v3.0](LICENSE).
