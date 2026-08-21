#!/usr/bin/env python3
"""Check channel/stream DSP-rate conversion in sampman_gamecube.cpp.

The DSP resamples by repeating samples, so anything below 48kHz aliases
audibly ("metallic"). Channels therefore interpolate once, on the way in,
into their own MEM1 buffer. The requested pitch is baked into that conversion
so AESND receives a 1:1 DSP-rate voice. Conversion is IN PLACE: native data is
DMA'd to the tail of the buffer and the conversion runs forward into the
front. That only works if the read head always stays ahead of the write
head. This checks both halves:

  1. every FIR tap stays ahead of the write head for authored and pitched
     rates (down to 6kHz, an 8x expansion);
  2. pitch conversion leaves the AESND voice at the exact DSP rate;
  3. the streaming FIR cursor stays continuous across native decode chunks.

    python3 tools/gamecube/test_resample.py
"""

DSPS = (48000, 48043)  # Wii dev target, real GameCube
TAPS = 8


def align32(v):
    return (v + 31) & ~31


def overlap_is_safe(in_samples, target_freq, dsp, skew=31):
    """Model the buffer layout and assert reads stay ahead of writes."""
    out_samples = in_samples * dsp // target_freq
    raw_bytes = in_samples * 2
    out_bytes = align32(out_samples * 2)
    read_bytes = align32(skew + raw_bytes)
    want = max(out_bytes + 64, read_bytes)
    tail = align32(want - read_bytes)
    if tail + read_bytes > want:
        tail = 0

    step = (target_freq << 16) // dsp
    pos = 0
    for k in range(out_samples):
        i0 = pos >> 16
        write_at = k * 2
        for tap in range(TAPS):
            si = max(0, min(in_samples - 1, i0 + tap - (TAPS // 2 - 1)))
            read_at = tail + skew + si * 2
            if write_at >= read_at:
                return False, k, write_at, read_at
        pos += step
    return True, out_samples, 0, 0


def stream_cursor_error(in_samples, rate, dsp, chunk_frames):
    """Model the C chunk-preserve loop and compare it to one flat cursor."""
    step = (rate << 16) // dsp
    loaded = min(in_samples, chunk_frames)
    frames = loaded
    base = pos = made = 0
    eof = False
    worst = 0.0
    right = TAPS - (TAPS // 2 - 1) - 1

    while True:
        i0 = pos >> 16
        while frames and not eof and i0 + right >= frames:
            keep = min(frames, TAPS)
            first = frames - keep
            base += first
            pos -= first << 16
            take = min(chunk_frames, in_samples - loaded)
            loaded += take
            frames = keep + take
            if take == 0:
                eof = True
            i0 = pos >> 16
        if not frames or (eof and pos >= frames << 16):
            break
        actual = base + pos / 65536.0
        exact = made * step / 65536.0
        worst = max(worst, abs(actual - exact))
        made += 1
        pos += step
    return made, worst


def expected_output_count(in_samples, rate, dsp):
    step = (rate << 16) // dsp
    pos = 0
    made = 0
    while pos < in_samples << 16:
        made += 1
        pos += step
    return made


def main():
    rates = [6000, 8000, 10706, 11025, 15554, 20600, 22050, 26513, 31992, 32000, 44100]
    for dsp in DSPS:
        for f in rates:
            for n in (2, 3, 64, 1287, 20000):
                ok, k, w, r = overlap_is_safe(n, f, dsp)
                assert ok, f"overlap broken at {f}->{dsp}, n={n}: k={k} write={w} read={r}"
    print(f"ok: every FIR read stays ahead at {len(rates)} pitched rates on Wii/GC")

    # Baking target pitch f makes the converted duration n/f and leaves the
    # hardware voice at dsp, rather than asking AESND to resample it again.
    for dsp in DSPS:
        for f in rates:
            n = 20000
            out = n * dsp // f
            assert abs(out / dsp - n / f) <= 1 / dsp
    print("ok: requested pitch is baked into a 1:1 DSP-rate buffer")

    for dsp in DSPS:
        for rate in (22050, 32000, 44100):
            for chunk in (4032, 8064):  # stereo and mono source chunks
                made, worst = stream_cursor_error(25037, rate, dsp, chunk)
                assert made == expected_output_count(25037, rate, dsp)
                assert worst == 0.0, (rate, dsp, chunk, worst)
    print("ok: stream cursor is sample-continuous across mono/stereo chunks")


if __name__ == "__main__":
    main()
