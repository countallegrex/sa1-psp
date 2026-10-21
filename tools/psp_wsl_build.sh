#!/bin/sh
# Build the PSP game in a build tree on WSL's own disk, then copy the outputs
# next to the checkout (build-psp/sa1psp/, where tools/psp_run.py looks).
#
#   tools/psp_wsl_build.sh [--prx] [--dir DIR] [-DNAME=VALUE ...]
#
# Run inside WSL from the checkout (a /mnt/c path) with PSPDEV set.  A build
# tree under /mnt/c pays for every file access through the Windows file
# share: on 2026-10-21 relinking the 15 MB PRX took 8-44 s there and 0.5 s on
# ext4, a one-file rebuild 13-49 s against 3 s, a full build 38 s.  The
# sources stay on /mnt/c; only the objects and the linked images live in DIR
# (default ~/sa1-build/<checkout name>), which is configured on first use
# with SA1_RECOMP_OUT taken from the checkout's own build-psp/CMakeCache.txt
# when there is one.  -D options are passed to cmake (e.g. a code offset).
# --prx builds and copies only the PSPLink PRX (sa1psp.prx, sa1psp_prx.elf,
# sa1prof.prx): enough for psp_run.py and psp_bench.py.
#
# Part of sa1-psp (MIT).
set -eu
: "${PSPDEV:?PSPDEV is not set}"
export PATH="$PATH:$PSPDEV/bin"
SRC=$(pwd)
DIR="$HOME/sa1-build/$(basename "$SRC")"
PRX=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prx) PRX=1; shift ;;
    --dir) DIR="$2"; shift 2 ;;
    -D*) break ;;
    *) echo "psp_wsl_build: unknown option $1" >&2; exit 2 ;;
  esac
done
if [ ! -f "$DIR/CMakeCache.txt" ]; then
  out=$(sed -n 's/^SA1_RECOMP_OUT:PATH=//p' build-psp/CMakeCache.txt 2>/dev/null || true)
  cmake -G Ninja -S "$SRC" -B "$DIR" -DTARGET=psp -DCMAKE_TOOLCHAIN_FILE="$PSPDEV/psp/share/pspdev.cmake" \
    ${out:+"-DSA1_RECOMP_OUT=$out"} "$@" > /dev/null
elif [ $# -gt 0 ]; then
  cmake "$DIR" "$@" > /dev/null
fi
if [ $PRX = 1 ]; then
  cmake --build "$DIR" -j"$(nproc)" --target sa1psp_prx sa1prof
  files="sa1psp.prx sa1psp_prx.elf sa1prof.prx"
else
  cmake --build "$DIR" -j"$(nproc)"
  files="EBOOT.PBP sa1psp.elf sa1psp.prx sa1psp_prx.elf sa1prof.prx sa1exc.prx"
fi
mkdir -p build-psp/sa1psp
for f in $files; do
  [ -f "$DIR/sa1psp/$f" ] && cp "$DIR/sa1psp/$f" build-psp/sa1psp/
done
echo "psp_wsl_build: $DIR -> build-psp/sa1psp ($files)"
