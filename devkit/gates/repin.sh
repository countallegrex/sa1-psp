#!/bin/sh
# desk re-pin, no console: same function order, @ pins dropped, built, pinned to that build, built again.
#   sh repin.sh TREE_WIN NAME OUT_WSL WORK_WIN
tree=$1; name=$2; out=$3; work=$4
wsl_path() { echo "/mnt/$(echo "$1" | cut -c2)$(echo "$1" | cut -c3-)"; }
cd "$tree" || exit 1
wtree=$(wsl_path "$tree"); wwork=$(wsl_path "$work")
cp tools/psp_hot_order.txt "$work/hot_$name.orig.txt"
# drop "@ADDR" lines and " @ADDR" suffixes of "=CHUNK.c @ADDR" / "=@ADDR"
sed -e '/^@/d' -e '/^=@/d' -e 's/^\(=[^ ]*\) *@0x[0-9a-f]*.*$/\1/' "$work/hot_$name.orig.txt" > tools/psp_hot_order.txt
MSYS_NO_PATHCONV=1 wsl -u root sh "$wtree/tools/psp_lever_build.sh" "$wtree" "${name}_u" "$out" "$wwork" || exit 1
python tools/psp_hot_order.py --pin "$work/b_${name}_u/sa1psp_prx.elf" || exit 1
cp tools/psp_hot_order.txt "$work/hot_$name.repinned.txt"
MSYS_NO_PATHCONV=1 wsl -u root sh "$wtree/tools/psp_lever_build.sh" "$wtree" "$name" "$out" "$wwork" || exit 1
git checkout tools/psp_hot_order.txt
md5sum "$work/b_${name}_u/sa1psp.prx" "$work/b_$name/sa1psp.prx"
