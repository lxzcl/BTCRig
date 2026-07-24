#!/usr/bin/env sh
set -eu

arch="${1:-sm_50}"
root_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
src="$root_dir/src/cuda_sha256d_kernel.cu"
ptx="${TMPDIR:-/tmp}/btcrig_cuda_sha256d_${arch}.ptx"
out="$root_dir/src/cuda_sha256d_ptx.h"
clangxx="${CLANGXX:-clang++}"

"$clangxx" -x cuda --cuda-gpu-arch="$arch" -S --cuda-device-only \
    -nocudainc -nocudalib -O3 "$src" -o "$ptx"

if grep -q 'lop3\.b32' "$ptx"; then
    awk 'BEGIN { done = 0 } !done && $0 == ".version 4.2" { print ".version 5.0"; done = 1; next } { print }' "$ptx" > "$ptx.tmp"
    mv "$ptx.tmp" "$ptx"
fi

{
    printf '#ifndef BTCRIG_CUDA_SHA256D_PTX_H\n'
    printf '#define BTCRIG_CUDA_SHA256D_PTX_H\n\n'
    printf 'static const char k_btcrig_cuda_sha256d_ptx[] =\n'
    sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$/\\n"/' "$ptx"
    printf ';\n\n'
    printf '#endif\n'
} > "$out"

printf 'Generated %s from %s for %s\n' "$out" "$src" "$arch"
