#!/bin/sh
# Regenerate tools/psp_hot_order.txt from fresh console profiles, end to end.
#
#   sh tools/psp_hot_regen.sh BASE NAME WORK [STAGES]
#
# Run from Git Bash on Windows, in the checkout (the PSP on PSPLink, pspdev in
# WSL).  BASE is the build folder to profile (sa1psp.prx, sa1psp_prx.elf,
# sa1prof.prx: a build of this checkout with the current list), WORK the
# folder for the profiles and builds, STAGES the --stage values profiled
# (default "ss ec"; their samples are summed, so the longer-frame stage
# weighs more).  Steps:
#   1. profile BASE on every stage, frames 200-700, frame skip off
#      (WORK/prof_NAME_STAGE.bin/.log; existing ones are reused);
#   2. tools/psp_hot_order.py writes the list from them, unpinned;
#   3. builds it (WORK/b_NAME_u);
#   4. pins the list to that build (--pin);
#   5. builds the pinned list (WORK/b_NAME), which is what lands, and says
#      whether it matches b_NAME_u (pinning a build's own list should change
#      nothing in it).
# Then bench BASE against WORK/b_NAME (tools/psp_bench.py, ss and ec) and
# check the pictures are byte-identical: a new layout is not always faster.
# A file WORK/STOP is checked before every console run.  The PSPLink host is
# never stopped (killing a connected host wedges the PSP until a power-cycle).
# build-psp/sa1psp holds b_NAME afterwards.
#
# Part of sa1-psp (MIT).
base=$1; name=$2; work=$3; stages=${4:-"ss ec"}
[ -n "$base" ] && [ -n "$name" ] && [ -n "$work" ] || { sed -n 3,4p "$0"; exit 2; }
cd "$(dirname "$0")/.." || exit 1
mkdir -p "$work"
run=build-psp/sa1psp/psplink-run
wsl_path() { p=$(cd "$1" && pwd -W 2>/dev/null || pwd); echo "/mnt/$(echo "$p" | cut -c1 | tr A-Z a-z)$(echo "$p" | cut -c3-)"; }
tree=$(wsl_path .)
wwork=$(wsl_path "$work")
out=$(sed -n 's/^SA1_RECOMP_OUT:PATH=//p' build-psp/CMakeCache.txt)
[ -n "$out" ] || out="$tree/recomp/out"
stop() { [ -f "$work/STOP" ] && { echo "psp_hot_regen: STOP"; exit 0; }; }

args=""
for st in $stages; do
    p="prof_${name}_$st"
    args="$args --pcs $work/$p.bin --log $work/$p.log"
    [ -f "$work/$p.bin" ] && [ -f "$work/$p.log" ] && { echo "psp_hot_regen: reusing $p"; continue; }
    stop
    python tools/psp_run.py --from "$base" -- --stage "$st" --no-pace --frames 720 --frame-skip off \
        --prof "200:700:$p.bin" > "$work/$p.out" 2>&1
    cp -f "$run/sa1psp.log" "$work/$p.log" && cp -f "$run/$p.bin" "$work/$p.bin" \
        || { echo "psp_hot_regen: profile $st failed (see $work/$p.out)"; exit 1; }
    echo "psp_hot_regen: profiled $st"
done
# shellcheck disable=SC2086
python tools/psp_hot_order.py $args --elf "$base/sa1psp_prx.elf" --out tools/psp_hot_order.txt || exit 1
MSYS_NO_PATHCONV=1 wsl -u root sh "$tree/tools/psp_lever_build.sh" "$tree" "${name}_u" "$out" "$wwork" || exit 1
python tools/psp_hot_order.py --pin "$work/b_${name}_u/sa1psp_prx.elf" || exit 1
MSYS_NO_PATHCONV=1 wsl -u root sh "$tree/tools/psp_lever_build.sh" "$tree" "$name" "$out" "$wwork" || exit 1
a=$(md5sum < "$work/b_${name}_u/sa1psp.prx" | cut -c1-12)
b=$(md5sum < "$work/b_$name/sa1psp.prx" | cut -c1-12)
if [ "$a" = "$b" ]; then echo "psp_hot_regen: b_$name = b_${name}_u ($b)"; else echo "psp_hot_regen: b_$name ($b) differs from b_${name}_u ($a)"; fi
