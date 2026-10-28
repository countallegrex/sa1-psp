#!/bin/sh
# tapes.sh EXE OUTDIR [extra args...]: the 4 tapes, silent, hidden, watch traces; 4 in parallel.
# SA1_DIV_MODE is passed through from the caller's environment (unset = the exe's default).
export SDL_AUDIODRIVER=dummy
EXE="$1"; O="$2"; shift 2
W="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/d3/wt"
D="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/dump/sa1_us_1005/data"
mkdir -p "$O"
for t in sonic_ec1 sonic_to_ec1_start title_menu ec_scripted; do
  "$EXE" "$D/1ST_READ.BIN" "$D" --hidden --headless --no-pad --no-audio --no-vmu \
    --replay "$W/tests/replay/$t.tape" --watch-list "$W/tests/replay/watch.txt" \
    --watch-trace "$O/$t.tsv" --log "$O/$t.log" "$@" > "$O/$t.out" 2>&1 &
done
wait
