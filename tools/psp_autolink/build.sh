#!/bin/sh
# Build sa1autolink.prx with pspsdk.  Needs psp-config on PATH (PSPDEV).
# From Windows:
#   wsl -d Ubuntu -u root -- bash -l "<project-dir>/sa1-psp/tools/psp_autolink/build.sh"
# Output: <repo>/build-autolink/sa1autolink.prx (git-ignored).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT="${1:-$REPO/build-autolink}"

command -v psp-config >/dev/null || { echo "psp-config not found; set up PSPDEV" >&2; exit 1; }

mkdir -p "$OUT"
cp "$HERE/Makefile" "$HERE/sa1autolink.c" "$HERE/stubs.S" "$OUT/"
make -C "$OUT" clean >/dev/null
make -C "$OUT"
test -f "$OUT/sa1autolink.prx"
echo "built $OUT/sa1autolink.prx"
