#!/bin/zsh
# Boot reVC in Dolphin and capture the USB Gecko log to a file.
# Logs land in the current directory (run this from a scratchpad, not the repo).
OUT="${OUT:-$PWD}"
SD="$HOME/Library/Application Support/Dolphin/Load/WiiSD.raw"
DOL="$(cd "$(dirname "$0")/../.." && pwd)/build/wii/src/reVC.dol"

rm -f "$OUT/gecko.log" "$OUT/dolphin.log" "$OUT/fsck.log"

# SIGTERM is not enough — a surviving instance keeps port 55020 bound and the
# next Dolphin logs "Failed to bind listener socket", losing the whole capture.
pkill -9 -x Dolphin 2>/dev/null
pkill -9 -f "Dolphin.app/Contents/MacOS/Dolphin" 2>/dev/null
for i in $(seq 1 30); do
	pgrep -x Dolphin >/dev/null || lsof -nP -iTCP:55020 >/dev/null 2>&1 || break
	sleep 1
done
sleep 3
fsck_msdos -y "$SD" > "$OUT/fsck.log" 2>&1

/Applications/Dolphin.app/Contents/MacOS/Dolphin -e "$DOL" > "$OUT/dolphin.log" 2>&1 &
echo "dolphin pid $!" >> "$OUT/fsck.log"

# Dolphin opens the gecko listener when emulation starts; keep reconnecting
# so a dropped/late connection does not lose the whole capture.
for i in $(seq 1 200); do
	nc -w 600 localhost 55020 >> "$OUT/gecko.log" 2>/dev/null
	pgrep -x Dolphin > /dev/null || break
	sleep 2
done
