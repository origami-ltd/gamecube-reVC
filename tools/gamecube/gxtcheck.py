#!/usr/bin/env python3
"""Validate a VC .gxt with the SAME rules as the game's CText::DecodeGxtFile
(src/text/Text.cpp): TABL directory, per-table TKEY/TDAT chunk walk with the
4-alignment tail rule, strictly strcmp-ascending keys, terminated strings,
in-range even offsets. Run this before shipping any patched gxt — the game
answers a bad file by refusing to boot.

    gxtcheck.py file.gxt [more.gxt ...]
"""
import struct, sys

def fail(path, why):
    print(f"{path}: FAIL — {why}")
    return False

def check_section(d, start, end, named, path, label):
    cur = start
    if named:
        if end - cur < 8:
            return fail(path, f"{label}: no room for table name")
        cur += 8
    keys = text = None
    key_size = text_size = 0
    while cur < end:
        rem = end - cur
        if rem < 8:
            if rem != 2 or (cur & 3) != 2 or (end & 3) != 0:
                return fail(path, f"{label}: bad tail rem={rem} cur&3={cur&3} end&3={end&3}")
            if d[cur:end] != b"\x00\x00":
                return fail(path, f"{label}: nonzero tail")
            break
        hdr = d[cur:cur+4]
        size, = struct.unpack_from("<I", d, cur+4)
        cur += 8
        if size > end - cur:
            return fail(path, f"{label}: chunk {hdr} overruns table")
        if hdr == b"TKEY":
            if keys is not None: return fail(path, f"{label}: two TKEY")
            keys, key_size = cur, size
        elif hdr == b"TDAT":
            if text is not None: return fail(path, f"{label}: two TDAT")
            text, text_size = cur, size
        else:
            return fail(path, f"{label}: unknown chunk {hdr!r} at {cur-8}")
        cur += size
    if keys is None or text is None:
        return fail(path, f"{label}: missing TKEY or TDAT")
    if key_size % 12 or text_size % 2:
        return fail(path, f"{label}: bad sizes key={key_size} text={text_size}")
    last_term = None
    for off in range(0, text_size, 2):
        if d[text+off] == 0 and d[text+off+1] == 0:
            last_term = off
    if key_size and last_term is None:
        return fail(path, f"{label}: TDAT has no terminator")
    prev = None
    for i in range(key_size // 12):
        e = keys + i*12
        off, = struct.unpack_from("<I", d, e)
        key = d[e+4:e+12]
        if 0 not in key:
            return fail(path, f"{label}: unterminated key at {i}")
        name = key.split(b"\x00")[0]
        if (off & 1) or off > last_term or off + 2 > text_size:
            return fail(path, f"{label}: key {name} bad offset {off}")
        if prev is not None and prev >= name.ljust(8, b"\x00")[:8]:
            return fail(path, f"{label}: keys not ascending at {name}")
        prev = name.ljust(8, b"\x00")[:8]
    return True

def check(path):
    d = open(path, "rb").read()
    if len(d) < 8 or d[0:4] != b"TABL":
        return fail(path, "no TABL")
    tabl, = struct.unpack_from("<I", d, 4)
    if tabl % 12 or tabl > len(d) - 8:
        return fail(path, "bad TABL size")
    tables = []
    for i in range(tabl // 12):
        name = d[8+i*12:8+i*12+8].split(b"\x00")[0]
        off, = struct.unpack_from("<I", d, 8+i*12+8)
        tables.append((name, off))
    for i, (name, off) in enumerate(tables):
        end = tables[i+1][1] if i+1 < len(tables) else len(d)
        if off >= len(d) or end > len(d) or off >= end:
            return fail(path, f"table {name}: bad bounds {off}..{end}")
        if not check_section(d, off, end, name != b"MAIN", path, name.decode()):
            return False
    print(f"{path}: OK ({len(tables)} tables, {len(d)} bytes)")
    return True

if __name__ == "__main__":
    ok = all(check(p) for p in sys.argv[1:])
    sys.exit(0 if ok else 1)
