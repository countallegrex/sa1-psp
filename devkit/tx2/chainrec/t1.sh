#!/bin/sh
# t1: one transition SS -> EC1 via next level c2/c4 + exit request de24
EXE="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/tx2/desk_base/sa1psp.exe"
D="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/dump/sa1_us_1005/data"
OUT=$1; shift
mkdir -p "$OUT"; O=$(cygpath -m "$OUT")
SDL_AUDIODRIVER=dummy "$EXE" "$D/1ST_READ.BIN" "$D" --poke 12:8c7493ba:26:2 --poke 12:8c7493bc:3:2 --poke 12:8c7493be:26:2 --poke 12:8c7493c0:3:2 --poke 12:8c760970:5:2 --no-pad --no-audio --hidden --no-vmu --log "$O/run.log" "$@" > "$OUT/run.out" 2>&1
echo "rc=$?"
grep -n "LAND_\|overlay: loaded\|stage " "$OUT/run.log" | cut -c1-140
