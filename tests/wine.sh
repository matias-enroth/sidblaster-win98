#!/bin/sh
# Runs SBTEST NET and HSPLAY.EXE (a SIDPLAY stand-in driving HardSID.dll) under
# Wine in Docker (linux/amd64) against tools/netsid_mock.py, in one container.
# Leaves build/wine/{mock.log,mock.err,SIDBLAST.LOG,run.txt}.
#   tests/wine.sh [seconds per tune] [tunes] [extra SIDBLAST.INI lines...]
set -e
cd "$(dirname "$0")/.."
SECS=${1:-5}; TUNES=${2:-2}; shift 2 2>/dev/null || shift $#
./build.sh >/dev/null
CC=${CC:-i686-w64-mingw32-gcc}
$CC -O2 -march=i586 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Wno-cast-function-type \
  -nostdlib -o build/HSPLAY.EXE tests/hsplay.c -Wl,-e,_mainCRTStartup \
  -Wl,--subsystem,console:4.0 -lkernel32 -luser32
docker build -q --platform linux/amd64 -t sbwine tests/wine >/dev/null
rm -rf build/wine && mkdir -p build/wine
{ printf '[SIDBlaster]\r\nBackend=NetSID\r\nHost=127.0.0.1\r\nLog=1\r\n'
  for l in "$@"; do printf '%s\r\n' "$l"; done; } > build/wine/SIDBLAST.INI
docker run --rm --platform linux/amd64 -v "$PWD":/src sbwine sh -c "
  set -e
  mkdir /work && cd /work
  cp /src/build/HardSID.dll /src/build/SBTEST.EXE /src/build/HSPLAY.EXE /src/build/wine/SIDBLAST.INI .
  python3 /src/tools/netsid_mock.py --log /src/build/wine/mock.log --start-ms 100 2>/src/build/wine/mock.err &
  sleep 1
  echo '== SBTEST NET 127.0.0.1'
  wine SBTEST.EXE NET 127.0.0.1
  echo '== HSPLAY $SECS $TUNES'
  wine HSPLAY.EXE $SECS $TUNES
  sleep 1
  cp SIDBLAST.LOG /src/build/wine/ 2>/dev/null || true
" 2>&1 | tr -d '\r' | tee build/wine/run.txt
