#!/usr/bin/env python3
"""Vice City audio -> Ogg Vorbis, for the GameCube SD image.

    python3 convert_audio.py <GTAVC/audio dir> <output dir>

The .adf radio files are not a format: they are ordinary MPEG-1 Layer III with
every byte XOR'd by 0x22. Undo that and ffprobe reports mp3, 128kbps, 44.1kHz
stereo — verified on emotion.adf, whose first bytes become ff fb 90 6c, a valid
frame header.

Quality is matched to the SOURCE rather than maximised blindly. Re-encoding a
128kbps MP3 at 256kbps Vorbis recovers nothing the MP3 encoder already threw
away; it only doubles the file, and the whole set has to fit a 1.5GB disc.
Vorbis at the same nominal bitrate is already perceptually ahead of MP3 at that
bitrate, so quality 4 is transparent against its own source — measured output:
44100Hz stereo, 126kbps. The loose files are 64kbps 22kHz mono and stay mono at
22kHz; upsampling would cost space for fidelity that is not in the input.

ffmpeg here decodes but does not encode Vorbis (this build ships only the
native encoder, which is experimental and worse than libvorbis), so sox does
the encoding. Both are checked for up front rather than failing halfway through
a 500MB job.
"""
import argparse
import os
import shutil
import subprocess
import sys

ADF_XOR = 0x22
# Per-byte Python loops spend longer un-XORing a 57MB file than ffmpeg spends
# decoding it; a 256-entry translation table is the whole difference.
DEXOR_TABLE = bytes(b ^ ADF_XOR for b in range(256))
CHUNK = 1 << 22


def need(tool):
    if shutil.which(tool) is None:
        sys.exit(f"{tool} not found on PATH; install it and re-run")


def dexor(src, dst):
    with open(src, "rb") as f, open(dst, "wb") as o:
        while True:
            block = f.read(CHUNK)
            if not block:
                break
            o.write(block.translate(DEXOR_TABLE))


def encode(mp3_path, ogg_path, quality):
    """Decode with ffmpeg, encode with sox, one pass through a pipe."""
    dec = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-y", "-i", mp3_path, "-vn", "-f", "wav", "-"],
        stdout=subprocess.PIPE,
    )
    enc = subprocess.Popen(
        ["sox", "-t", "wav", "-", "-C", str(quality), ogg_path],
        stdin=dec.stdout,
    )
    dec.stdout.close()   # so the decoder sees EPIPE if the encoder dies
    enc.communicate()
    dec.wait()
    if enc.returncode != 0 or dec.returncode != 0:
        raise RuntimeError(f"convert failed: {os.path.basename(mp3_path)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("src", help="GTA Vice City audio directory")
    ap.add_argument("dst", help="output directory")
    ap.add_argument("--radio-quality", type=int, default=4,
                    help="sox -C for the 128kbps radio (default 4)")
    ap.add_argument("--sfx-quality", type=int, default=1,
                    help="sox -C for the 64kbps mono files (default 1)")
    ap.add_argument("--subdir", default="audio",
                    help="output subdirectory; the game looks under audio/")
    args = ap.parse_args()

    need("ffmpeg")
    need("sox")
    # The game builds its paths from StreamedNameTable ("AUDIO\\WILD.ADF"),
    # so the output has to land in audio/ with the same base names. Keeping the
    # names rather than numbering them is what lets the backend reuse that
    # table instead of maintaining a second mapping that would drift.
    args.dst = os.path.join(args.dst, args.subdir) if args.subdir else args.dst
    os.makedirs(args.dst, exist_ok=True)

    tmp = os.path.join(args.dst, ".adf.tmp.mp3")
    done = skipped = 0
    try:
        for name in sorted(os.listdir(args.src)):
            stem, ext = os.path.splitext(name)
            ext = ext.lower()
            if ext not in (".adf", ".mp3"):
                continue
            out = os.path.join(args.dst, stem + ".ogg")
            if os.path.exists(out):
                skipped += 1
                continue
            src = os.path.join(args.src, name)
            print(f"{stem}{ext}", flush=True)
            if ext == ".adf":
                dexor(src, tmp)
                encode(tmp, out, args.radio_quality)
            else:
                encode(src, out, args.sfx_quality)
            done += 1
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)

    # sfx.raw is a raw sample bank indexed by sfx.sdt, not a container, so it
    # is copied whole: the index addresses it by byte offset, and re-encoding
    # would mean rebuilding the index too.
    for name in ("sfx.raw", "sfx.sdt"):
        src = os.path.join(args.src, name)
        dst = os.path.join(args.dst, name)
        if os.path.exists(src) and not os.path.exists(dst):
            print(f"copy {name}", flush=True)
            shutil.copy2(src, dst)

    total = sum(os.path.getsize(os.path.join(args.dst, f))
                for f in os.listdir(args.dst))
    print(f"converted {done}, skipped {skipped}, total {total/2**20:.0f} MB")


if __name__ == "__main__":
    main()
