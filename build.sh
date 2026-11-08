#!/bin/sh
# Cross-builds HardSID.dll and SBTEST.EXE for Windows 9x.
# Needs i686-w64-mingw32-gcc (macOS: brew install mingw-w64; Debian/Ubuntu: gcc-mingw-w64-i686).
# No C runtime; the binaries import only Win95-era KERNEL32/USER32/ADVAPI32 functions
# (WSOCK32 is loaded at run time, for Backend=NetSID / SBTEST NET only),
# and code is generated for i586 (no CMOV), so it runs on K6-2/Pentium-class CPUs.
set -e
cd "$(dirname "$0")"
CC=${CC:-i686-w64-mingw32-gcc}
CF="-O2 -march=i586 -mtune=k6-2 -ffreestanding -fno-builtin -fno-stack-protector \
 -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns -Wall -Wextra \
 -Wno-unused-parameter -DWINVER=0x0400 -D_WIN32_WINNT=0x0400"
LF="-nostdlib -Wl,--major-os-version,4 -Wl,--minor-os-version,0 \
 -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--no-insert-timestamp"
mkdir -p build
NET="src/netsid_net.c src/netsid_proto.c"
$CC $CF -shared $LF -o build/HardSID.dll src/hardsid.c $NET src/minifmt.c -Wl,--kill-at \
  -Wl,-e,_DllMain@12 -Wl,--subsystem,windows:4.0 -Wl,--image-base,0x68000000 -lkernel32 -luser32 -ladvapi32
$CC $CF $LF -o build/SBTEST.EXE src/sbtest.c $NET \
  -Wl,-e,_mainCRTStartup -Wl,--subsystem,console:4.0 -lkernel32 -luser32 -ladvapi32
cp dist/README.TXT dist/SIDBLAST.INI build/
echo "built: build/HardSID.dll build/SBTEST.EXE"
