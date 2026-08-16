#!/bin/zsh
# Walk the player around so the streamer actually has to work. Standing still
# measures nothing: the stutter and the LOD flicker are both consequences of
# models coming and going, and neither happens in an alley with the pad idle.
# Arrow keys are the main stick (Dolphin GCPadNew.ini, Quartz keyboard).
SECS=${1:-180}
osascript -e 'tell application "Dolphin" to activate' 2>/dev/null
sleep 2

# key codes: 126 up, 125 down, 123 left, 124 right
hold() {   # hold <code> <seconds>
	osascript -e "tell application \"System Events\" to key down $1"
	sleep $2
	osascript -e "tell application \"System Events\" to key up $1"
}

END=$((SECONDS + SECS))
osascript -e 'tell application "System Events" to key down 126'   # forward, held
while (( SECONDS < END )); do
	sleep 6
	# Sweep the camera through new territory rather than running a straight
	# line into a wall: turning is what pulls unloaded blocks into view.
	hold 123 1
	sleep 6
	hold 124 1
done
osascript -e 'tell application "System Events" to key up 126'
echo "drive: done"
