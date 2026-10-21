#!/bin/bash
# Desktop A/B of a native HLE function against its recompiled body, in one exe:
# each tape runs twice (native, and with VAR=1), the screenshots are compared
# byte for byte.  The three tapes run in parallel.
#
#   bash tools/desk_shots.sh EXE VAR OUTDIR [DATA]
#
# VAR is the switch the wrapper reads to call fn_<addr>_orig (e.g.
# SA1_MESH_REC, SA1_COLL_REC).  DATA defaults to ../private/dump/sa1_us_1005/data.
# Prints "TAPE: N same, M DIFF" per tape; the exit status is 1 if any DIFF.
#
# Part of sa1-psp (MIT).
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EXE=$1; VAR=$2; OUT=$3
DATA=${4:-$ROOT/../private/dump/sa1_us_1005/data}
mkdir -p "$OUT"
# Windows exe under Git Bash: "FRAME:/c/..." is not path-converted, so pass C:/... forms.
if command -v cygpath > /dev/null; then OUT=$(cygpath -m "$OUT"); ROOT=$(cygpath -m "$ROOT"); DATA=$(cygpath -m "$DATA"); fi

one() {  # TAPE FRAMES...
    local tape=$1; shift
    local out="$OUT/$tape" last=0 f
    for f in "$@"; do last=$f; done
    mkdir -p "$out/nat" "$out/rec"
    for mode in nat rec; do
        local args=()
        for f in "$@"; do args+=(--screenshot "$f:$out/$mode/f$f.ppm"); done
        if [ $mode = rec ]; then env "$VAR=1" "$EXE" "$DATA/1ST_READ.BIN" "$DATA" --log "$out/$mode.log" --no-audio --hidden \
                --replay "$ROOT/tests/replay/$tape.tape" --frames $((last + 2)) "${args[@]}" > "$out/$mode.out" 2>&1 &
        else env -u "$VAR" "$EXE" "$DATA/1ST_READ.BIN" "$DATA" --log "$out/$mode.log" --no-audio --hidden \
                --replay "$ROOT/tests/replay/$tape.tape" --frames $((last + 2)) "${args[@]}" > "$out/$mode.out" 2>&1 &
        fi
    done
    wait
    local same=0 diff=0
    for f in "$@"; do
        if cmp -s "$out/nat/f$f.ppm" "$out/rec/f$f.ppm"; then same=$((same + 1)); else diff=$((diff + 1)); fi
    done
    echo "$tape: $same same, $diff DIFF" > "$out/result.txt"
}

one title_menu 500 1000 1500 2000 2500 2700 &
one sonic_to_ec1_start 1000 2000 4000 6000 8000 10000 12000 14000 16000 18000 &
one sonic_ec1 1000 3000 6000 9000 12000 15000 18000 21000 24000 &
wait
cat "$OUT"/*/result.txt
! grep -q ' [1-9][0-9]* DIFF' "$OUT"/*/result.txt
