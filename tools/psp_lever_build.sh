#!/bin/sh
# Build one PSP lever variant in WSL into its own folder.
#
#   sh tools/psp_lever_build.sh TREE NAME RECOMP_OUT WORK [EXTRA_CFLAGS]
#
# TREE is the source checkout (a WSL path), RECOMP_OUT the emitted recompiler
# output it builds against, WORK the folder that receives WORK/b_NAME
# (sa1psp.prx, sa1psp_prx.elf, sa1prof.prx) and WORK/bb_NAME.log.  Every NAME
# gets a fresh CMake folder /root/sa1-build/g_NAME; EXTRA_CFLAGS are appended
# to the toolchain's C flags.  Prints one line: the time and the text size.
#
# Part of sa1-psp (MIT).
export PSPDEV=/opt/pspdev
export PATH=$PATH:$PSPDEV/bin
tree=$1; name=$2; out=$3; work=$4; extra=$5
cd "$tree" || exit 1
mkdir -p "$work"
log="$work/bb_$name.log"
s=$(date +%s)
if [ -n "$extra" ]; then
    sh tools/psp_wsl_build.sh --prx --dir "/root/sa1-build/g_$name" "-DSA1_RECOMP_OUT=$out" \
        "-DCMAKE_C_FLAGS=-I$PSPDEV/psp/include -I$PSPDEV/psp/sdk/include -DPSP $extra" > "$log" 2>&1
else
    sh tools/psp_wsl_build.sh --prx --dir "/root/sa1-build/g_$name" "-DSA1_RECOMP_OUT=$out" > "$log" 2>&1
fi || { grep -E "error|Error" "$log" | head; exit 1; }
mkdir -p "$work/b_$name"
cp build-psp/sa1psp/sa1psp.prx build-psp/sa1psp/sa1psp_prx.elf build-psp/sa1psp/sa1prof.prx "$work/b_$name/"
echo "b_$name built in $(( $(date +%s) - s )) s, text $(psp-size "$work/b_$name/sa1psp_prx.elf" | awk 'NR==2{print $1}')"
