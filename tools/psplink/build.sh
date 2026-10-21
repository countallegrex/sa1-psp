#!/usr/bin/env bash
# Builds PSPLink with the sa1-psp patches (README.md here): usbhostfs.prx in WSL with
# pspdev, usbhostfs_pc.exe with a Cygwin install.  Run from Git Bash on Windows.
#
#   tools/psplink/build.sh WORKDIR OUTDIR [CYGWIN_ROOT]
#
# WORKDIR  a clone of psplinkusb is made (or reused) here, at the pinned commit, patched
# OUTDIR   gets PSPLINK2/ (the PSP folder: the stock build plus the patched usbhostfs.prx)
#          and psplinkusb-windows/ (usbhostfs_pc.exe and the Cygwin DLLs it was built with)
# CYGWIN_ROOT  default WORKDIR/../cygwin; needs gcc-core make pkg-config libusb1.0-devel
#          (setup-x86_64.exe --no-admin --quiet-mode --root DIR --packages ...)
#
# Part of sa1-psp (MIT).  PSPLink itself is BSD (pspdev/psplinkusb); only the patches live here.
set -euo pipefail

PIN=4b767c097e1f269b580758a7de0e386f5ad9a247     # master 2026-10-10, PSPLink v3.2.1
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mkdir -p "$1" && cd "$1" && pwd)"
OUT="$(mkdir -p "$2" && cd "$2" && pwd)"
CYG="${3:-$WORK/../cygwin}"

if [ ! -d "$WORK/.git" ]; then
    git clone -q https://github.com/pspdev/psplinkusb.git "$WORK"
fi
git -C "$WORK" checkout -q "$PIN"
git -C "$WORK" checkout -q -- usbhostfs/main.c usbhostfs_pc/main.c bootstrap/Makefile bootstrap/main.c psplink/shell.c
for p in "$HERE"/0*.patch; do
    git -C "$WORK" apply --whitespace=nowarn "$p"
done

# PSPLINK2's XMB icon (original art, made by the project), for 0003's pack-pbp
cp "$HERE/icon0.png" "$WORK/bootstrap/icon0.png"

winpath() { cygpath -w "$1" 2>/dev/null || echo "$1"; }
wslpath_of() { MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- wslpath -a "$(winpath "$1")" | tr -d '\r'; }

# PSP side: the whole PSPLink release (stock), then the patched usbhostfs.prx over it
W="$(wslpath_of "$WORK")"
MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -lc "set -e; cd '$W'; make -f Makefile.psp release PSP_FW_VERSION=271 >/dev/null"
mkdir -p "$OUT/PSPLINK2"
cp "$WORK"/release/psplink/* "$OUT/PSPLINK2/"
cp "$WORK/usbhostfs/usbhostfs.prx" "$OUT/PSPLINK2/"
# the card's copy has LF endings (a git checkout on Windows gives CRLF)
tr -d '\r' < "$WORK/psplink/psplink.ini" > "$OUT/PSPLINK2/psplink.ini"

# PC side
C="$(winpath "$CYG")"
"$CYG/bin/bash.exe" --norc -c "export PATH=/usr/bin:\$PATH; cd \"\$(cygpath -u '$(winpath "$WORK")')/usbhostfs_pc\" && make >/dev/null 2>&1"
mkdir -p "$OUT/psplinkusb-windows"
cp "$WORK/usbhostfs_pc/usbhostfs_pc.exe" "$OUT/psplinkusb-windows/"
for d in cygwin1.dll cygusb-1.0.dll cyggcc_s-seh-1.dll; do
    cp "$CYG/bin/$d" "$OUT/psplinkusb-windows/"
done

(cd "$OUT" && sha256sum PSPLINK2/* psplinkusb-windows/*)
