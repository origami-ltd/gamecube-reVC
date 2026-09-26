#!/usr/bin/env python3
"""One-command builds for macOS, Linux and Windows.

    python3 build.py            # GameCube DOL (build/cube/src/reVC.dol)
    python3 build.py iso        # DOL + disc -> build/release/reVC-GameCube.iso

iso reads your Vice City install from assets/GTAVC (or --game) and needs
xorriso, ffmpeg and sox on PATH; Dockerfile has everything.
Needs a devkitPro install with the GameCube toolchain (see README).
"""
import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.abspath(__file__))
DKP_GROUPS = ["gamecube-dev"]


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


def github_latest_asset(repo, match):
    with urllib.request.urlopen(
            f"https://api.github.com/repos/{repo}/releases/latest") as r:
        release = json.load(r)
    for asset in release["assets"]:
        if match in asset["name"]:
            return asset["name"], asset["browser_download_url"]
    sys.exit(f"no release asset matching '{match}' in {repo}")


def download(url, name):
    path = os.path.join(tempfile.gettempdir(), name)
    print(f"+ download {url}")
    urllib.request.urlretrieve(url, path)
    return path


def dkp_install_groups(pacman="dkp-pacman", sudo=True):
    cmd = (["sudo"] if sudo else []) + [pacman, "-Sy", "--noconfirm",
                                        "--needed"] + DKP_GROUPS
    run(cmd)


def setup_macos():
    if shutil.which("brew"):
        run(["brew", "install", "--quiet", "cmake", "ninja"])
    else:
        print("Homebrew not found; install cmake and ninja yourself.")
    if not shutil.which("dkp-pacman"):
        name, url = github_latest_asset("devkitPro/pacman", ".pkg")
        pkg = download(url, name)
        run(["sudo", "installer", "-pkg", pkg, "-target", "/"])
    dkp_install_groups()


def setup_linux():
    if shutil.which("apt-get"):
        run(["sudo", "apt-get", "install", "-y", "cmake", "ninja-build",
             "wget"])
        if not shutil.which("dkp-pacman"):
            script = download(
                "https://apt.devkitpro.org/install-devkitpro-pacman",
                "install-devkitpro-pacman")
            os.chmod(script, 0o755)
            run(["sudo", "bash", script])
        dkp_install_groups()
    elif shutil.which("pacman"):
        run(["sudo", "pacman", "-S", "--needed", "--noconfirm", "cmake",
             "ninja"])
        pacman = "dkp-pacman" if shutil.which("dkp-pacman") else "pacman"
        if pacman == "pacman":
            print("Add the devkitPro repositories to /etc/pacman.conf first "
                  "if this fails: https://devkitpro.org/wiki/devkitPro_pacman")
        dkp_install_groups(pacman)
    else:
        sys.exit("Neither apt-get nor pacman found; install devkitPro "
                 "manually: https://devkitpro.org/wiki/Getting_Started")


def setup_windows():
    if shutil.which("winget"):
        for pkg in ("Kitware.CMake", "Ninja-build.Ninja"):
            subprocess.run(["winget", "install", "-e", "--id", pkg,
                            "--accept-package-agreements",
                            "--accept-source-agreements"])
    else:
        print("winget not found; install cmake and ninja yourself.")
    if not os.path.isdir("C:/devkitPro"):
        name, url = github_latest_asset("devkitPro/installer", ".exe")
        exe = download(url, name)
        print("Launching the devkitPro installer — select the GameCube "
              "development packages.")
        os.startfile(exe)  # noqa: attribute exists on Windows
    else:
        print("devkitPro found at C:/devkitPro; run the devkitPro updater "
              "to add gamecube-dev if it is missing.")


def setup():
    system = platform.system()
    try:
        if system == "Darwin":
            setup_macos()
        elif system == "Linux":
            setup_linux()
        elif system == "Windows":
            setup_windows()
        else:
            sys.exit(f"unsupported OS: {system}")
    except (subprocess.CalledProcessError, OSError) as error:
        sys.exit(f"setup step failed ({error}); the README lists the manual "
                 "installation steps for every OS.")
    print("\nSetup done. Now run: python3 build.py")


def find_devkitpro():
    for candidate in (os.environ.get("DEVKITPRO"), "/opt/devkitpro",
                      "C:/devkitPro", "C:\\devkitPro"):
        if candidate and os.path.isfile(
                os.path.join(candidate, "cmake", "ogc-common.cmake")):
            return candidate.replace("\\", "/")
    sys.exit("devkitPro not found. Install it (with the GameCube packages) "
             "and/or set the DEVKITPRO environment variable.")


def find_tool(name, dkp):
    tool = shutil.which(name)
    if tool:
        return tool
    bundled = os.path.join(dkp, "tools", "bin", name)
    for path in (bundled, bundled + ".exe"):
        if os.path.isfile(path):
            return path
    sys.exit(f"{name} not found on PATH; install it or add it to PATH.")


def build(dkp, cmake, ninja):
    toolchain = f"{dkp}/cmake/GameCube.cmake"
    build_dir = os.path.join(ROOT, "build", "cube")
    os.makedirs(build_dir, exist_ok=True)
    env = dict(os.environ, DEVKITPRO=dkp)
    if not os.path.isfile(os.path.join(build_dir, "build.ninja")):
        subprocess.run([
            cmake, "-G", "Ninja", "-S", ROOT, "-B", build_dir,
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
            "-DLIBRW_PLATFORM=GAMECUBE",
            "-DREVC_THEORA_ROOT=" + os.path.join(ROOT, "vendor", "portlibs",
                                                 "ppc"),
        ], check=True, env=env)
    subprocess.run([ninja, "-C", build_dir], check=True, env=env)
    dol = os.path.join(build_dir, "src", "reVC.dol")
    print(f"\n  cube: {dol}")


def build_txdconv():
    """Host-compile the ahead-of-time texture converter against librw."""
    host_dir = os.path.join(ROOT, "build", "host")
    librw = os.path.join(ROOT, "vendor", "librw")
    cmake = shutil.which("cmake") or sys.exit("cmake not found")
    ninja = shutil.which("ninja") or sys.exit("ninja not found")
    if not os.path.isfile(os.path.join(host_dir, "build.ninja")):
        run([cmake, "-G", "Ninja", "-S", librw, "-B", host_dir,
             "-DCMAKE_BUILD_TYPE=Release", "-DLIBRW_PLATFORM=NULL",
             "-DLIBRW_TOOLS=OFF", "-DLIBRW_INSTALL=OFF"])
    run([ninja, "-C", host_dir])
    exe = os.path.join(host_dir, "txdconv")
    src = os.path.join(ROOT, "tools", "gamecube", "txdconv.cpp")
    lib = None
    for cand in ("src/librw.a", "librw.a", "src/librw.lib"):
        if os.path.isfile(os.path.join(host_dir, cand)):
            lib = os.path.join(host_dir, cand)
            break
    if lib is None:
        sys.exit("host librw static library not found under build/host")
    if (not os.path.isfile(exe) or
            os.path.getmtime(exe) < os.path.getmtime(src)):
        cxx = (os.environ.get("CXX") or shutil.which("c++") or
               shutil.which("g++") or shutil.which("clang++"))
        if not cxx:
            sys.exit("no host C++ compiler found (set CXX)")
        run([cxx, "-O2", "-std=c++14", src, f"-I{librw}", lib, "-o", exe])
    return exe


TOOLS = os.path.join(ROOT, "tools", "gamecube")


def tool(name, *args):
    run([sys.executable, os.path.join(TOOLS, name), *args])


def link_tree(src, dst):
    """Hard-link copy: the disc roots are ~1.4GB and never edited in place."""
    def link(s, d):
        try:
            os.link(s, d)
        except OSError:
            shutil.copy2(s, d)
    shutil.copytree(src, dst, copy_function=link, dirs_exist_ok=True)


def opening_movies(args, game):
    """opening.ogv + titles.ogv: pre-encoded ones if given, else encoded once
    from the PC movies into build/assets/movies (minutes of Theora)."""
    names = ("opening.ogv", "titles.ogv")
    have = lambda d: all(os.path.isfile(os.path.join(d, m)) for m in names)
    given = args.movies or os.path.join(ROOT, "assets", "movies")
    if have(given):
        return given
    cache = os.path.join(ROOT, "build", "assets", "movies")
    if not have(cache):
        src = os.path.join(game, "movies")
        pc = {f.lower(): os.path.join(src, f) for f in os.listdir(src)}
        # titles.ogv plays first: the Rockstar logo reel; opening.ogv is the
        # Vice City title montage.
        for mpg, ogv in (("logo.mpg", "titles.ogv"), ("gtatitles.mpg", "opening.ogv")):
            if mpg not in pc:
                sys.exit(f"missing opening FMV: {src}/{mpg}")
            tool("encode_fmv.py", pc[mpg], os.path.join(cache, ogv))
    return cache


def disc_root(args, txdconv):
    """GTAVC -> the disc root, under build/assets/gamecube/root.

    Textures of 256 and up at half unless that erases thin detail (txdconv
    --adaptive, user 09-26), the rest full size with DXT1 moved to CMPR bit
    for bit, CI8 palettes and the shared texel pool (both need the ARAM
    tier), native map/vehicle/pad geometry, console audio, the movies.
    """
    game = os.path.abspath(args.game or os.path.join(ROOT, "assets", "GTAVC"))
    if not os.path.isdir(os.path.join(game, "models")):
        sys.exit(f"game data not found at {game}; copy your Vice City "
                 "install there or pass --game (see assets/README.md)")
    work = os.path.join(ROOT, "build", "assets", "gamecube")
    root = os.path.join(work, "root")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)

    cmd = ["--game", game, "--out", root, "--txdconv", txdconv, "--gamefiles",
           os.path.abspath(args.gamefiles or os.path.join(ROOT, "assets", "gamefiles")),
           "--adaptive", "0.70"]
    tool("build_sd.py", *cmd, "--preencoded-movies", opening_movies(args, game))

    # Converted once and kept across builds: minutes of ffmpeg/sox.
    audio = os.path.join(ROOT, "build", "assets", "audio")
    if not os.path.isfile(os.path.join(audio, "lengths.cache")):
        shutil.rmtree(audio, ignore_errors=True)
        tool("convert_audio.py", os.path.join(game, "audio"), audio + ".tmp")
        os.replace(audio + ".tmp", audio)
    link_tree(audio, os.path.join(root, "audio"))

    models = os.path.join(root, "models")
    img, dir_ = os.path.join(models, "gta3.img"), os.path.join(models, "gta3.dir")
    ci8 = [os.path.join(work, "ci8.img"), os.path.join(work, "ci8.dir")]
    pool = [os.path.join(work, "pool.img"), os.path.join(work, "pool.dir")]
    tool("ci8img.py", img, dir_, *ci8)
    tool("sharedpool.py", *ci8, *pool, os.path.join(models, "shared.txd"),
         "--pool-kb", "2048")
    os.replace(pool[0], img)
    os.replace(pool[1], dir_)
    for f in ci8:
        os.remove(f)
    # Seek locality on the disc: each TXD next to its models, groups in map
    # order. Byte-identical entries, new order and lowercase names.
    layout = os.path.join(work, "layout")
    tool("layout_img.py", "--root", root, "--out", layout,
         "--report", os.path.join(work, "layout.json"))
    os.replace(os.path.join(layout, "gta3.img"), img)
    os.replace(os.path.join(layout, "gta3.dir"), dir_)
    shutil.rmtree(layout)
    native = os.path.join(work, "native")
    tool("nativeimg.py", "--root", root, "--out", native)
    for f in ("gta3.img", "gta3.dir", "frontend_gcc.dff"):
        if os.path.isfile(os.path.join(native, f)):
            os.replace(os.path.join(native, f), os.path.join(models, f))
    shutil.rmtree(native)
    return root


def package_iso(root, out_dir):
    iso = os.path.join(out_dir, "reVC-GameCube.iso")
    tool("build_iso.py", "--root", root, "--dol",
         os.path.join(ROOT, "build", "cube", "src", "reVC.dol"), "--out", iso)
    print(f"\n  GameCube ISO: {iso}")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", nargs="?", default="cube",
                        choices=("cube", "iso"))
    parser.add_argument("--game", help="Vice City install "
                        "(default: assets/GTAVC)")
    parser.add_argument("--gamefiles", help="reVC gamefiles: TEXT, neo, models, "
                        "data (default: assets/gamefiles)")
    parser.add_argument("--out", help="release output directory "
                        "(default: build/release)")
    parser.add_argument("--movies", help="pre-encoded opening.ogv + titles.ogv "
                        "(default: assets/movies if present; otherwise the "
                        "PC movies are encoded, which needs libtheora's "
                        "encoder_example)")
    parser.add_argument("--setup", action="store_true",
                        help="install the build dependencies for this OS "
                             "(brew / apt / pacman / winget + devkitPro)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        assert callable(build) and callable(setup) and ROOT
        print("build.py self-test passed")
        return
    if args.setup:
        setup()
        return
    dkp = find_devkitpro()
    cmake = find_tool("cmake", dkp)
    ninja = find_tool("ninja", dkp)
    build(dkp, cmake, ninja)
    if args.target != "iso":
        return
    out_dir = os.path.abspath(args.out or os.path.join(ROOT, "build", "release"))
    os.makedirs(out_dir, exist_ok=True)
    package_iso(disc_root(args, build_txdconv()), out_dir)


if __name__ == "__main__":
    main()
