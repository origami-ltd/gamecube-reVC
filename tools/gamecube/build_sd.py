#!/usr/bin/env python3
"""Assemble the SD/disc image for the GameCube port.

    python3 build_sd.py --game ~/GTAVC --out ~/revc-sd [--audio ~/revc-audio-ogg]
                        --txdconv /path/to/txdconv [--size-mb 1500]

Why this exists rather than a copy: the loose texture dictionaries were being
shipped unconverted, and that is a silent failure.

repack_img.py converts every .txd inside gta3.img, but the game also reads
dictionaries straight off the card — particle.txd, hud.txd, fonts.txd,
generic.txd, misc.txd — and those were byte-identical to the PC originals.
The GX backend rejects anything that is not PLATFORM_GAMECUBE and returns nil
(gxraster.cpp readNativeTexture), so every texture in them simply does not
exist at runtime, with no error anywhere: gpWatersprayRaster stays null and the
hydrant spray never draws, and the same goes for the rest of the particles.

Nothing crashes, which is exactly why it went unnoticed. The rejection is
logged to dvd:/native.log, and an earlier session read that log being empty as
"nothing was rejected" — it meant the reader had not run yet.
"""
import argparse
import os
import shutil
import subprocess
import sys

# Read straight off the card by CTxdStore rather than out of gta3.img, so
# repack_img.py never sees them.
LOOSE_TXD_DIRS = ("models", "txd")


def convert_txd(txdconv, src, dst, max_dim=None):
    cmd = [txdconv]
    if max_dim:
        cmd += ["--max-dim", str(max_dim)]
    cmd += [src, dst]
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return r.returncode == 0 and os.path.exists(dst) and os.path.getsize(dst) > 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True, help="Vice City install directory")
    ap.add_argument("--out", required=True, help="staging directory for the card")
    ap.add_argument("--txdconv", required=True)
    ap.add_argument("--audio", help="converted audio directory (from convert_audio.py)")
    ap.add_argument("--max-dim", type=int, help="cap texture axes, passed to txdconv")
    ap.add_argument("--size-mb", type=int, default=1500,
                    help="target disc size, for the fit report")
    args = ap.parse_args()

    if not os.path.isdir(args.game):
        sys.exit("game directory not found: " + args.game)
    os.makedirs(args.out, exist_ok=True)

    # Everything the game reads. gta3.img is NOT copied here — repack_img.py
    # produces the converted archive separately, and copying the PC one would
    # quietly overwrite it.
    for name in ("anim", "data", "text", "txd", "models"):
        src = os.path.join(args.game, name)
        if not os.path.exists(src):
            continue
        dst = os.path.join(args.out, name)
        print("copy %s" % name, flush=True)
        shutil.copytree(src, dst, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("gta3.img", "gta3.dir"))

    converted = failed = 0
    for sub in LOOSE_TXD_DIRS:
        d = os.path.join(args.out, sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.lower().endswith(".txd"):
                continue
            path = os.path.join(d, name)
            tmp = path + ".gx"
            if convert_txd(args.txdconv, path, tmp, args.max_dim):
                os.replace(tmp, path)
                converted += 1
            else:
                # Left as the PC original deliberately, and reported: shipping
                # it means that dictionary's textures will be missing, which is
                # worth knowing before the card is written rather than after.
                if os.path.exists(tmp):
                    os.remove(tmp)
                failed += 1
                print("  FAILED %s/%s — its textures will be missing" % (sub, name))

    if args.audio and os.path.isdir(args.audio):
        dst = os.path.join(args.out, "audio")
        print("copy audio", flush=True)
        shutil.copytree(args.audio, dst, dirs_exist_ok=True)

    total = 0
    for root, _, files in os.walk(args.out):
        for f in files:
            total += os.path.getsize(os.path.join(root, f))
    mb = total / 1048576.0
    print("\nloose txd converted: %d, failed: %d" % (converted, failed))
    print("card contents      : %.0f MB of %d MB" % (mb, args.size_mb))
    if mb > args.size_mb:
        print("OVER BUDGET by %.0f MB" % (mb - args.size_mb))
        return 1
    print("headroom           : %.0f MB" % (args.size_mb - mb))
    return 0


if __name__ == "__main__":
    sys.exit(main())
