# shared by build_cross.sh and build_amd64.sh: the tools
TOOLCRT="-include $HERE/toolcrt.h \
 -D_INC_STDIO -D_INC_STDLIB -D_INC_STRING -D_INC_CTYPE -D_INC_TIME -D_INC_STDARG \
 -D_STDIO_H_=1 -D_STDLIB_H=1 -D_STRING_H_=1 -D_CTYPE_H_=1 -D_TIME_H_=1 \
 -Dmain=appmain -Dprintf=ext_printf -Dfprintf=ext_fprintf -Dsprintf=ext_sprintf -Dsnprintf=ext_snprintf -Dvsnprintf=ext_vsnprintf \
 -Dvprintf=ext_vprintf -Dmalloc=ext_malloc -Dfree=ext_free -Dcalloc=ext_calloc -Drealloc=ext_realloc -Dstrdup=ext_strdup \
 -Dstrtoul=ext_strtoul -Dtime=ext_time -Dlocaltime=ext_localtime -Dtoupper=ext_toupper -D_stricmp=ext_stricmp"
TINC="-I$SRC -I$ROOT/tools -I$ROOT/fmt -I$ROOT/chk -I$ROOT/boot -I$OUT"
ENGSRC="$SRC/udfsup.c $SRC/udfbuf.c $SRC/udfvol.c $SRC/udfino.c $SRC/udfdir.c $SRC/udfshim.c $ROOT/tools/udfimg.c"
echo "Assembling the boot loader..."
gcc -E -P -x assembler-with-cpp "$ROOT/boot/udfboot.S" > "$OUT/udfboot.s"
as --32 -o "$OUT/udfboot.o" "$OUT/udfboot.s"
ld -m elf_i386 -Ttext 0x7C00 --oformat binary -o "$OUT/udfboot.bin" "$OUT/udfboot.o"
rm -f "$OUT/udfboot.o" "$OUT/udfboot.s"
[ "$(stat -c %s "$OUT/udfboot.bin")" = 1024 ] || { echo "udfboot.bin is not 1024 bytes"; exit 1; }
python3 "$HERE/bin2h.py" "$OUT/udfboot.bin" UdfBootCode > "$OUT/udfbootbin.h"
echo "Linking udffmt.exe, udfboot.exe, udfchk.exe, udfinst.exe..."
$GCC $EXEBASE $TOOLCRT $TINC -o "$OUT/udffmt.exe" "$ROOT/fmt/udffmt.c" "$ROOT/fmt/udfmkfs.c" $ENGSRC "$HERE/toolcrt.c" -lkernel32 -lgcc
$GCC $EXEBASE $TOOLCRT $TINC -o "$OUT/udfboot.exe" "$ROOT/fmt/udfboot.c" $ENGSRC "$HERE/toolcrt.c" -lkernel32 -lgcc
$GCC $EXEBASE $TOOLCRT $TINC -o "$OUT/udfchk.exe" "$ROOT/chk/udfchk.c" "$SRC/udfsup.c" "$ROOT/tools/udfimg.c" "$HERE/toolcrt.c" -lkernel32 -lgcc
if [ -f "$ROOT/inst/udfinst.c" ]; then
    $GCC $EXEBASE $TOOLCRT -o "$OUT/udfinst.exe" "$ROOT/inst/udfinst.c" "$HERE/toolcrt.c" -ladvapi32 -lkernel32 -lgcc
fi
