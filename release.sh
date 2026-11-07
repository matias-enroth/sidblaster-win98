#!/bin/sh
# Builds, checks that the binaries are still Win9x-safe, and packs the release zip:
#   build/sidblaster-win98-<version>.zip  with  SIDBLAST/{HardSID.dll,SBTEST.EXE,SIDBLAST.INI,README.TXT}
set -e
cd "$(dirname "$0")"
VER=$(sed -n 's/^#define SB_VERSION *"\(.*\)"/\1/p' src/sbcommon.h)
OD=${OD:-i686-w64-mingw32-objdump}
grep -Eq "version $VER[[:space:]]*\$" dist/README.TXT || { echo "dist/README.TXT doesn't say version $VER"; exit 1; }

rm -f build/HardSID.dll build/SBTEST.EXE
./build.sh

# Imports: only functions Windows 95 has. Anything new must be checked and added here.
OK='ADVAPI32.dll KERNEL32.dll USER32.dll
RegCloseKey RegEnumKeyExA RegOpenKeyExA RegQueryValueExA
CloseHandle CreateEventA CreateFileA CreateThread DisableThreadLibraryCalls
EnterCriticalSection ExitProcess GetCommState GetCommandLineA GetLastError
GetModuleFileNameA GetPrivateProfileIntA GetPrivateProfileStringA GetStdHandle
GetTickCount InitializeCriticalSection LeaveCriticalSection LoadLibraryA PurgeComm
QueryPerformanceCounter QueryPerformanceFrequency ReadFile SetCommState
SetCommTimeouts SetEvent SetFilePointer SetThreadPriority SetupComm Sleep
WaitForSingleObject WriteFile lstrcatA lstrcmpiA lstrcpyA lstrcpynA lstrlenA
wsprintfA wvsprintfA'
fail=0
for f in build/HardSID.dll build/SBTEST.EXE; do
  imps=$($OD -p "$f" | sed -n '/Import Tables/,/Export Tables/p' \
         | awk '/DLL Name:/{print $3} /^\t[0-9a-f]+ +<none>/{print $NF}')
  # objdump's format changing must not make this pass by finding nothing
  [ "$(echo "$imps" | wc -w)" -ge 10 ] || { echo "$f: couldn't read its imports"; fail=1; }
  for imp in $imps; do
    echo "$OK" | tr ' ' '\n' | grep -qx "$imp" || { echo "$f: unexpected import $imp"; fail=1; }
  done
  $OD -p "$f" | grep -q 'MajorSubsystemVersion	4' || { echo "$f: subsystem version is not 4"; fail=1; }
  # i586 (K6-2): no CMOV, FCMOV, FCOMI, long NOPs (0F 1F), MMX/SSE
  if $OD -d --no-show-raw-insn "$f" | grep -Eiq '	(cmov|fcmov|fcomi|fucomi|nopw|nopl|movs[sd]|movap|movup|movq|movd|xorps|andps|orps|cvt|ucomis|comis|emms|p(xor|and|or|add|sub|mul|cmp|shuf|unpck|ack|sll|srl|sra|min|max|avg))'; then
    echo "$f: instructions newer than i586"; fail=1
  fi
  # libgcc helpers (64-bit divide etc.); mingw adds a leading _ to C symbols
  if $OD -d "$f" | grep -Eq '<___?(u)?(div|mod)di3|<___?udivmoddi4|<___?chkstk'; then
    echo "$f: libgcc helper linked in"; fail=1
  fi
done
[ $fail = 0 ] || exit 1
echo "checks: Win95 imports only, subsystem 4.0, i586, no libgcc helpers"

ZIP=sidblaster-win98-$VER.zip
rm -rf build/pkg && mkdir -p build/pkg/SIDBLAST
cp build/HardSID.dll build/SBTEST.EXE dist/SIDBLAST.INI dist/README.TXT build/pkg/SIDBLAST/
(cd build/pkg && rm -f "../$ZIP" && zip -qX -r "../$ZIP" SIDBLAST)
echo "packed: build/$ZIP"
unzip -l "build/$ZIP"
