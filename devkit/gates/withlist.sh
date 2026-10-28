#!/bin/sh
# Build TREE's PSP PRX against a given hot list, then restore the tree's own list (git checkout).
#   sh withlist.sh TREE NAME OUT_WSL WORK LIST      (TREE, WORK, LIST: Windows or Git Bash paths)
# WORK/b_NAME receives sa1psp.prx, sa1psp_prx.elf, sa1prof.prx (tools/psp_lever_build.sh).
# WSL paths come from the Windows form (cygpath -m), never the Git Bash one (Temp is /tmp there).
wsl_path() { m=$(cygpath -m "$1"); d=$(echo "$m" | cut -c1 | tr A-Z a-z); echo "/mnt/$d$(echo "$m" | cut -c3-)"; }
tree=$1; name=$2; out=$3; work=$4; list=$5
mkdir -p "$(cygpath -u "$work")"
cd "$(cygpath -u "$tree")" || exit 1
cp "$(cygpath -u "$list")" tools/psp_hot_order.txt || exit 1
MSYS_NO_PATHCONV=1 wsl -u root sh "$(wsl_path "$tree")/tools/psp_lever_build.sh" "$(wsl_path "$tree")" "$name" "$out" "$(wsl_path "$work")"
rc=$?
git checkout tools/psp_hot_order.txt
[ -f "$(cygpath -u "$work")/b_$name/sa1psp.prx" ] || { echo "withlist: no $work/b_$name/sa1psp.prx"; exit 1; }
exit $rc
