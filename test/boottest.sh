#!/bin/bash
# boottest.sh: udffmt /BOOT + filler + a synthetic Windows tree + udfboot, for
# several revisions and block sizes; the FAT view (read with mtools) must
# equal the UDF files, and udfchk must find the volume clean.
B=${B:-/tmp/claude-0/udfbuild}; W=${W:-/tmp/claude-0/udfbuild/boottest}
export MTOOLS_SKIP_CHECK=1
rm -rf $W; mkdir -p $W/t/WINNT/System32/drivers $W/t/WINNT/System32/config $W/t/WINNT/Fonts $W/t/WINNT/inf $W/fill
cd $W/t
head -c 156496 /dev/urandom > ntldr; head -c 26816 /dev/urandom > NTDETECT.COM
printf '[boot loader]\r\ndefault=multi(0)disk(0)rdisk(0)partition(1)\\WINNT\r\n' > boot.ini
for f in ntoskrnl.exe hal.dll kdcom.dll bootvid.dll c_1252.nls l_intl.nls; do head -c $((RANDOM*20+1000)) /dev/urandom > WINNT/System32/$f; done
for i in $(seq 1 150); do head -c $((RANDOM+100)) /dev/urandom > WINNT/System32/drivers/drv$i.sys; done
head -c 3000000 /dev/urandom > WINNT/System32/config/SYSTEM; head -c 2000000 /dev/urandom > WINNT/System32/config/system.alt
head -c 5000 /dev/urandom > WINNT/Fonts/vgaoem.fon; head -c 900 /dev/urandom > WINNT/inf/biosinfo.inf
for i in $(seq 1 60); do head -c $((RANDOM*30)) /dev/urandom > $W/fill/f$i.bin; done
rc=0
for v in "200 NOBOOT /R:2.01" "300 NOBOOT /R:2.50" "200 GROW /R:2.01" "300 GROW /R:2.50 /B:512" "200 /R:2.01" "200 /R:1.02" "150 /R:1.50" "300 /R:2.50" "300 /R:2.60 /B:4096" "200 /R:2.01 /B:2048" "400 /R:2.00 /B:8192" "300 /R:2.01 /B:65536" "200 /R:2.50 /B:1024 /NOMETA"; do
  set -- $v; MB=$1; shift 1; mode=; FB=/BOOT
  case "$1" in NOBOOT) mode=$1; FB=; shift;; GROW) mode=$1; shift;; esac
  cd $W; rm -f d.img
  truncate -s $((MB+1))M d.img
  python3 - "$MB" <<'P'
import struct,sys
mb=int(sys.argv[1]); f=open('d.img','r+b')
d=bytearray(512); d[446:462]=bytes([0,0,0,0,0x07,0,0,0])+struct.pack('<II',63,mb*2048); d[510:512]=b'\x55\xaa'
f.write(d)
P
  $B/udffmt d.img $FB /Y /Q "$@" >/dev/null || { echo "== $*: udffmt failed"; rc=1; continue; }
  $B/udftool d.img putr $W/fill /fill >/dev/null          # the start of the volume taken first
  $B/udftool d.img putr $W/t / >/dev/null
  out=$($B/udfboot d.img 2>&1) || { echo "== $*: udfboot failed: $out"; rc=1; continue; }
  if [ "$mode" = GROW ]; then
    # after the view: more files (kept out of the window), a hive that grew, a new driver
    $B/udftool d.img putr $W/fill /fill2 >/dev/null
    head -c 4500000 /dev/urandom > t/WINNT/System32/config/SYSTEM; $B/udftool d.img write /WINNT/System32/config/SYSTEM 0 t/WINNT/System32/config/SYSTEM >/dev/null
    head -c 70000 /dev/urandom > t/WINNT/System32/drivers/udfnt.sys; $B/udftool d.img put t/WINNT/System32/drivers/udfnt.sys /WINNT/System32/drivers/udfnt.sys >/dev/null
    out=$($B/udfboot d.img 2>&1) || { echo "== $*: udfboot (again) failed: $out"; rc=1; continue; }
  fi
  rm -rf fv; mkdir fv; mcopy -s -n -i d.img@@32256 '::/*' fv/ 2>/dev/null
  bad=0; n=0
  for f in $(cd fv; find . -type f); do
    n=$((n+1)); src=$(cd t; find . -ipath "$f" | head -1)
    cmp -s "fv/$f" "t/$src" || { bad=$((bad+1)); echo "   differs: $f"; }
  done
  c=$($B/udfchk d.img /Q >/dev/null 2>&1; echo $?)
  want=163; [ "$mode" = GROW ] && want=164
  [ $n = $want ] && [ $bad = 0 ] && [ $c = 0 ] && s=OK || { s=FAIL; rc=1; }
  echo "== ${MB}M $mode $*: $s ($n files in the view, $bad differ, udfchk $c) $(echo "$out" | grep -o '[0-9]* KB clusters') $(echo "$out" | grep -o '[0-9]* files moved further out')"
  rm -f t/WINNT/System32/drivers/udfnt.sys
done
exit $rc
