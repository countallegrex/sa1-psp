#!/bin/sh
# Headless timing ABBA x3, WV2 and SD3 (pokes), 6000 frames each, sequential, silent.
# n = bd_div native (default); x = the same exe with --math-exact 8c10ca5c,8c10cb10.
export SDL_AUDIODRIVER=dummy
unset SA1_DIV_MODE SA1_WALK_MODE SA1_DIV_COUNT
T="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/d3"
D="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/dump/sa1_us_1005/data"
E="$T/bd_div/backend/desktop/sa1psp.exe"
O="$T/tm"; mkdir -p "$O"; : > "$O/times.txt"
for rep in 1 2 3; do
  for b in n x x n; do
    case $b in n) X="";; x) X="--math-exact 8c10ca5c,8c10cb10";; esac
    for s in wv2:2:1:4 sd3:6:2:4; do
      IFS=: read n l a m <<E2
$s
E2
      pk="--poke 12:0x8C7493BA:$l:2 --poke 12:0x8C7493BC:$a:2 --poke 12:0x8C7493BE:$l:2 --poke 12:0x8C7493C0:$a:2 --poke 12:0x8C760970:$m:2"
      t0=$(date +%s.%N)
      "$E" "$D/1ST_READ.BIN" "$D" --no-audio --headless --hidden --no-vmu --no-pad --frames 6000 $pk $X \
        --log "$O/$n.$b.log" > /dev/null 2>&1
      t1=$(date +%s.%N)
      echo "$rep $b $n $(python -c "print(round($t1 - $t0, 3))")" | tee -a "$O/times.txt"
    done
  done
done
python - "$O/times.txt" <<'EOF'
import statistics, sys
r = {}
for ln in open(sys.argv[1]):
    _, b, n, t = ln.split(); r.setdefault((n, b), []).append(float(t))
for n in ("wv2", "sd3"):
    a, x = statistics.median(r[(n, "n")]), statistics.median(r[(n, "x")])
    print("%s native %.3f  kill switch %.3f  native vs kill %+.2f %%" % (n, a, x, 100 * (a - x) / x))
EOF
