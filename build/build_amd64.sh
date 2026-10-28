#!/bin/sh
# UDFNT for Windows XP x64 / Server 2003 x64 (amd64), with GCC
# (gcc-mingw-w64-x86-64): udfnt.sys and the tools.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SRC="$ROOT/src"
OUT="${1:-/tmp/udfnt-amd64}"
mkdir -p "$OUT"

GCC=x86_64-w64-mingw32-gcc
INC="-I$SRC -I/usr/x86_64-w64-mingw32/include/ddk -I/usr/x86_64-w64-mingw32/include"
# The mingw DDK headers want the Vista level to be self-consistent; only
# functions XP x64 exports are imported (checked below).
FLAGS="-c -ffreestanding -fno-stack-protector -fno-builtin -mno-stack-arg-probe -fno-strict-aliasing \
 -D_AMD64_ -DWIN32 -D_WIN64 -D_WIN32_WINNT=0x0600 -DNTDDI_VERSION=0x06000000 -DUDF_KERNEL -w $EXTRACFLAGS"
ENGINE="udfsup udfbuf udfvol udfino udfdir udfshim ufx"

echo "Compiling driver sources (amd64)..."
rm -f "$OUT"/*.o
for f in "$SRC"/udn*.c; do
    $GCC $FLAGS -O1 $INC "$f" -o "$OUT/$(basename "${f%.c}").o"
done
for b in $ENGINE; do
    $GCC $FLAGS -O2 $INC "$SRC/$b.c" -o "$OUT/$b.o"
done
$GCC $FLAGS -O2 $INC "$HERE/udfcrt.c" -o "$OUT/udfcrt.o"

echo "Linking udfnt.sys (native, subsystem 5.02)..."
$GCC -shared -nostdlib -nostartfiles \
    -Wl,--subsystem,native -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2 \
    -Wl,--major-os-version,5 -Wl,--minor-os-version,2 \
    -Wl,--major-image-version,1 -Wl,--minor-image-version,1 \
    -Wl,--image-base,0x10000 -Wl,--entry,DriverEntry -Wl,--file-alignment,0x200 \
    -Wl,--section-alignment,0x1000 \
    -o "$OUT/udfnt.sys" "$OUT"/*.o -lntoskrnl -lhal -lgcc
x86_64-w64-mingw32-strip --strip-all "$OUT/udfnt.sys"
python3 "$HERE/pechecksum.py" "$OUT/udfnt.sys"

if [ -f "$XP64_EXPORTS" ]; then
    echo "Checking imports against XP x64 ntoskrnl..."
    x86_64-w64-mingw32-objdump -p "$OUT/udfnt.sys" | awk '/^\t[0-9a-f]+\t +[0-9]+ +[A-Za-z_]/{print $3}' | while read s; do
        grep -qx "$s" "$XP64_EXPORTS" || echo "  NOT EXPORTED BY XP x64: $s"
    done
fi
echo "Built: $OUT/udfnt.sys"
[ -n "$DRIVER_ONLY" ] && exit 0

# ----------------------------------------------------------------------
# Tools (console, no C runtime DLL)
# ----------------------------------------------------------------------
EXEBASE="-O2 -w -ffreestanding -nostdlib -nostartfiles -mno-stack-arg-probe -e mainCRTStartup \
 -Wl,--subsystem,console -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2 \
 -Wl,--major-os-version,5 -Wl,--minor-os-version,2 -D_WIN32 -D_CONSOLE"
. "$HERE/tools.inc"
echo "Done."
