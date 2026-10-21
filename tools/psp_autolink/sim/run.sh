#!/bin/sh
# Host simulation of sa1autolink.c's logic: fake clock, fake ms0, fake
# loadexec (harness.c, headers in mock/).  Runs the crash-loop back-off,
# validation, holds, cancels, log cap and corrupt-state cases under ASan and
# UBSan.  Needs a host gcc (WSL: bash -l tools/psp_autolink/sim/run.sh).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d)
gcc -O1 -g -Wall -Wextra -Wno-unused-const-variable -fsanitize=address,undefined \
    -I"$HERE/mock" -c "$HERE/../sa1autolink.c" -o "$OUT/plugin.o"
gcc -O1 -g -Wall -fsanitize=address,undefined -I"$HERE" -c "$HERE/harness.c" -o "$OUT/harness.o"
gcc -fsanitize=address,undefined "$OUT/plugin.o" "$OUT/harness.o" -o "$OUT/sim"
"$OUT/sim"
