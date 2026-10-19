#!/bin/sh
# Names host addresses from a PSP crash: the "(host)" lines of a
# backtrace in sa1psp.log (backend/psp/crash_psp.c), or the PC and RA that
# PPSSPP's log or psplink print for an exception.  The EBOOT is a plain ELF
# loaded at its link address (0x08804000), so the addresses are the ELF's.
#
#   sh tools/psp_addr2line.sh 0x0880a1c4 0x0912b3f0 ...
#   sh tools/psp_addr2line.sh < sa1psp.log      (every 0x08xxxxxx/0x09xxxxxx word in it)
#
# Needs pspdev (psp-addr2line) and the unstripped build-psp/sa1psp/sa1psp.elf
# of the same build.  Recompiled functions are named fn_8cXXXXXX (guest
# address), HLE bodies sdk_8cXXXXXX.
set -eu
: "${PSPDEV:?PSPDEV is not set}"
ELF="${SA1_PSP_ELF:-build-psp/sa1psp/sa1psp.elf}"
test -f "$ELF" || { echo "no $ELF (build the PSP target first)" >&2; exit 1; }
if [ $# -gt 0 ]; then
  "$PSPDEV/bin/psp-addr2line" -f -C -p -e "$ELF" "$@"
else
  grep -oE '0x0[89][0-9a-fA-F]{6}' | sort -u | xargs -r "$PSPDEV/bin/psp-addr2line" -f -C -p -e "$ELF"
fi
