#!/usr/bin/env bash
set -Eeuo pipefail

BTC_URL="${BTC_URL:-https://github.com/lxzcl/BTCRig/archive/refs/heads/master.zip}"
INSTALL_DIR="${INSTALL_DIR:-${HOME}/BTCRig}"
BTCRIG_NATIVE="${BTCRIG_NATIVE:-OFF}"
BTCRIG_OPENCL="${BTCRIG_OPENCL:-OFF}"
BTCRIG_RUN="${BTCRIG_RUN:-1}"

pkg update
pkg upgrade -y
pkg install -y clang make cmake jsoncpp git openssl openssl-tool pkg-config libjansson wget unzip

ensure_cmake_works() {
    if cmake --version >/dev/null 2>&1; then
        return 0
    fi

    echo "Termux cmake failed to start. Reinstalling cmake/jsoncpp to repair library mismatch..." >&2
    pkg install -y cmake jsoncpp
    pkg reinstall -y cmake jsoncpp || pkg install -y --reinstall cmake jsoncpp || true

    if cmake --version >/dev/null 2>&1; then
        return 0
    fi

    echo "cmake still cannot start." >&2
    echo "Please run these commands, then run this installer again:" >&2
    echo "  pkg update" >&2
    echo "  pkg upgrade -y" >&2
    echo "  pkg reinstall -y cmake jsoncpp" >&2
    return 1
}

if ensure_cmake_works; then
    use_cmake=1
else
    use_cmake=0
    echo "Falling back to direct clang build without cmake." >&2
fi

tmp_dir="$(mktemp -d)"
zip_path="${tmp_dir}/btc.zip"
src_root="${tmp_dir}/src"
jobs="$(nproc 2>/dev/null || echo 1)"

cleanup() {
    status=$?
    if [ "${status}" -eq 0 ]; then
        rm -rf "${tmp_dir}"
    else
        echo "Install failed. Temporary files are kept at: ${tmp_dir}" >&2
    fi
}
trap cleanup EXIT

download_zip() {
    local url="$1"
    local out="${zip_path}.tmp"
    rm -f "${out}" "${zip_path}"
    echo "Downloading ${url}"
    wget --tries=3 --timeout=30 --retry-connrefused -O "${out}" "${url}"
    if [ ! -s "${out}" ]; then
        echo "Downloaded file is empty: ${url}" >&2
        return 1
    fi
    mv "${out}" "${zip_path}"
}

if ! download_zip "${BTC_URL}"; then
    echo "Download failed: ${BTC_URL}" >&2
    exit 1
fi
if ! unzip -tq "${zip_path}" >/dev/null; then
    echo "Archive check failed: ${zip_path}" >&2
    exit 1
fi
mkdir -p "${src_root}"
unzip -q "${zip_path}" -d "${src_root}"

src_dir="${src_root}"
if [ ! -f "${src_dir}/CMakeLists.txt" ]; then
    first_dir="$(find "${src_root}" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
    if [ -n "${first_dir}" ] && [ -f "${first_dir}/CMakeLists.txt" ]; then
        src_dir="${first_dir}"
    fi
fi

if [ ! -f "${src_dir}/CMakeLists.txt" ]; then
    echo "CMakeLists.txt not found in ${BTC_URL}" >&2
    exit 1
fi

if [ -e "${INSTALL_DIR}" ]; then
    backup="${INSTALL_DIR}.backup.$(date +%Y%m%d%H%M%S)"
    mv "${INSTALL_DIR}" "${backup}"
    echo "Existing install moved to ${backup}"
fi

mkdir -p "${INSTALL_DIR}"
cp -a "${src_dir}/." "${INSTALL_DIR}/"
find "${INSTALL_DIR}" -exec touch {} +

cd "${INSTALL_DIR}"

build_with_cmake() {
    rm -rf build
    cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBTC_MINER_NATIVE="${BTCRIG_NATIVE}" \
        -DBTCRIG_OPENCL="${BTCRIG_OPENCL}"
    cmake --build build -j"${jobs}"
}

generate_version_header() {
    local version tag
    mkdir -p build/generated
    version="$(tr -d '[:space:]' < VERSION)"
    version="${version#v}"
    tag="v${version}"
    sed \
        -e "s/@BTCRIG_VERSION@/${version}/g" \
        -e "s/@BTCRIG_VERSION_TAG@/${tag}/g" \
        src/btcrig_version.h.in > build/generated/btcrig_version.h
}

build_with_clang() {
    local openssl_cflags openssl_libs jansson_cflags jansson_libs common_flags native_flags
    local arm_sha_flags sha_defs sha_objects

    openssl_cflags="$(pkg-config --cflags openssl 2>/dev/null || true)"
    openssl_libs="$(pkg-config --libs openssl 2>/dev/null || echo "-lssl -lcrypto")"
    jansson_cflags="$(pkg-config --cflags jansson 2>/dev/null || true)"
    jansson_libs="$(pkg-config --libs jansson 2>/dev/null || echo "-ljansson")"
    common_flags="-std=c11 -Wall -Wextra -Wpedantic -DOPENSSL_SUPPRESS_DEPRECATED -Isrc -Ibuild/generated"
    native_flags=""
    if [ "${BTCRIG_NATIVE}" = "ON" ] || [ "${BTCRIG_NATIVE}" = "on" ] || [ "${BTCRIG_NATIVE}" = "1" ]; then
        if printf 'int main(void){return 0;}\n' | cc -x c - -o /dev/null -march=native >/dev/null 2>&1; then
            native_flags="-march=native"
        fi
    fi

    arm_sha_flags=""
    sha_defs=""
    sha_objects="build/obj/sha256d.o"
    if printf '#include <arm_neon.h>\nint main(void){uint32x4_t x=vdupq_n_u32(0); x=vsha256hq_u32(x,x,x); return (int)vgetq_lane_u32(x,0);}\n' | cc -x c - -o /dev/null -march=armv8-a+crypto >/dev/null 2>&1; then
        arm_sha_flags="-march=armv8-a+crypto"
    fi

    rm -rf build
    mkdir -p build/obj
    generate_version_header

    if [ -n "${arm_sha_flags}" ]; then
        sha_defs="-DBTC_MINER_ARM_SHA2"
        sha_objects="${sha_objects} build/obj/sha256d_arm_sha2.o"
        echo "Detected ARMv8 SHA2 compiler support: ${arm_sha_flags}"
        cc ${common_flags} -O3 ${arm_sha_flags} -c src/sha256d_arm_sha2.c -o build/obj/sha256d_arm_sha2.o
    fi

    cc ${common_flags} ${native_flags} ${sha_defs} -O3 ${openssl_cflags} -c src/sha256d.c -o build/obj/sha256d.o

    echo "Building btc_bench with clang..."
    cc ${common_flags} ${native_flags} ${sha_defs} -O3 \
        ${openssl_cflags} \
        src/main.c src/console.c src/cpu_info.c ${sha_objects} \
        -o build/btc_bench \
        ${openssl_libs} -pthread

    echo "Building btc_stratum with clang..."
    cc ${common_flags} ${native_flags} ${sha_defs} -O3 \
        ${openssl_cflags} ${jansson_cflags} \
        src/stratum_main.c src/stratum.c src/console.c src/cpu_info.c src/donation.c src/miner.c ${sha_objects} \
        -o build/btc_stratum \
        ${openssl_libs} ${jansson_libs} -pthread

    echo "Building btc_proxy with clang..."
    cc ${common_flags} ${native_flags} -O2 \
        ${openssl_cflags} ${jansson_cflags} \
        src/proxy_main.c \
        -o build/btc_proxy \
        ${openssl_libs} ${jansson_libs} -pthread
}

if [ "${use_cmake}" -eq 1 ]; then
    if ! build_with_cmake; then
        echo "cmake build failed. Falling back to direct clang build..." >&2
        build_with_clang
    fi
else
    build_with_clang
fi

./build/btc_stratum --self-test

echo "Installed to ${INSTALL_DIR}"
if [ "${BTCRIG_RUN}" = "0" ] || [ "${BTCRIG_RUN}" = "OFF" ] || [ "${BTCRIG_RUN}" = "off" ]; then
    exit 0
fi

exec ./build/btc_stratum "$@"
