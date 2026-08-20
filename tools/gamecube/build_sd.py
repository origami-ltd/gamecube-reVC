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

# Controller diagrams for pads this console does not have. Frontend.cpp points
# every controller slot at FRONTEND_GCC.TXD, so none of these is ever opened —
# about 6MB, and FRONTEND_DS2.TXD is the one dictionary txdconv cannot read,
# which made shipping it a live D3D8 path in a GX-only build.
SKIP_TXD = ("frontend_ds2.txd", "frontend_ds3.txd", "frontend_ds4.txd",
            "frontend_x360.txd", "frontend_xone.txd", "frontend_nsw.txd")

# fe_arrows1..4 are the highlight overlays drawn on top of the pad. Blank
# rather than absent: CSprite2d::Draw on a sprite with no texture paints an
# untextured quad, which would cover the diagram it is meant to annotate.
GCC_TXD_IMAGES = (("fe_controller", "fe_controller.tga"),
                  ("fe_arrows1", "fe_blank.tga"),
                  ("fe_arrows2", "fe_blank.tga"),
                  ("fe_arrows3", "fe_blank.tga"),
                  ("fe_arrows4", "fe_blank.tga"))


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
            if name.lower() in SKIP_TXD:
                os.remove(path)
                continue
            # Already GX, and rebuilt below: feeding it back through txdconv
            # only produces a "cannot read" on a re-run into the same staging
            # directory.
            if name.lower() == "frontend_gcc.txd":
                continue
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

    # neo/ — the neo pipeline assets (env/rim/gloss tweak tables and
    # neo.txd) ship with the reVC source, not with the PC game, which is why
    # the card kept losing them: this script only copied from the game dir.
    # Without neo/neo.txd the four pipeline rows never appear in Graphics
    # Setup at all (re3.cpp gates them on opening that file).
    repo_neo = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "..", "gamefiles", "neo")
    if os.path.isdir(repo_neo):
        print("copy neo", flush=True)
        shutil.copytree(repo_neo, os.path.join(args.out, "neo"),
                        dirs_exist_ok=True)
        # neo.txd is an RW 3.5 D3D8 dictionary; the console's reader refuses
        # it the same way the host's does, so it ships GX-converted (txdconv
        # grew a manual walker for exactly this file).
        neo_txd = os.path.join(args.out, "neo", "neo.txd")
        if convert_txd(args.txdconv, neo_txd, neo_txd + ".gx"):
            os.replace(neo_txd + ".gx", neo_txd)
        else:
            if os.path.exists(neo_txd + ".gx"):
                os.remove(neo_txd + ".gx")
            print("  FAILED neo/neo.txd — gloss/lightmap textures will be missing")

    # The GameCube pad diagram, built from loose TGAs because no PC-side
    # dictionary contains one.
    assets = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets")
    gcc = os.path.join(args.out, "models", "frontend_gcc.txd")
    cmd = [args.txdconv]
    for texname, img in GCC_TXD_IMAGES:
        cmd += ["--image", "%s=%s" % (texname, os.path.join(assets, img))]
    cmd.append(gcc)
    print("build frontend_gcc.txd", flush=True)
    if subprocess.run(cmd, stdout=subprocess.DEVNULL).returncode != 0:
        sys.exit("frontend_gcc.txd failed; the frontend would draw an "
                 "untextured quad where the pad goes")

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
