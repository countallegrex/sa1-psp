#!/bin/sh
export SDL_AUDIODRIVER=dummy   # every desktop run is silent (the maintainer, 10-28)
# D2 census: which static-TRS (0x40) nodes reach fn_8c040784's stack-angle rotate.
T="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/d2"
R="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/sa1-psp"
D="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/dump/sa1_us_1005/data"
E="$T/bd/backend/desktop/sa1psp.exe"
for t in sonic_ec1 sonic_to_ec1_start title_menu ec_scripted; do
  D2_OUT="$T/census_$t.txt" "$E" "$D/1ST_READ.BIN" "$D" --hidden --no-pad --no-audio --no-vmu \
    --replay "$R/tests/replay/$t.tape" --log "$T/$t.log" > "$T/$t.out" 2>&1 &
done
wait
for t in sonic_ec1 sonic_to_ec1_start title_menu ec_scripted; do echo "$t: $(head -1 "$T/census_$t.txt" 2>/dev/null)"; done
