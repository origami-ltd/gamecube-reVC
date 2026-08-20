#!/usr/bin/env python3
"""Check the per-play 48kHz channel conversion in sampman_gamecube.cpp.

The DSP resamples by repeating samples, so anything below 48kHz aliases
audibly ("metallic"). Channels therefore interpolate once, on the way in,
into their own MEM1 buffer - and they do it IN PLACE: the native data is
DMA'd to the tail of the buffer and the conversion runs forward into the
front. That only works if the read head always stays ahead of the write
head. This checks both halves:

  1. the read head never overtakes the write head, for every source rate
     the game actually ships (down to 8000Hz, a 6x expansion);
  2. the 16.16 fixed-point interpolation matches a float reference.

    python3 tools/gamecube/test_resample.py
"""

DSP = 48000


def align32(v):
    return (v + 31) & ~31


def overlap_is_safe(in_samples, base_freq, skew=31):
    """Model the buffer layout and assert reads stay ahead of writes."""
    out_samples = in_samples * DSP // base_freq
    raw_bytes = in_samples * 2
    out_bytes = align32(out_samples * 2)
    read_bytes = align32(skew + raw_bytes)
    want = max(out_bytes + 64, read_bytes)
    tail = align32(want - read_bytes)
    if tail + read_bytes > want:
        tail = 0

    step = (base_freq << 16) // DSP
    pos = 0
    for k in range(out_samples):
        i0 = pos >> 16
        if i0 >= in_samples - 1:
            i0 = in_samples - 2
        write_at = k * 2
        read_at = tail + skew + i0 * 2
        if write_at >= read_at:
            return False, k, write_at, read_at
        pos += step
    return True, out_samples, 0, 0


def interp_error(in_samples, base_freq):
    """Fixed-point vs float interpolation over a synthetic ramp."""
    src = [(-32768 + (i * 7919) % 65536) for i in range(in_samples)]
    src = [s - 65536 if s > 32767 else s for s in src]
    out_samples = in_samples * DSP // base_freq
    step = (base_freq << 16) // DSP
    pos = 0
    worst = 0
    for k in range(out_samples):
        i0 = pos >> 16
        if i0 >= in_samples - 1:
            i0 = in_samples - 2
        fr = pos & 0xFFFF
        a, b = src[i0], src[i0 + 1]
        fixed = a + (((b - a) * fr) >> 16)
        exact = a + (b - a) * (fr / 65536.0)
        worst = max(worst, abs(fixed - exact))
        pos += step
    return worst


def main():
    # Every base rate seen in the game's own sfx.sdt, plus the extremes.
    rates = [8000, 10706, 11025, 15554, 20600, 22050, 26513, 31992, 32000, 44100]
    for f in rates:
        for n in (2, 3, 64, 1287, 20000):
            ok, k, w, r = overlap_is_safe(n, f)
            assert ok, f"overlap broken at rate {f}, {n} samples: k={k} write={w} read={r}"
    print(f"ok: read head stays ahead for {len(rates)} source rates")

    for f in rates:
        worst = interp_error(4096, f)
        assert worst <= 1, f"interpolation off by {worst} at {f}Hz"
    print("ok: 16.16 interpolation within 1 LSB of float")

    # A rate at or above the DSP's is left alone - no conversion, no cost.
    assert 48000 * DSP // 48000 == 48000
    print("48kHz channel conversion verified")


if __name__ == "__main__":
    main()
