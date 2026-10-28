#!/bin/sh
# tcmp.sh BASEDIR NEWDIR: tape_diff --exact and cmp per tape, plus the divide report lines of NEWDIR.
W="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}/private/d3/wt"
for t in sonic_ec1 sonic_to_ec1_start title_menu ec_scripted; do
  echo "$t: $(python "$W/tools/tape_diff.py" band --exact "$1/$t.tsv" "$2/$t.tsv" 2>&1 | tail -1)"
  if cmp -s "$1/$t.tsv" "$2/$t.tsv"; then echo "$t: byte-identical ($(wc -l < "$2/$t.tsv") lines)"; else echo "$t: TRACES DIFFER"; fi
  grep -h "SHC divide: math\|SHC divide: calls\|SHC divide summary" "$2/$t.log" | sed "s/^/  /"
done
