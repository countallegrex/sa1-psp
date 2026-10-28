#!/bin/sh
# --dump byte-compare of a WV2 run (pokes, --no-pad) at frames 1000, 3000, 6000: bd_div native vs the same exe
# with --math-exact 8c10ca5c,8c10cb10.  Both arms --strict (no host time in guest time; without it the same arm
# differs from itself in a few timer words) and pin the other gated routines exact.
export SDL_AUDIODRIVER=dummy
unset SA1_DIV_MODE SA1_WALK_MODE SA1_DIV_COUNT
T="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/d3"
D="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/dump/sa1_us_1005/data"
E="$T/bd_div/backend/desktop/sa1psp.exe"
PIN="8c029b00,8c029400,8c091380,8c639c70"
pk="--poke 12:0x8C7493BA:2:2 --poke 12:0x8C7493BC:1:2 --poke 12:0x8C7493BE:2:2 --poke 12:0x8C7493C0:1:2 --poke 12:0x8C760970:4:2"
mkdir -p "$T/dumps"; cd "$T/dumps"
for b in n x; do
  case $b in n) X="--math-exact $PIN";; x) X="--math-exact $PIN,8c10ca5c,8c10cb10";; esac
  "$E" "$D/1ST_READ.BIN" "$D" --hidden --headless --no-pad --no-audio --no-vmu --strict $pk $X \
    --dump 1000:d1000.$b.bin --dump 3000:d3000.$b.bin --dump 6000:d6000.$b.bin \
    --frames 6001 --log dump.$b.log > dump.$b.out 2>&1 &
done
wait
for f in 1000 3000 6000; do
  if cmp -s d$f.n.bin d$f.x.bin; then echo "frame $f: identical ($(stat -c %s d$f.n.bin) bytes)"; else echo "frame $f: DIFFER"; cmp -l d$f.n.bin d$f.x.bin | wc -l; fi
done
grep -h "SHC divide: math\|SHC divide: calls" dump.n.log dump.x.log
