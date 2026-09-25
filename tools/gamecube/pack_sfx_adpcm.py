#!/usr/bin/env python3
"""Resident SFX bank as IMA ADPCM for ARAM: sfx.raw + sfx.sdt -> sfx.adp

    python3 pack_sfx_adpcm.py <sfx.raw> <sfx.sdt> <sfx.adp> [--verify] [--all]

The GameCube's 16MB of ARAM holds the texel tier (12MB) and a 4MB audio
reserve; the resident bank (the first RESIDENT_SAMPLES entries, everything
below SAMPLEBANK_PED_START) is 14.3MB as 16-bit PCM and 3.6MB as 4-bit IMA
ADPCM. The PS2 release kept these samples in the SPU as 4-bit ADPCM too.
Block layout is the one sampman_gamecube.cpp already decodes for the VOICE
files (gcWavDecode): 512-byte blocks, header = predictor int16 LE + step
index u8 + pad, then nibbles low-first; the header predictor is the first
sample, so a block carries 1 + 508*2 = 1017 samples. Sample rates and loop
points stay in sfx.sdt: the decoder yields the same sample count as the PCM.

File: "GCSFXA1\\0", u32 count, u32 dataStart, then count x {u32 offset,
u32 bytes} (big-endian, offsets from file start, 32-byte aligned), then data.
"""
import os, struct, sys

MAGIC = b"GCSFXA1\0"
HEADER = struct.Struct(">8sII")
ENTRY = struct.Struct(">II")
RESIDENT_SAMPLES = 524      # SAMPLEBANK_PED_START
BLOCK = 512
ALIGN = 32

STEP = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,
        130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,
        1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,
        6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,
        29794,32767]
INDEX = [-1,-1,-1,-1,2,4,6,8]

def clamp16(v): return -32768 if v < -32768 else 32767 if v > 32767 else v

def encode_nibble(sample, pred, idx):
    step = STEP[idx]; diff = sample - pred; nib = 0
    if diff < 0: nib = 8; diff = -diff
    d = step >> 3
    if diff >= step: nib |= 4; diff -= step; d += step
    if diff >= step >> 1: nib |= 2; diff -= step >> 1; d += step >> 1
    if diff >= step >> 2: nib |= 1; d += step >> 2
    pred = clamp16(pred - d if nib & 8 else pred + d)
    idx = min(88, max(0, idx + INDEX[nib & 7]))
    return nib, pred, idx

def decode_nibble(nib, pred, idx):
    step = STEP[idx]; d = step >> 3
    if nib & 1: d += step >> 2
    if nib & 2: d += step >> 1
    if nib & 4: d += step
    if nib & 8: d = -d
    pred = clamp16(pred + d)
    idx = min(88, max(0, idx + INDEX[nib & 7]))
    return pred, idx

def encode(pcm):
    """pcm: list of int16 (mono) -> bytes of 512-byte blocks."""
    out = bytearray(); i = 0; n = len(pcm); idx = 0
    per = 1 + (BLOCK - 4) * 2
    while i < n:
        pred = pcm[i]
        blk = bytearray(BLOCK)
        struct.pack_into("<hBB", blk, 0, pred, idx, 0)
        j = i + 1; pos = 4; lo = True
        while pos < BLOCK:
            s = pcm[j] if j < n else pred
            nib, pred, idx = encode_nibble(s, pred, idx)
            if lo: blk[pos] = nib
            else: blk[pos] |= nib << 4; pos += 1
            lo = not lo; j += 1
        out += blk; i += per
    return bytes(out)

def decode(data, count):
    out = []; pos = 0
    while pos + BLOCK <= len(data) and len(out) < count:
        pred, idx, _ = struct.unpack_from("<hBB", data, pos)
        out.append(pred)
        for b in data[pos+4:pos+BLOCK]:
            for nib in (b & 15, b >> 4):
                pred, idx = decode_nibble(nib, pred, idx)
                out.append(pred)
        pos += BLOCK
    return out[:count]

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    verify = "--verify" in sys.argv
    raw_path, sdt_path, out_path = args
    sdt = open(sdt_path, "rb").read()
    samples = [struct.unpack_from("<IIIII", sdt, o) for o in range(0, len(sdt), 20)]
    # --all: every sample, resident ones first (same order as sfx.sdt); the
    # runtime keeps the first RESIDENT_SAMPLES in ARAM and freads the rest
    # (ped comments) from this file on demand, so sfx.raw leaves the disc.
    resident = samples if "--all" in sys.argv else samples[:RESIDENT_SAMPLES]
    data_start = (HEADER.size + ENTRY.size * len(resident) + ALIGN - 1) & -ALIGN
    entries = []; pcm_bytes = 0; worst = 0.0
    with open(raw_path, "rb") as raw, open(out_path, "wb+") as out:
        out.write(b"\0" * data_start)
        for i, (offset, size, rate, ls, le) in enumerate(resident):
            raw.seek(offset); pcm_raw = raw.read(size)
            pcm = list(struct.unpack("<%dh" % (size // 2), pcm_raw[: size & ~1]))
            enc = encode(pcm)
            if verify:
                dec = decode(enc, len(pcm))
                err = sum((a - b) ** 2 for a, b in zip(pcm, dec)) / max(1, len(pcm))
                sig = sum(a * a for a in pcm) / max(1, len(pcm))
                snr = 10 * __import__("math").log10(sig / err) if err and sig else 99.0
                worst = snr if i == 0 else min(worst, snr)
                if len(dec) != len(pcm):
                    raise SystemExit(f"sample {i}: decoded {len(dec)} of {len(pcm)} samples")
            pos = out.tell(); out.write(enc)
            pad = ((len(enc) + ALIGN - 1) & -ALIGN) - len(enc)
            if pad: out.write(b"\0" * pad)
            entries.append((pos, len(enc))); pcm_bytes += size
        out.seek(0); out.write(HEADER.pack(MAGIC, len(resident), data_start))
        for e in entries: out.write(ENTRY.pack(*e))
    print(f"resident bank: {len(resident)} samples, PCM {pcm_bytes//1024}K -> ADPCM {os.path.getsize(out_path)//1024}K"
          + (f", worst SNR {worst:.1f} dB" if verify else ""))

if __name__ == "__main__":
    main()
