#!/bin/sh
# .text of the div PRX vs the walker PRX; where the wrappers and the _orig bodies landed.
export PATH=$PATH:/opt/pspdev/bin
P="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private"
for e in "$P/d2/b_psp_walk/sa1psp_prx.elf" "$P/d3/psp/b_div/sa1psp_prx.elf"; do
  echo "== $e"
  psp-size -A "$e" | grep -E "^\.text "
  psp-nm -S "$e" | grep -E " fn_8c10ca5c| fn_8c10cb10"
done
