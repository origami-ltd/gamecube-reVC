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

MINI_DVD_BYTES = 1_459_978_240

# Read straight off the card by CTxdStore rather than out of gta3.img, so
# repack_img.py never sees them.
LOOSE_TXD_DIRS = ("models", "txd")

# Controller diagrams for pads this console does not have. Frontend.cpp points
# every controller slot at FRONTEND_GCC.TXD, so none of these is ever opened —
# about 6MB, and FRONTEND_DS2.TXD is the one dictionary txdconv cannot read,
# which made shipping it a live D3D8 path in a GX-only build.
SKIP_TXD = ("frontend_ds2.txd", "frontend_ds3.txd", "frontend_ds4.txd",
            "frontend_x360.txd", "frontend_xone.txd", "frontend_nsw.txd",
            "ps3btns.txd", "x360btns.txd", "nswbtns.txd")

# The controller itself is a real DFF clump. gc_controller is its full-size
# base colour converted to a GX-native raster. The old diagram and its arrow
# overlays remain as blank compatibility slots because LoadController still
# owns those five frontend sprites on non-GC builds.
GCC_TXD_IMAGES = (("gc_controller", "gamecube_controller.tga"),
                  ("fe_controller", "fe_blank.tga"),
                  ("fe_arrows1", "fe_blank.tga"),
                  ("fe_arrows2", "fe_blank.tga"),
                  ("fe_arrows3", "fe_blank.tga"),
                  ("fe_arrows4", "fe_blank.tga"))

# These two languages come from the user's install rather than the editable
# utils/gxt sources. Add port-only labels to the staged copy so selecting one
# on an ISO cannot turn a new menu row into a blank string.
STAGED_GXT_LABELS = {
    "portuguese.gxt": (
        ("FED_WDP", "GOTAS DE CHUVA"),
        ("GCL_LFO", "L: Centralizar / clique: Olhar para trás"),
        ("GCL_RFO", "R: Mirar / clique: Atirar"),
        ("GCL_MOV", "Alavanca: Mover"),
        ("GCL_WEP", "Direcional: Armas"),
        ("GCL_SPR", "A: Correr"),
        ("GCL_JMP", "Y: Pular"),
        ("GCL_DRV", "Alavanca: Dirigir"),
        ("GCL_DPV", "Direcional: Rádio / Buzina / Missão"),
        ("GCL_LM2", "L: Freio / clique: Ré"),
        ("GCL_RM2", "R: Acelerar / clique: Máximo"),
        ("GCL_NAA", "A: Não atribuído"),
        ("GCL_NAY", "Y: Não atribuído"),
        ("GCL_LVC", "L: Olhar à esquerda / clique: Atirar"),
        ("GCL_RVC", "R: Olhar à direita / clique: Atirar"),
        ("GCL_ACC", "A: Acelerar"),
        ("GCL_BRK", "Y: Freio / Ré"),
        ("GCL_PAU", "START: Pausa"),
        ("GCL_ENT", "Z: Entrar / Sair"),
        ("GCL_CRH", "X: Agachar"),
        ("GCL_HBR", "X: Freio de mão"),
        ("GCL_ATK", "B: Atacar / Atirar"),
        ("GCL_FIR", "B: Atirar"),
        ("GCL_CAM", "C: Câmera livre / Mirar"),
    ),
    "russian.gxt": (
        ("FED_WDP", "КАПЛИ ДОЖДЯ"),
        ("GCL_LFO", "L: Центр / нажатие: взгляд назад"),
        ("GCL_RFO", "R: Прицел / нажатие: огонь"),
        ("GCL_MOV", "Стик: Движение"),
        ("GCL_WEP", "Крестовина: Оружие"),
        ("GCL_SPR", "A: Бег"),
        ("GCL_JMP", "Y: Прыжок"),
        ("GCL_DRV", "Стик: Руление"),
        ("GCL_DPV", "Крестовина: Радио / Гудок / Миссия"),
        ("GCL_LM2", "L: Тормоз / нажатие: задний ход"),
        ("GCL_RM2", "R: Газ / нажатие: максимум"),
        ("GCL_NAA", "A: Не назначено"),
        ("GCL_NAY", "Y: Не назначено"),
        ("GCL_LVC", "L: Взгляд влево / нажатие: огонь"),
        ("GCL_RVC", "R: Взгляд вправо / нажатие: огонь"),
        ("GCL_ACC", "A: Газ"),
        ("GCL_BRK", "Y: Тормоз / Задний ход"),
        ("GCL_PAU", "START: Пауза"),
        ("GCL_ENT", "Z: Войти / Выйти"),
        ("GCL_CRH", "X: Присесть"),
        ("GCL_HBR", "X: Ручной тормоз"),
        ("GCL_ATK", "B: Атака / Огонь"),
        ("GCL_FIR", "B: Огонь"),
        ("GCL_CAM", "C: Свободная камера / Прицел"),
    ),
}


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
    ap.add_argument("--theora-encoder",
                    help="Xiph encoder_example; also accepted via THEORA_ENCODER_EXAMPLE")
    ap.add_argument("--preencoded-movies",
                    help="directory containing opening.ogv and titles.ogv")
    ap.add_argument("--max-dim", type=int, help="cap texture axes, passed to txdconv")
    ap.add_argument("--size-mb", type=float,
                    default=MINI_DVD_BYTES / 1048576.0,
                    help="target disc size, for the fit report")
    ap.add_argument("--keep-sfx-raw", action="store_true",
                    help="keep the unpacked sample bank for an SD-only build")
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
                        ignore=shutil.ignore_patterns("gta3.img", "gta3.dir",
                                                     "*.bak", "._*"))

    # The stock PC movies cannot be decoded by the console backend. Transcode
    # both original opening parts to the bounded GameCube stream: Rockstar's
    # logo first, then Vice City's title montage. Missing either is fatal so a
    # release ISO cannot silently ship half the opening.
    movies_dir = next((os.path.join(args.game, name)
                       for name in os.listdir(args.game)
                       if name.lower() == "movies" and
                       os.path.isdir(os.path.join(args.game, name))), None)
    logo_movie = None
    titles_movie = None
    if movies_dir:
        logo_movie = next((os.path.join(movies_dir, name)
                           for name in os.listdir(movies_dir)
                           if name.lower() == "logo.mpg"), None)
        titles_movie = next((os.path.join(movies_dir, name)
                             for name in os.listdir(movies_dir)
                             if name.lower() == "gtatitles.mpg"), None)
    if logo_movie is None:
        sys.exit("missing opening FMV: <game>/movies/logo.mpg")
    if titles_movie is None:
        sys.exit("missing opening FMV: <game>/movies/gtatitles.mpg")
    encode_fmv = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "encode_fmv.py")
    encoder_arg = (["--encoder-example", args.theora_encoder]
                   if args.theora_encoder else [])
    # A reused staging tree must not keep the retired MPEG payloads. Besides
    # wasting disc space, their presence makes release inspection ambiguous.
    for legacy_name in ("opening.gcmv", "titles.gcmv"):
        legacy_movie = os.path.join(args.out, "movies", legacy_name)
        if os.path.exists(legacy_movie):
            os.unlink(legacy_movie)
    opening_movie = os.path.join(args.out, "movies", "opening.ogv")
    titles_output = os.path.join(args.out, "movies", "titles.ogv")
    if args.preencoded_movies:
        for name, output in (("opening.ogv", opening_movie),
                             ("titles.ogv", titles_output)):
            source = os.path.join(args.preencoded_movies, name)
            if not os.path.isfile(source):
                sys.exit("missing pre-encoded FMV: " + source)
            os.makedirs(os.path.dirname(output), exist_ok=True)
            print("reuse %s" % name, flush=True)
            if not os.path.exists(output) or not os.path.samefile(source, output):
                shutil.copy2(source, output)
    else:
        # The console plays titles.ogv FIRST and its comments define it as the
        # Rockstar logo reel; opening.ogv follows with the Vice City montage.
        # This mapping was swapped, which played the montage before the logo.
        print("encode logo FMV (titles.ogv)", flush=True)
        subprocess.run([sys.executable, encode_fmv] + encoder_arg +
                       [logo_movie, titles_output],
                       check=True)
        print("encode title-montage FMV (opening.ogv)", flush=True)
        subprocess.run([sys.executable, encode_fmv] + encoder_arg +
                       [titles_movie, opening_movie],
                       check=True)

    # Repo GXT files contain the port-specific GameCube labels. They must win
    # over the stock PC text copied above or the new keys render as blanks.
    repo_text = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "..", "..", "assets", "gamefiles", "TEXT")
    if os.path.isdir(repo_text):
        shutil.copytree(repo_text, os.path.join(args.out, "text"),
                        dirs_exist_ok=True)
    gxtpatch = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "gxtpatch.py")
    for filename, labels in STAGED_GXT_LABELS.items():
        staged_gxt = os.path.join(args.out, "text", filename)
        if os.path.isfile(staged_gxt):
            for key, text in labels:
                subprocess.run([sys.executable, gxtpatch, "--add", staged_gxt,
                                key, text], check=True)

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

    # The world archive is intentionally excluded from copytree above because
    # every embedded PC TXD must be converted before the console streams it.
    source_img = os.path.join(args.game, "models", "gta3.img")
    source_dir = os.path.join(args.game, "models", "gta3.dir")
    if not os.path.isfile(source_img) or not os.path.isfile(source_dir):
        sys.exit("missing source world archive: <game>/models/gta3.img + gta3.dir")
    staged_img = os.path.join(args.out, "models", "gta3.img")
    staged_dir = os.path.join(args.out, "models", "gta3.dir")
    next_img, next_dir = staged_img + ".new", staged_dir + ".new"
    repack = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "repack_img.py")
    cmd = [sys.executable, repack]
    if args.max_dim:
        cmd += ["--max-dim", str(args.max_dim)]
    # --static-ide-root belongs to the iso-hardening branch's repack (static
    # DFF pre-instancing); this branch's repack_img.py does not take it.
    cmd += [source_img, source_dir, next_img, next_dir, args.txdconv]
    print("repack gta3.img", flush=True)
    subprocess.run(cmd, check=True)
    os.replace(next_img, staged_img)
    os.replace(next_dir, staged_dir)

    # Keep world TXDs whole, as in the last playable 20 August build. Splitting
    # these dictionaries moved DVD reads into first render and made geometry
    # appear in front of the player. The native GX payload remains exact and
    # full-resolution; only its load boundary changes back to one TXD.
    sidecar_root = os.path.join(args.out, "models", "gctex")
    shutil.rmtree(sidecar_root, ignore_errors=True)
    os.makedirs(sidecar_root)

    # Some original DFFs deliberately reference a texture owned by a different
    # TXD. Desktop RenderWare searches globally; the compact GameCube demand
    # dictionaries need the same exact native chunks available without keeping
    # every donor dictionary resident. This shared bundle is lossless and tiny.
    from extract_txd_sidecars import (selected_texture_chunks, texture_chunks,
                                      write_bundle)
    shared_chunks = selected_texture_chunks(staged_img, staged_dir, (
        ("ocmiamistrip9", "tallhousewall3_256"),
        ("northbuild", "topwallac1_256"),
        ("dynpostbx", "white64"),
        ("lod_starsmall", "orangeLOD"),
    ))
    shared_count = write_bundle(os.path.join(sidecar_root, "shared.gtb"),
                                shared_chunks)
    print("shared: %d textures" % shared_count, flush=True)

    # DEFAULT.DAT merges these two loose dictionaries into the runtime
    # "generic" TXD. Bundle the same lossless native chunks so dictionary-only
    # plants can be released and restored when a later DFF references them.
    generic_chunks = []
    wheels_txd = os.path.join(args.out, "models", "generic", "wheels.txd")
    if not os.path.exists(wheels_txd):
        # Case-insensitive source trees (Windows/macOS VCs, or a zip that
        # uppercased the extension) keep wheels.TXD; Linux needs the exact name.
        for name in os.listdir(os.path.join(args.out, "models", "generic")):
            if name.lower() == "wheels.txd":
                wheels_txd = os.path.join(args.out, "models", "generic", name)
                break
    for loose in (wheels_txd,
                  os.path.join(args.out, "models", "generic.txd")):
        with open(loose, "rb") as source:
            generic_chunks.extend(texture_chunks(source.read()))
    generic_count = write_bundle(os.path.join(sidecar_root, "generic.gtb"),
                                 generic_chunks)
    print("generic: %d textures" % generic_count, flush=True)

    # neo/ — the neo pipeline assets (env/rim/gloss tweak tables and
    # neo.txd) ship with the reVC source, not with the PC game, which is why
    # the card kept losing them: this script only copied from the game dir.
    # Without neo/neo.txd the four pipeline rows never appear in Graphics
    # Setup at all (re3.cpp gates them on opening that file).
    repo_neo = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "..", "assets", "gamefiles", "neo")
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

    controller_dff = os.path.join(assets, "gamecube_controller.dff")
    if not os.path.isfile(controller_dff):
        sys.exit("missing converted 3D controller: " + controller_dff)
    shutil.copy2(controller_dff,
                 os.path.join(args.out, "models", "frontend_gcc.dff"))

    if args.audio and os.path.isdir(args.audio):
        dst = os.path.join(args.out, "audio")
        print("copy audio", flush=True)
        shutil.copytree(args.audio, dst, dirs_exist_ok=True)

    # The mini-DVD uses an exact lossless pack, not lower-rate audio. Verify
    # all 9,941 random-access samples before removing the 340MB raw bank.
    audio_dir = os.path.join(args.out, "audio")
    raw = os.path.join(audio_dir, "sfx.raw")
    sdt = os.path.join(audio_dir, "sfx.sdt")
    pak = os.path.join(audio_dir, "sfx.pak")
    if not args.keep_sfx_raw and os.path.isfile(raw) and os.path.isfile(sdt):
        packer = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "pack_sfx.py")
        cmd = [sys.executable, packer, raw, sdt, pak]
        if os.path.isfile(pak):
            cmd.append("--check")
        else:
            cmd.append("--verify")
        print("verify lossless sfx.pak", flush=True)
        if subprocess.run(cmd).returncode != 0:
            sys.exit("sfx.pak failed verification; refusing to remove sfx.raw")
        os.remove(raw)

    total = 0
    for root, _, files in os.walk(args.out):
        for f in files:
            total += os.path.getsize(os.path.join(root, f))
    mb = total / 1048576.0
    print("\nloose txd converted: %d, failed: %d" % (converted, failed))
    print("card contents      : %.1f MiB of %.1f MiB" % (mb, args.size_mb))
    if mb > args.size_mb:
        print("OVER BUDGET by %.1f MiB" % (mb - args.size_mb))
        return 1
    print("headroom           : %.1f MiB" % (args.size_mb - mb))
    return 0


if __name__ == "__main__":
    sys.exit(main())
