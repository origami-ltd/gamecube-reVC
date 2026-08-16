// Contract check for the int16 quantiser gxPackGeometry uses (gxraster.cpp).
// A quantiser is the kind of thing that corrupts silently: get the shift wrong
// by one and every vertex lands at half or twice its coordinate, which reads on
// screen as a model that is subtly the wrong size rather than as an error.
//
//   cc -O2 -o packtest packtest.c && ./packtest
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int
shiftFor(float maxAbs, int floorShift)
{
	if(maxAbs <= 0.0f)
		return 15;
	int s = 0;
	while(s < 15 && maxAbs*(float)(1 << (s+1)) <= 32767.0f)
		s++;
	return s < floorShift ? -1 : s;
}

static int16_t
quant(float v, int shift)
{
	float q = v*(float)(1 << shift);
	if(q > 32767.0f) return 32767;
	if(q < -32768.0f) return -32768;
	return (int16_t)(q < 0.0f ? q - 0.5f : q + 0.5f);
}

int
main(void)
{
	// The shift must be the largest that still holds the extent, and holding
	// it is what stops a vertex wrapping to the opposite corner of the model.
	for(float m = 0.01f; m < 20000.0f; m *= 1.7f){
		int s = shiftFor(m, 0);
		assert(s >= 0 && s <= 15);
		assert(m*(float)(1 << s) <= 32767.0f);
		assert(s == 15 || m*(float)(1 << (s+1)) > 32767.0f);
		assert(quant(m, s) <= 32767 && quant(-m, s) >= -32768);
	}

	// Round trip stays inside half a quantum, at both ends of the range.
	for(float m = 0.5f; m < 4000.0f; m *= 3.1f){
		int s = shiftFor(m, 0);
		float quantum = 1.0f/(float)(1 << s);
		for(float t = -1.0f; t <= 1.0f; t += 0.03125f){
			float v = m*t;
			float back = quant(v, s)/(float)(1 << s);
			assert(fabsf(back - v) <= quantum*0.5f + 1e-6f);
		}
	}

	// A model reaching 128 units lands on 1/128 unit, the quantum
	// COMPRESSED_COL_VECTORS already uses for every collision vertex; twice
	// the extent costs exactly one bit of it.
	assert(shiftFor(128.0f, 3) == 7);
	assert(shiftFor(256.0f, 3) == 6);
	// Refusal below the floor is the safety valve: too coarse must come back
	// negative so the geometry keeps its float arrays rather than being
	// quantised badly.
	assert(shiftFor(100000.0f, 3) < 0);
	assert(shiftFor(64.0f, 9) < 0);   // uv floor rejects heavy tiling
	// Ordinary 0..1 uv gets 1/16384, not 1/32768: 32768 is one past int16.
	assert(shiftFor(1.0f, 9) == 14);

	printf("packtest: ok\n");
	return 0;
}
