#!/bin/sh
# tx2: the poked stage chain on the PSP (chainrec recipe): SS -> EC1 -> SS -> EC2 -> SS -> EC3 -> SS, twice.
#   sh chain_psp.sh BUILDDIR TAG [CHUNKS]   -> private/tx2/psp_chain_TAG.log, shots_chain_TAG/
P="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}"
B=$1; TAG=$2; CH=${3:-6}; EXTRA=$4
set -- --stage ss --tex-chunks "$CH" --no-pace --frames 9900 --stick 150:0:-127:9700 --mem 9850
f=1500; k=0
for la in 1:0 26:3 1:1 26:3 1:2 26:3 1:0 26:3 1:1 26:3 1:2 26:3; do
    l=${la%:*}; a=${la#*:}
    set -- "$@" --poke "$f:8c7493c2:$l:2" --poke "$f:8c7493c4:$a:2" --poke "$f:8c19de24:3:2"
    s=$((f + 500))
    set -- "$@" --screenshot "$s:c$s.ppm"
    f=$((f + 700))
done
set -- "$@" $EXTRA
cd "$P/private/tx2/wt" || exit 1
rm -f "$P/sa1-psp/build-psp/sa1psp/psplink-run/"c*.ppm
python tools/psp_run.py --timeout 900 --from "$B" --build "$P/sa1-psp/build-psp/sa1psp" --out "$P/private/tx2/psp_chain_$TAG.log" -- "$@" 2>&1 | tail -2
mkdir -p "$P/private/tx2/shots_chain_$TAG"
cp "$P/sa1-psp/build-psp/sa1psp/psplink-run/"c*.ppm "$P/private/tx2/shots_chain_$TAG/" 2>/dev/null
L="$P/private/tx2/psp_chain_$TAG.log"
echo "loads: $(grep -c 'native land: native/' "$L")  no memory: $(grep -c 'no memory' "$L")  decode failed: $(grep -c 'decode failed' "$L")  no room: $(grep -c 'no room yet' "$L")  traps: $(grep -ci 'trap\|exception' "$L")"
grep "texpool (report)\|exit:\|njInitTexture dropped" "$L" | cut -c1-230
