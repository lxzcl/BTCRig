#!/usr/bin/env bash
set -Eeuo pipefail

BTC_URL="${BTC_URL:-https://github.com/lxzcl/BTCRig/archive/refs/heads/master.zip}"
INSTALL_DIR="${INSTALL_DIR:-${HOME}/BTCRig}"

if [ "$(id -u)" -eq 0 ]; then
    APT="apt"
else
    APT="sudo apt"
fi

$APT update
$APT install -y build-essential cmake make pkg-config git libssl-dev libjansson-dev ca-certificates wget unzip

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
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBTC_MINER_NATIVE=ON
cmake --build build -j"${jobs}"

echo "Installed to ${INSTALL_DIR}"
exec ./build/btc_stratum "$@"
