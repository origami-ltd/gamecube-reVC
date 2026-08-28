#!/usr/bin/env python3
"""Create a deterministic Dolphin movie for reVC traversal tests."""

import argparse
import struct
from pathlib import Path


FPS = 60
HEADER_SIZE = 256
PAD_SIZE = 8
FLIGHT_START_SECONDS = 300
FLIGHT_MOVIE_SKIP_INPUTS = 2


def pad(*, start=False, a=False, b=False, x=False, y=False, z=False,
		dpad_up=False, dpad_down=False, dpad_left=False, dpad_right=False,
		l=False, r=False, up=False, down=False, left=False, right=False,
		c_up=False, c_down=False, c_left=False, c_right=False):
	buttons0 = (int(start) | int(a) << 1 | int(b) << 2 | int(x) << 3 |
			int(y) << 4 | int(z) << 5 | int(dpad_up) << 6 |
			int(dpad_down) << 7)
	buttons1 = (int(dpad_left) | int(dpad_right) << 1 | int(l) << 2 |
			int(r) << 3 | 1 << 6)  # Connected.
	stick_x = 0 if left else 255 if right else 128
	stick_y = 0 if down else 255 if up else 128
	c_x = 0 if c_left else 255 if c_right else 128
	c_y = 0 if c_down else 255 if c_up else 128
	return bytes((buttons0, buttons1, 0, 0, stick_x, stick_y, c_x, c_y))


def make_inputs(seconds, flight=False):
	inputs = []
	for frame in range(seconds * FPS):
		if flight:
			flying = frame >= FLIGHT_START_SECONDS * FPS
			# The movie player drains the press before normal game input starts.
			# Keeping this at the start of the recording makes the long flight
			# harness independent of the opening movie's duration.
			inputs.append(pad(start=frame < FLIGHT_MOVIE_SKIP_INPUTS,
				a=flying, up=flying))
			continue
		# Press A once a second through every frontend/loading/cutscene segment.
		# The opening sequence lasts well beyond 70 seconds on GameCube; stopping
		# the pulses there left later scenes running and made the traversal test
		# look stuck with player controls intentionally disabled. Once gameplay is
		# active these harmless pulses merely make Tommy sprint.
		a = frame >= 8 * FPS and frame % FPS < 2
		up = frame >= 45 * FPS
		phase = frame % (14 * FPS)
		left = up and 6 * FPS <= phase < 7 * FPS
		right = up and 13 * FPS <= phase
		inputs.append(pad(a=a, up=up, left=left, right=right))
	return inputs


def make_header(input_count, game_id=b"GBLPGL", wii_executable=True):
	header = bytearray(HEADER_SIZE)
	header[0:4] = b"DTM\x1a"
	# The canonical disc image identifies itself as GBLPGL. Dolphin rejects a
	# movie whose six-byte id belongs to the old standalone-DOL test executable.
	header[4:10] = game_id
	header[10] = int(wii_executable)
	header[11] = 1  # GameCube controller in port 1.
	struct.pack_into("<Q", header, 13, input_count)  # frameCount
	struct.pack_into("<Q", header, 21, input_count)  # inputCount
	header[49:49 + 5] = b"Codex"
	header[81:81 + 5] = b"Metal"
	header[97:97 + 3] = b"HLE"
	# CheckInputEnd also compares emulated ticks against tickCount. A synthetic
	# boot movie has no recording clock, so zero ends playback on the first pad
	# poll. Max keeps the movie active until its input bytes are consumed.
	struct.pack_into("<Q", header, 237, (1 << 64) - 1)
	return header


def self_test():
	neutral = pad()
	assert len(neutral) == PAD_SIZE
	assert neutral == bytes((0, 1 << 6, 0, 0, 128, 128, 128, 128))
	assert pad(a=True, up=True, left=True)[0] == 1 << 1
	assert pad(a=True, up=True, left=True)[4:6] == bytes((0, 255))
	assert pad(right=True)[4] == 255
	assert pad(start=True, dpad_down=True, dpad_right=True, l=True)[:2] == bytes((0x81, 0x46))

	inputs = make_inputs(72)
	assert len(inputs) == 72 * FPS
	assert inputs[8 * FPS] == pad(a=True)
	assert inputs[8 * FPS + 2] == pad()
	assert inputs[45 * FPS + 2] == pad(up=True)
	assert inputs[48 * FPS] == pad(a=True, up=True, left=True)
	assert inputs[48 * FPS + 2] == pad(up=True, left=True)
	flight = make_inputs(FLIGHT_START_SECONDS + 1, flight=True)
	assert flight[0] == pad(start=True)
	assert flight[FLIGHT_MOVIE_SKIP_INPUTS - 1] == pad(start=True)
	assert flight[FLIGHT_MOVIE_SKIP_INPUTS] == pad()
	assert flight[FLIGHT_START_SECONDS * FPS - 1] == pad()
	assert flight[FLIGHT_START_SECONDS * FPS] == pad(a=True, up=True)

	header = make_header(len(inputs))
	assert len(header) == HEADER_SIZE
	assert header[:4] == b"DTM\x1a"
	assert header[4:10] == b"GBLPGL"
	assert header[10:12] == bytes((1, 1))
	assert struct.unpack_from("<Q", header, 13)[0] == len(inputs)
	assert struct.unpack_from("<Q", header, 21)[0] == len(inputs)
	assert struct.unpack_from("<Q", header, 237)[0] == (1 << 64) - 1
	print("DTM self-test passed")


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("output", nargs="?", type=Path)
	parser.add_argument("--seconds", type=int, default=240)
	parser.add_argument("--game-id", default="GBLPGL")
	parser.add_argument("--gamecube-disc", action="store_true")
	parser.add_argument("--flight", action="store_true",
	    help=f"hold A + stick forward after {FLIGHT_START_SECONDS}s")
	parser.add_argument("--self-test", action="store_true")
	args = parser.parse_args()
	if args.self_test:
		self_test()
		return
	if args.output is None:
		parser.error("output is required unless --self-test is used")
	if args.seconds < 1:
		parser.error("--seconds must be positive")
	if args.flight and args.seconds <= FLIGHT_START_SECONDS:
		parser.error(f"--flight requires more than {FLIGHT_START_SECONDS} seconds")
	try:
		game_id = args.game_id.encode("ascii")
	except UnicodeEncodeError:
		parser.error("--game-id must be ASCII")
	if len(game_id) != 6:
		parser.error("--game-id must be exactly six ASCII characters")

	inputs = make_inputs(args.seconds, args.flight)
	args.output.write_bytes(make_header(
		len(inputs), game_id, not args.gamecube_disc) + b"".join(inputs))
	print(f"wrote {args.output}: {len(inputs)} inputs at {FPS} Hz")


if __name__ == "__main__":
	main()
