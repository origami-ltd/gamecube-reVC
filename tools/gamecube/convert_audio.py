#!/usr/bin/env python3
"""Vice City audio -> the console audio set the GameCube/Wii runtime plays.

    python3 convert_audio.py <GTAVC/audio dir> <output dir>

What sampman_gamecube.cpp reads (09-03 decisions, measured on the mini-DVD):

  radio (*.adf)      Ogg Vorbis 32 kHz stereo, sox -C 3 (~110 kbps). The .adf
                     files are MPEG-1 Layer III with every byte XOR'd by 0x22.
                     Tremor decodes them through the ARAM read-ahead rings.
  streams (*.mp3)    IMA ADPCM .wav at the SOURCE rate (sources above 40 kHz
                     go to 40 kHz) and channel count (ffmpeg adpcm_ima_wav,
                     1024-byte blocks). Cutscene,
                     mission and ambience tracks; no Vorbis on the main
                     thread. Where a stream and a mission .wav share a name
                     (ass_1, ass_2) the stream wins, as on the working disc.
  voice (*.wav)      copied byte for byte: already IMA ADPCM.
  sfx.raw + sfx.sdt  sfx.adp = every sample as 512-byte IMA ADPCM blocks
                     (pack_sfx_adpcm.py --all), sfx.sdt copied, sfx.raw left
                     behind (340 MB the disc cannot hold).
  lengths.cache      per-stream duration in ms, big-endian u32, in
                     StreamedNameTable order: a pressed disc is read-only, so
                     the game cannot write its own and would probe 1200 files.

ffmpeg decodes and writes the ADPCM; sox writes the Vorbis (ffmpeg builds
without libvorbis are common). Both are checked for up front.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys

ADF_XOR = 0x22
# Per-byte Python loops spend longer un-XORing a 57MB file than ffmpeg spends
# decoding it; a 256-entry translation table is the whole difference.
DEXOR_TABLE = bytes(b ^ ADF_XOR for b in range(256))
CHUNK = 1 << 22
RADIO_RATE = 32000
RADIO_QUALITY = 3
STREAM_MAX_RATE = 40000
HERE = os.path.dirname(os.path.abspath(__file__))


def need(tool):
    if shutil.which(tool) is None:
        sys.exit(f"{tool} not found on PATH; install it and re-run")


def run(cmd):
    subprocess.run(cmd, check=True)


def dexor(src, dst):
    with open(src, "rb") as f, open(dst, "wb") as o:
        while True:
            block = f.read(CHUNK)
            if not block:
                break
            o.write(block.translate(DEXOR_TABLE))


def radio(adf, ogg, tmp):
    dexor(adf, tmp)
    dec = subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-i", tmp, "-vn",
                            "-f", "wav", "-"], stdout=subprocess.PIPE)
    enc = subprocess.Popen(["sox", "-t", "wav", "-", "-C", str(RADIO_QUALITY),
                            "-r", str(RADIO_RATE), "-c", "2", ogg],
                           stdin=dec.stdout)
    dec.stdout.close()   # so the decoder sees EPIPE if the encoder dies
    enc.communicate()
    dec.wait()
    if enc.returncode or dec.returncode:
        sys.exit(f"radio convert failed: {os.path.basename(adf)}")


def source_rate(path):
    p = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                        "stream=sample_rate", "-of", "csv=p=0", path],
                       capture_output=True, text=True)
    return int(p.stdout.split()[0])


def adpcm(mp3, wav):
    # Source rate and channels, never upsampled; the four 44.1/48 kHz
    # sources go to 40 kHz, as the user asked on 09-02.
    rate = ["-ar", str(STREAM_MAX_RATE)] if source_rate(mp3) > STREAM_MAX_RATE else []
    run(["ffmpeg", "-v", "error", "-y", "-i", mp3, "-vn", "-map_metadata", "-1",
         *rate, "-c:a", "adpcm_ima_wav", wav])


def duration_ms(path):
    p = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                        "format=duration", "-of", "csv=p=0", path],
                       capture_output=True, text=True)
    try:
        return int(float(p.stdout.strip()) * 1000)
    except ValueError:
        return 0


def stream_names():
    names, in_table = [], False
    with open(os.path.join(HERE, "..", "..", "src", "audio", "sampman.h")) as f:
        for line in f:
            if line.startswith("static char StreamedNameTable"):
                in_table = True
            elif in_table:
                if line.startswith("};"):
                    break
                m = line.split('"')
                if len(m) >= 2 and m[1]:
                    names.append(m[1])
    return names


def build_lengths_cache(dst):
    out, missing = bytearray(), 0
    names = stream_names()
    for n in names:
        stem = os.path.basename(os.path.splitext(n.replace("\\", "/").lower())[0])
        ms = 0
        for ext in (".ogg", ".wav"):
            path = os.path.join(dst, stem + ext)
            if os.path.exists(path):
                ms = duration_ms(path)
                break
        missing += ms == 0
        out += struct.pack(">I", ms)
    with open(os.path.join(dst, "lengths.cache"), "wb") as f:
        f.write(out)
    print(f"lengths.cache: {len(names)} tracks, {missing} without a file")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", help="GTA Vice City audio directory")
    ap.add_argument("dst", help="output directory (becomes <disc>/audio)")
    args = ap.parse_args()
    for tool in ("ffmpeg", "ffprobe", "sox"):
        need(tool)
    os.makedirs(args.dst, exist_ok=True)

    files = sorted(os.listdir(args.src))
    lower = {f.lower(): f for f in files}
    # Voice first, so a stream of the same name overwrites it below.
    for f in files:
        if f.lower().endswith(".wav"):
            shutil.copyfile(os.path.join(args.src, f), os.path.join(args.dst, f.lower()))
    tmp = os.path.join(args.dst, ".adf.tmp.mp3")
    try:
        for f in files:
            stem, ext = os.path.splitext(f.lower())
            src = os.path.join(args.src, f)
            if ext == ".adf":
                print(f"radio  {f}", flush=True)
                radio(src, os.path.join(args.dst, stem + ".ogg"), tmp)
            elif ext == ".mp3":
                print(f"adpcm  {f}", flush=True)
                adpcm(src, os.path.join(args.dst, stem + ".wav"))
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)

    raw, sdt = lower.get("sfx.raw"), lower.get("sfx.sdt")
    if not (raw and sdt):
        sys.exit("missing sfx.raw/sfx.sdt in " + args.src)
    shutil.copyfile(os.path.join(args.src, sdt), os.path.join(args.dst, "sfx.sdt"))
    print("pack   sfx.adp (all samples)", flush=True)
    run([sys.executable, os.path.join(HERE, "pack_sfx_adpcm.py"),
         os.path.join(args.src, raw), os.path.join(args.src, sdt),
         os.path.join(args.dst, "sfx.adp"), "--all"])
    build_lengths_cache(args.dst)
    total = sum(os.path.getsize(os.path.join(args.dst, f)) for f in os.listdir(args.dst))
    print(f"audio: {total/2**20:.0f} MB")


if __name__ == "__main__":
    main()
