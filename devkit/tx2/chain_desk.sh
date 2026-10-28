#!/bin/sh
# tx2: does a poked stage chain hold on the desktop?  SS -> EC1 -> SS -> EC2 -> SS, by the --stage
# pokes at later frames (next level/act + game mode).  Silent, hidden.
#   sh chain_desk.sh EXE OUTDIR [BUDGET_KB]
EXE=$1; OUT=$2; BUDGET=${3:-0}
D="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/dump/sa1_us_1005/data"
mkdir -p "$OUT"
O=$(cygpath -m "$OUT")
set -- --poke 12:8c7493ba:26:2 --poke 12:8c7493bc:3:2 --poke 12:8c7493be:26:2 --poke 12:8c7493c0:3:2 --poke 12:8c760970:5:2 --no-pad --no-audio --hidden --no-vmu --frames 6200 --log "$O/chain.log" \
  --poke 1500:8c7493be:1:2 --poke 1500:8c7493c0:0:2 --poke 1500:8c760970:4:2 \
  --poke 2700:8c7493be:26:2 --poke 2700:8c7493c0:3:2 --poke 2700:8c760970:5:2 \
  --poke 3900:8c7493be:1:2 --poke 3900:8c7493c0:1:2 --poke 3900:8c760970:4:2 \
  --poke 5100:8c7493be:26:2 --poke 5100:8c7493c0:3:2 --poke 5100:8c760970:5:2 \
  --screenshot "1400:$O/f1400.ppm" --screenshot "2600:$O/f2600.ppm" --screenshot "3800:$O/f3800.ppm" \
  --screenshot "5000:$O/f5000.ppm" --screenshot "6100:$O/f6100.ppm"
if [ "$BUDGET" != 0 ]; then export SA1_TEX_BUDGET=$BUDGET; fi
SDL_AUDIODRIVER=dummy "$EXE" "$D/1ST_READ.BIN" "$D" "$@" > "$OUT/chain.out" 2>&1
echo "rc=$?"
grep -n "stage \|njInitTexture\|texpool\|no memory\|decode failed\|live," "$OUT/chain.log" | cut -c1-150 | awk 'NR<=60'
