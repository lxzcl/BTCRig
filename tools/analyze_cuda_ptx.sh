#!/usr/bin/env sh
set -eu

arch="${1:-sm_50}"
root_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
src="$root_dir/src/cuda_sha256d_kernel.cu"
ptx="${TMPDIR:-/tmp}/btcrig_cuda_sha256d_${arch}_analysis.ptx"

clang++ -x cuda --cuda-gpu-arch="$arch" -S --cuda-device-only \
    -nocudainc -nocudalib -O3 "$src" -o "$ptx"

printf 'CUDA PTX analysis: %s (%s)\n' "$src" "$arch"
printf '%-44s %8s %8s %8s %8s %8s %8s %8s %8s %8s %8s %8s\n' \
    kernel pred b32 b64 instr shf lop3 and xor shr add mad

awk '
function reset() {
    kernel = "";
    pred = 0; b32 = 0; b64 = 0; instr = 0;
    shf = 0; lop3 = 0; andc = 0; xorc = 0; shr = 0; add = 0; mad = 0;
}
function print_kernel() {
    if (kernel != "") {
        printf "%-44s %8d %8d %8d %8d %8d %8d %8d %8d %8d %8d %8d\n",
               kernel, pred, b32, b64, instr, shf, lop3, andc, xorc, shr, add, mad;
    }
}
BEGIN {
    reset();
}
$1 == ".visible" && $2 == ".entry" {
    print_kernel();
    reset();
    kernel = $3;
    sub(/\(.*/, "", kernel);
    next;
}
kernel != "" && $1 == ".reg" && $2 == ".pred" {
    line = $0;
    sub(/.*%p</, "", line);
    sub(/>.*/, "", line);
    pred = line + 0;
    next;
}
kernel != "" && $1 == ".reg" && $2 == ".b32" {
    line = $0;
    sub(/.*%r</, "", line);
    sub(/>.*/, "", line);
    b32 = line + 0;
    next;
}
kernel != "" && $1 == ".reg" && $2 == ".b64" {
    line = $0;
    sub(/.*%rd</, "", line);
    sub(/>.*/, "", line);
    b64 = line + 0;
    next;
}
kernel != "" && $1 ~ /^[A-Za-z_][A-Za-z0-9_]*\./ {
    op = $1;
    sub(/\..*/, "", op);
    instr++;
    if (op == "shf") shf++;
    else if (op == "lop3") lop3++;
    else if (op == "and") andc++;
    else if (op == "xor") xorc++;
    else if (op == "shr") shr++;
    else if (op == "add") add++;
    else if (op == "mad") mad++;
}
END {
    print_kernel();
}
' "$ptx"

printf '\nNotes:\n'
printf '%s\n' '- shf in PTX means rotate expressions reached funnel-shift form before driver JIT.'
printf '%s\n' '- lop3 and iadd3 are normally SASS-level checks; if this report shows lop3=0, the driver may still fuse and/xor chains during JIT.'
printf '%s\n' '- Use nvdisasm/cuobjdump on a CUDA Toolkit machine for final SASS confirmation.'
