#!/bin/bash
# Builds the Linux tools: udftool (engine test tool), udffmt, udfchk.
D=$(cd "$(dirname "$0")/.." && pwd); OUT=${OUT:-/tmp/claude-0/udfbuild}; mkdir -p "$OUT"
ENG="$D/src/udfsup.c $D/src/udfbuf.c $D/src/udfvol.c $D/src/udfino.c $D/src/udfdir.c $D/src/udfshim.c"
CF="-O1 -g -Wall -Wno-unused-function -Wno-pointer-sign -I$D/src -I$D/tools -I$D/fmt ${CFLAGS:-}"
set -e
cc $CF $ENG "$D/tools/udftool.c" "$D/tools/udfimg.c" -o "$OUT/udftool"
[ -f "$D/fmt/udffmt.c" ] && cc $CF "$D/fmt/udffmt.c" "$D/fmt/udfmkfs.c" "$D/tools/udfimg.c" $ENG -o "$OUT/udffmt"
gcc -E -P -x assembler-with-cpp "$D/boot/udfboot.S" > "$OUT/udfboot.s"
as --32 -o "$OUT/udfboot.o" "$OUT/udfboot.s"
ld -m elf_i386 -Ttext 0x7C00 --oformat binary -o "$OUT/udfboot.bin" "$OUT/udfboot.o"
[ "$(stat -c %s "$OUT/udfboot.bin")" = 1024 ]
python3 "$D/build/bin2h.py" "$OUT/udfboot.bin" UdfBootCode > "$OUT/udfbootbin.h"
cc $CF -I"$OUT" "$D/fmt/udfboot.c" "$D/tools/udfimg.c" $ENG -o "$OUT/udfboot"
[ -f "$D/chk/udfchk.c" ] && cc $CF "$D/chk/udfchk.c" "$D/tools/udfimg.c" $D/src/udfsup.c -o "$OUT/udfchk"
true
