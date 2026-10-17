#!/bin/sh
# UDFNT cross build: a native NT 3.1 - XP (i386) udfnt.sys with GCC and the
# mingw-w64 DDK headers, no Visual C++; then the tools.
#
# Needs: gcc-mingw-w64-i686 (ships ddk headers), binutils, python3.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SRC="$ROOT/src"
OUT="${1:-/tmp/udfnt-i386}"
mkdir -p "$OUT"

GCC=i686-w64-mingw32-gcc
DLLTOOL=i686-w64-mingw32-dlltool
NM=i686-w64-mingw32-nm
INC="-I$SRC -I/usr/i686-w64-mingw32/include/ddk -I/usr/i686-w64-mingw32/include"
# -march=i486: NT 3.1 runs on 80486 (and 386) CPUs; no P6 instructions.
FLAGS="-c -ffreestanding -fno-stack-protector -fno-builtin -march=i486 -mtune=i486 -fno-strict-aliasing \
 -D_X86_ -DWIN32 -D_WIN32_WINNT=0x0600 -DNTDDI_VERSION=0x06000000 -DUDN_CROSS -DUDF_KERNEL -w $EXTRACFLAGS"
ENGINE="udfsup udfbuf udfvol udfino udfdir udfshim ufx"

echo "Compiling driver sources..."
rm -f "$OUT"/*.o
for f in "$SRC"/udn*.c; do
    $GCC $FLAGS -O1 $INC "$f" -o "$OUT/$(basename "${f%.c}").o"
done
for b in $ENGINE; do
    $GCC $FLAGS -O2 $INC "$SRC/$b.c" -o "$OUT/$b.o"
done
$GCC $FLAGS -O2 $INC "$HERE/udfcrt.c" -o "$OUT/udfcrt.o"

echo "Building the ntoskrnl import library from the symbols used..."
$NM "$OUT"/*.o 2>/dev/null | awk '/ U /{print $2}' | grep '^__imp_' \
    | sed 's/^__imp_//; s/^_//' | sort -u > "$OUT/need.txt"
{ echo "LIBRARY ntoskrnl.exe"; echo "EXPORTS"; cat "$OUT/need.txt"; } > "$OUT/ntoskrnl.def"
$DLLTOOL --kill-at -d "$OUT/ntoskrnl.def" -l "$OUT/libntoskrnl_nt31.a"

if [ -f "$NT31_EXPORTS" ]; then
    echo "Checking imports against NT 3.1 ntoskrnl..."
    sed 's/@[0-9]*$//' "$OUT/need.txt" | while read s; do
        grep -qx "$s" "$NT31_EXPORTS" || echo "  NOT EXPORTED BY NT 3.1: $s"
    done
fi

echo "Linking udfnt.sys (native, subsystem 3.10, base 0x10000)..."
$GCC -shared -nostdlib -nostartfiles \
    -Wl,--subsystem,native -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 \
    -Wl,--major-os-version,1 -Wl,--minor-os-version,0 \
    -Wl,--major-image-version,1 -Wl,--minor-image-version,0 \
    -Wl,--image-base,0x10000 -Wl,--entry,_DriverEntry@8 -Wl,--file-alignment,0x200 \
    -o "$OUT/udfnt.sys" "$OUT"/*.o "$OUT/libntoskrnl_nt31.a" -lgcc

cp "$OUT/udfnt.sys" "$OUT/udfnt.dbg"
i686-w64-mingw32-strip --strip-all "$OUT/udfnt.sys"
python3 "$HERE/pechecksum.py" "$OUT/udfnt.sys"
echo "Built: $OUT/udfnt.sys"
[ -n "$DRIVER_ONLY" ] && exit 0

# ----------------------------------------------------------------------
# The user-mode tools: freestanding console programs (subsystem 3.10) so
# they run on NT 3.1 with no C-runtime DLL; toolcrt.c is the tiny runtime.
# ----------------------------------------------------------------------
EXEBASE="-O2 -w -ffreestanding -nostdlib -nostartfiles -march=i486 -mtune=i486 -e _mainCRTStartup \
 -Wl,--subsystem,console -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 \
 -Wl,--major-os-version,1 -Wl,--minor-os-version,0 -D_WIN32 -D_CONSOLE"
. "$HERE/tools.inc"
echo "Done."
