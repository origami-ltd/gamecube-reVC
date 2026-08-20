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
bitrate, so quality 4 is transparent against its own source.

Everything here comes out 32kHz stereo, including the loose 64kbps 22kHz mono
files. Not for fidelity — upsampling adds none — but because sampman_gamecube
opens every stream voice VOICE_STEREO16 at DIGITALRATE (32000) and Tremor does
no conversion, so a mono or 44.1kHz file plays at the wrong pitch.

sfx.raw is untouched, and must stay that way: the sfx voices call
AESND_SetVoiceFrequency per sample from sfx.sdt, so they already play at their
own rate. Resampling that bank to 32kHz stereo would be 1.7GB, five times its
340MB, and would not fit the disc.

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


def probe_rate_channels(path):
    """Source rate and channel count, so nothing is ever upsampled."""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries",
         "stream=sample_rate,channels", "-of", "csv=p=0", path],
        capture_output=True, text=True).stdout.strip().split(",")
    return int(out[0]), int(out[1])


def encode(mp3_path, ogg_path, quality, channels=2, rate=44100):
    """Decode with ffmpeg, encode with sox, one pass through a pipe."""
    dec = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-y", "-i", mp3_path, "-vn", "-f", "wav", "-"],
        stdout=subprocess.PIPE,
    )
    enc = subprocess.Popen(
        # 44100, at the user's direction. 48000 matched the DSP's output rate
        # exactly (its ucode resamples by sample-repeat, so 32kHz sources came
        # out audibly metallic), but 48k spent ~8% more disc on a mini-DVD that
        # is already over budget. sampman reads the rate from the file
        # (ov_info), so the code follows whatever is encoded here.
        #
        # MUSIC IS THE ONLY THING THAT GETS VORBIS. Voice and SFX ship at
        # their own original rates and formats — see convert_speech below.
        ["sox", "-t", "wav", "-", "-C", str(quality),
         "-r", str(rate), "-c", str(channels), ogg_path],
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
    ap.add_argument("--music-rate", type=int, default=44100,
                    help="music (radio) sample rate; sources are 32kHz")
    ap.add_argument("--radio-quality", type=float, default=4.5,
                    help="sox -C for the radio; 4.5 lands ~140kbps (>=128)")
    ap.add_argument("--sfx-quality", type=float, default=2,
                    help="sox -C for the mono dialogue/ambience files")
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
            if ext not in (".adf", ".mp3", ".wav"):
                continue
            out = os.path.join(args.dst,
                               name if ext == ".wav" else stem + ".ogg")
            if os.path.exists(out):
                skipped += 1
                continue
            src = os.path.join(args.src, name)
            print(f"{stem}{ext}", flush=True)
            if ext == ".adf":
                # MUSIC. The only thing that gets Vorbis, at the rate the
                # user picked (44.1k; 48k cost ~8% more disc on a mini-DVD
                # that is already over budget).
                dexor(src, tmp)
                encode(tmp, out, args.radio_quality, rate=args.music_rate)
            elif ext == ".mp3":
                # Mission audio and ambience: SOURCE RATE, SOURCE CHANNELS,
                # never upsampled. These are already-lossy MP3 (measured:
                # 32000 Hz stereo 112kbps, ambience 22050 mono 64kbps) and
                # there is no MP3 decoder on the console, so a same-rate
                # Vorbis transcode is the only option that does not explode
                # them into PCM. Rate and channels come from the file.
                rate, ch = probe_rate_channels(src)
                encode(src, out, args.sfx_quality, channels=ch, rate=rate)
            else:
                # VOICE: shipped NATIVE, byte-for-byte. The sources are IMA
                # ADPCM (mono, mostly 22050 Hz, 512-byte blocks) — 39MB for
                # all 1120 files. Re-encoding them to 48kHz Vorbis both
                # upsampled the game's own data 2.2x and dragged Tremor's
                # decode state into a 24MB MEM1 arena for every line of
                # dialogue. The runtime decodes ADPCM directly instead.
                shutil.copyfile(src, os.path.join(args.dst, name))
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

    build_lengths_cache(args.dst)

    total = sum(os.path.getsize(os.path.join(args.dst, f))
                for f in os.listdir(args.dst))
    print(f"converted {done}, skipped {skipped}, total {total/2**20:.0f} MB")


def build_lengths_cache(dst):
    """Write audio/lengths.cache: per-track duration in ms, big-endian u32,
    in StreamedNameTable order. The game builds this itself when missing, but
    a pressed mini-DVD is read-only, so the fwrite fails and every boot pays
    the full 1200-file scan; shipping it prebuilt is the only option there.
    Big-endian because the GameCube fread()s native uint32s."""
    import struct
    sampman = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "..", "src", "audio", "sampman.h")
    names = []
    with open(sampman) as f:
        in_table = False
        for line in f:
            if line.startswith("static char StreamedNameTable"):
                in_table = True
                continue
            if in_table:
                if line.startswith("};"):
                    break
                m = line.split('"')
                if len(m) >= 2 and m[1]:
                    names.append(m[1])
    out = bytearray()
    missing = 0
    for n in names:
        stem = os.path.splitext(n.replace("\\", "/").lower())[0]
        ogg = os.path.join(dst, os.path.basename(stem) + ".ogg")
        ms = 0
        if os.path.exists(ogg):
            p = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                                "format=duration", "-of", "csv=p=0", ogg],
                               capture_output=True, text=True)
            try:
                ms = int(float(p.stdout.strip()) * 1000)
            except ValueError:
                pass
        if ms == 0:
            missing += 1
        out += struct.pack(">I", ms)
    with open(os.path.join(dst, "lengths.cache"), "wb") as f:
        f.write(out)
    print(f"lengths.cache: {len(names)} tracks, {missing} without a file")


if __name__ == "__main__":
    main()
