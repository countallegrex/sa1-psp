#!/bin/sh
# Base (d2 walk/bd) and bd_div in native (default), exact (kill switch) and check modes; 4 tapes each, 4 at a time.
T="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/d3"
B="$T/../d2/walk/bd/backend/desktop/sa1psp.exe"
N="$T/bd_div/backend/desktop/sa1psp.exe"
unset SA1_DIV_MODE SA1_WALK_MODE SA1_DIV_COUNT
cd "$T"
sh tapes.sh "$B" tp_base
sh tapes.sh "$N" tp_native
sh tapes.sh "$N" tp_exact --math-exact 8c10ca5c,8c10cb10
SA1_DIV_MODE=check sh tapes.sh "$N" tp_check
for m in native exact check; do echo "== $m"; sh tcmp.sh tp_base tp_$m; done
echo TAPES DONE
