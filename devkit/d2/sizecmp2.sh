#!/bin/sh
for v in b l1; do echo "$v: $(wc -l < /tmp/s_$v.txt) fns, $(awk '{s+=$2} END{print s}' /tmp/s_$v.txt) bytes"; done
echo "delta sum over common: $(awk '{s+=$1} END{print s}' /tmp/s_d.txt)"
comm -13 <(cut -d' ' -f1 /tmp/s_b.txt) <(cut -d' ' -f1 /tmp/s_l1.txt) | head -5
