#!/bin/sh
# Call counter (check mode: counts + compares) on WV2 and SD3, 6000 frames each, pokes, --no-pad; both in parallel.
export SDL_AUDIODRIVER=dummy
unset SA1_WALK_MODE SA1_DIV_COUNT
T="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/d3"
D="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/dump/sa1_us_1005/data"
E="$T/bd_div/backend/desktop/sa1psp.exe"
mkdir -p "$T/count"
for s in wv2:2:1:4 sd3:6:2:4; do
  IFS=: read n l a m <<E2
$s
E2
  pk="--poke 12:0x8C7493BA:$l:2 --poke 12:0x8C7493BC:$a:2 --poke 12:0x8C7493BE:$l:2 --poke 12:0x8C7493C0:$a:2 --poke 12:0x8C760970:$m:2"
  SA1_DIV_MODE=check "$E" "$D/1ST_READ.BIN" "$D" --no-audio --headless --hidden --no-vmu --no-pad --frames 6000 $pk \
    --log "$T/count/$n.log" > /dev/null 2>&1 &
done
wait
for n in wv2 sd3; do echo "== $n"; grep -h "SHC divide" "$T/count/$n.log" | grep -v "SHC divide: math"; done
