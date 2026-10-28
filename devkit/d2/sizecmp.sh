#!/bin/sh
# per-function text size, base vs lever 1 PSP PRX
cd "${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/d2"
NM=/opt/pspdev/bin/psp-nm
for v in b l1; do
  $NM -S b_psp_$v/sa1psp_prx.elf | awk '$3 ~ /^[tT]$/ {print $4, strtonum("0x" $2)}' | sort > /tmp/s_$v.txt
done
join /tmp/s_b.txt /tmp/s_l1.txt | awk '{d = $3 - $2; t += d; if (d > 0) g += d; else s += d; print d, $1, $2, $3} END {print t, "TOTAL", g, s > "/dev/stderr"}' | sort -n > /tmp/s_d.txt
head -5 /tmp/s_d.txt; echo ...; tail -8 /tmp/s_d.txt
grep -E " (fn_8c040784|fn_8c040612|fn_8c040942|sa1_call) " /tmp/s_d.txt
wc -l < /tmp/s_b.txt
