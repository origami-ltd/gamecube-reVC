#!/usr/bin/env python3
"""Repoint one GXT key at a new string (VC format: TABL > per-table TKEY/TDAT).

    gxtpatch.py file.gxt KEY "New Text"

The new UTF-16 string is appended to the owning table's TDAT and the key's
offset repointed, so no other offset inside the table moves; every later
table's TABL offset shifts by the growth. In-place edit.
"""
import struct, sys

def patch(path, key, text):
    d = bytearray(open(path, "rb").read())
    assert d[0:4] == b"TABL", "not a VC gxt"
    tabl_size, = struct.unpack_from("<I", d, 4)
    ntab = tabl_size // 12
    tables = []
    for i in range(ntab):
        name = bytes(d[8+i*12:8+i*12+8]).split(b"\x00")[0]
        off, = struct.unpack_from("<I", d, 8+i*12+8)
        tables.append([name, off, 8+i*12+8])
    key8 = key.encode().ljust(8, b"\x00")
    for name, toff, entpos in tables:
        p = toff
        if name != b"MAIN":
            p += 8  # mission tables repeat their name first
        assert d[p:p+4] == b"TKEY", (name, d[p:p+4])
        tkey_size, = struct.unpack_from("<I", d, p+4)
        keys_at = p + 8
        tdat_at = keys_at + tkey_size
        assert d[tdat_at:tdat_at+4] == b"TDAT"
        tdat_size, = struct.unpack_from("<I", d, tdat_at+4)
        for k in range(tkey_size // 12):
            e = keys_at + k*12
            if bytes(d[e+4:e+12]) == key8:
                new = text.encode("utf-16-le") + b"\x00\x00"
                insert_at = tdat_at + 8 + tdat_size
                struct.pack_into("<I", d, e, tdat_size)      # point at old TDAT end
                struct.pack_into("<I", d, tdat_at+4, tdat_size + len(new))
                d[insert_at:insert_at] = new
                delta = len(new)
                for name2, toff2, entpos2 in tables:
                    if toff2 > toff:
                        struct.pack_into("<I", d, entpos2, toff2 + delta)
                open(path, "wb").write(d)
                print(f"{path}: {key} -> {text!r} in table {name.decode()}")
                return True
    raise SystemExit(f"{key} not found in {path}")

def add(path, key, text):
    """Insert a NEW key into MAIN (TKEY stays sorted — the game binary-searches)."""
    d = bytearray(open(path, "rb").read())
    assert d[0:4] == b"TABL"
    tabl_size, = struct.unpack_from("<I", d, 4)
    ntab = tabl_size // 12
    tables = []
    for i in range(ntab):
        name = bytes(d[8+i*12:8+i*12+8]).split(b"\x00")[0]
        off, = struct.unpack_from("<I", d, 8+i*12+8)
        tables.append([name, off, 8+i*12+8])
    key8 = key.encode().ljust(8, b"\x00")
    main = next(t for t in tables if t[0] == b"MAIN")
    toff = main[1]
    assert d[toff:toff+4] == b"TKEY"
    tkey_size, = struct.unpack_from("<I", d, toff+4)
    keys_at = toff + 8
    tdat_at = keys_at + tkey_size
    assert d[tdat_at:tdat_at+4] == b"TDAT"
    tdat_size, = struct.unpack_from("<I", d, tdat_at+4)
    entries = []
    for k in range(tkey_size // 12):
        e = keys_at + k*12
        entries.append((bytes(d[e+4:e+12]), struct.unpack_from("<I", d, e)[0]))
    if any(e[0] == key8 for e in entries):
        return patch(path, key, text)
    new_str = text.encode("utf-16-le") + b"\x00\x00"
    entries.append((key8, tdat_size))
    entries.sort(key=lambda e: e[0])
    new_tkey = b"".join(struct.pack("<I", off) + k for k, off in entries)
    body = bytearray()
    body += b"TKEY" + struct.pack("<I", len(new_tkey)) + new_tkey
    body += b"TDAT" + struct.pack("<I", tdat_size + len(new_str))
    body += d[tdat_at+8:tdat_at+8+tdat_size] + new_str
    old_table_len = 8 + tkey_size + 8 + tdat_size
    d[toff:toff+old_table_len] = body
    delta = len(body) - old_table_len
    for name2, toff2, entpos2 in tables:
        if toff2 > toff:
            struct.pack_into("<I", d, entpos2, toff2 + delta)
    open(path, "wb").write(d)
    print(f"{path}: +{key} = {text!r}")
    return True

if __name__ == "__main__":
    if sys.argv[1] == "--add":
        add(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        patch(sys.argv[1], sys.argv[2], sys.argv[3])
