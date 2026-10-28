#!/bin/sh
# After the maintainer's 2a47191 re-play: copy the card log off (read-only, two reads compared) and summarize it.
P="${SA1_ROOT:?set SA1_ROOT to the folder that holds sa1-psp and private}"
O="$P/private/card-log/2026-10-27/replay-2a47191"
mkdir -p "$O"
python "$P/private/livefix/tools/psplink_ping.py" 2>&1 | grep "^ping" || exit 1
python "$P/private/livefix/tools/card_get.py" "$O" ls ms0:/PSP/GAME/SA1PSP get ms0:/PSP/GAME/SA1PSP/sa1psp.log 2>&1 | grep "^card_get\|sa1psp.log\|vmu"
L="$O/sa1psp.log"
[ -f "$L" ] || { echo "no log"; exit 1; }
echo "first: $(head -1 "$L" | cut -c1-100)"
echo "last:  $(tail -1 "$L" | cut -c1-140)"
echo "frames/time: $(tail -1 "$L" | grep -o '^\[ *[0-9.]* f[0-9]*')"
echo "no memory: $(grep -c 'no memory' "$L")  decode failed: $(grep -c 'decode failed' "$L")  no room yet: $(grep -c 'no room yet' "$L")"
echo "trap/crash: $(grep -c -i 'trap\|exception\|crash' "$L")"
grep -i 'trap\|exception' "$L" | head -3 | cut -c1-160
echo "stages: $(grep -o 'stage [0-9]*[: ][0-9]*' "$L" | uniq | tr '\n' ' ' | cut -c1-200)"
echo "saves: $(grep -c -i 'vmu.*writ\|save' "$L")"
grep -i 'texpool\|pool' "$L" | tail -2 | cut -c1-160
grep -i 'snd: core\|exit:' "$L" | tail -2 | cut -c1-160
