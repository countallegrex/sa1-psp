#!/bin/sh
# tx2 chainrec: a poked stage chain SS -> EC1 -> SS -> EC2 -> SS -> EC3 -> SS on the desktop, the game's own way.
# Each transition pokes the next level/act the game itself uses (0x8C7493C2/C4, as fn_8c04f794 sets them)
# and the exit request 0x8C19DE24 = 3 (what fn_8c04e386(3) writes); the game state machine
# (0x8C7493B4) then fades out, copies C2/C4 into the current level/act (state 17), picks
# mode 4 or 5 from the level (fn_8c04c960) and loads it.  Silent, hidden.
#   sh chain_desk2.sh EXE OUTDIR [BUDGET_KB]
EXE=$1; OUT=$2; BUDGET=${3:-0}
D="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/dump/sa1_us_1005/data"
mkdir -p "$OUT"
O=$(cygpath -m "$OUT")
set -- --poke 12:8c7493ba:26:2 --poke 12:8c7493bc:3:2 --poke 12:8c7493be:26:2 --poke 12:8c7493c0:3:2 --poke 12:8c760970:5:2 --no-pad --no-audio --hidden --no-vmu --frames 5700 --log "$O/chain.log" \
  --poke 1500:8c7493c2:1:2  --poke 1500:8c7493c4:0:2 --poke 1500:8c19de24:3:2 \
  --poke 2200:8c7493c2:26:2 --poke 2200:8c7493c4:3:2 --poke 2200:8c19de24:3:2 \
  --poke 2900:8c7493c2:1:2  --poke 2900:8c7493c4:1:2 --poke 2900:8c19de24:3:2 \
  --poke 3600:8c7493c2:26:2 --poke 3600:8c7493c4:3:2 --poke 3600:8c19de24:3:2 \
  --poke 4300:8c7493c2:1:2  --poke 4300:8c7493c4:2:2 --poke 4300:8c19de24:3:2 \
  --poke 5000:8c7493c2:26:2 --poke 5000:8c7493c4:3:2 --poke 5000:8c19de24:3:2 \
  --screenshot "1400:$O/f1400.ppm" --screenshot "2100:$O/f2100.ppm" --screenshot "2800:$O/f2800.ppm" \
  --screenshot "3500:$O/f3500.ppm" --screenshot "4200:$O/f4200.ppm" --screenshot "4900:$O/f4900.ppm" \
  --screenshot "5600:$O/f5600.ppm"
if [ "$BUDGET" != 0 ]; then export SA1_TEX_BUDGET=$BUDGET; fi
SDL_AUDIODRIVER=dummy "$EXE" "$D/1ST_READ.BIN" "$D" "$@" > "$OUT/chain.out" 2>&1
echo "rc=$?"
grep -n "LAND_.*BIN for\|overlay: loaded\|frame limit\|trap\|no memory\|decode failed" "$OUT/chain.log" | cut -c1-120
