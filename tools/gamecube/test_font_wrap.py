#!/usr/bin/env python3
"""Check that CFont's line-measuring loops terminate on an unfittable word.

CFont::GetNumberLines and CFont::GetTextRect (src/renderer/Font.cpp) wrap by
resetting x and advancing y WITHOUT advancing the string pointer. A word wider
than the wrap box therefore re-tests the same condition forever and the game
hangs with the GP idle and no crash log — which is what the debug HUD hit: its
background box is ~24 units wide, and a stale frame period made the fps number
seven digits.

The fix mirrors the guard PrintString already had: only wrap when the line
already holds something. This models both loops.

    python3 tools/gamecube/test_font_wrap.py
"""

LIMIT = 10_000  # iterations before we call it a hang


def measure(words, widths, wrap, xstart, guard):
    """Model the loop. Returns the line count, or None if it never terminates."""
    x, lines, i, steps = xstart, 0, 0, 0
    while i < len(words):
        steps += 1
        if steps > LIMIT:
            return None
        too_wide = x + widths[i] > wrap
        if too_wide and (not guard or x > xstart):
            x = xstart          # wrap: y advances, i does NOT
            lines += 1
            continue
        x += widths[i]
        i += 1
    return lines + 1


def main():
    xstart, wrap = 8.0, 32.0        # the HUD's snug box: 24 units of room

    # "60" fits: both versions agree and terminate.
    for guard in (False, True):
        assert measure(["60"], [14.0], wrap, xstart, guard) == 1

    # "1000000" does not fit in 24 units.
    assert measure(["1000000"], [49.0], wrap, xstart, False) is None, \
        "unguarded loop was expected to hang"
    assert measure(["1000000"], [49.0], wrap, xstart, True) == 1, \
        "guarded loop must consume the oversized word and finish"

    # A long word after a short one still wraps once, then fits.
    assert measure(["ar", "1000000"], [10.0, 49.0], wrap, xstart, True) == 2

    # Words that do fit keep wrapping exactly as before the guard: four 9-unit
    # words into a 22-unit line hold two per line, so two lines.
    words = ["sim1", "rnd2", "sky3", "fx4"]
    widths = [9.0] * 4
    assert measure(words, widths, 30.0, 8.0, True) == 2
    assert measure(words, widths, 30.0, 8.0, False) == 2, \
        "the guard must not change wrapping for text that fits"

    print("ok: guarded wrap terminates on unfittable words; unguarded hangs")


if __name__ == "__main__":
    main()
