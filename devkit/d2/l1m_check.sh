#!/bin/sh
export SDL_AUDIODRIVER=dummy   # every desktop run is silent (the maintainer, 10-28)
bdir() { [ "$1" = b ] && echo bd_fix || echo bd_l1m; }
# lever 1 (push/pop-matched preserved): 4 tapes base vs l1 (watch traces, tape_diff --exact),
# then headless timing ABBA x3 on 4 places (pokes), 6000 frames each.
T="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/d2"
R="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/sa1-psp"
D="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/dump/sa1_us_1005/data"
mkdir -p "$T/tp_mb" "$T/tp_ml1" "$T/tmm"
for b in b l1; do
  for t in sonic_ec1 sonic_to_ec1_start title_menu ec_scripted; do
    "$T/$(bdir $b)/backend/desktop/sa1psp.exe" "$D/1ST_READ.BIN" "$D" --hidden --headless --no-pad --no-audio --no-vmu \
      --replay "$R/tests/replay/$t.tape" --watch-list "$R/tests/replay/watch.txt" \
      --watch-trace "$T/tp_m$b/$t.tsv" --log "$T/tp_m$b/$t.log" > "$T/tp_m$b/$t.out" 2>&1 &
  done
done
wait
for t in sonic_ec1 sonic_to_ec1_start title_menu ec_scripted; do
  echo "$t: $(python "$R/tools/tape_diff.py" band --exact "$T/tp_mb/$t.tsv" "$T/tp_ml1/$t.tsv" 2>&1 | tail -1)"
  cmp -s "$T/tp_mb/$t.tsv" "$T/tp_ml1/$t.tsv" && echo "$t: traces byte-identical"
done
[ "$1" = notime ] && exit 0
for rep in 1 2 3; do
  for b in b l1 l1 b; do
    for s in chaos0:15:0:4 casino2:9:1:4 sd3:6:2:4 ec1:1:0:4; do
      IFS=: read n l a m <<E2
$s
E2
      pk="--poke 12:0x8C7493BA:$l:2 --poke 12:0x8C7493BC:$a:2 --poke 12:0x8C7493BE:$l:2 --poke 12:0x8C7493C0:$a:2 --poke 12:0x8C760970:$m:2"
      t0=$(date +%s.%N)
      "$T/$(bdir $b)/backend/desktop/sa1psp.exe" "$D/1ST_READ.BIN" "$D" --no-audio --headless --hidden --no-vmu --no-pad --frames 6000 $pk \
        --log "$T/tmm/$n.$b.log" > /dev/null 2>&1
      t1=$(date +%s.%N)
      echo "$rep $b $n $(python -c "print(round($t1 - $t0, 3))")" | tee -a "$T/tmm/times.txt"
    done
  done
done
