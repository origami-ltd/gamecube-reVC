#!/usr/bin/env python3
"""One-command DOL build for macOS, Linux and Windows.

    python3 build.py            # GameCube DOL (build/cube/src/reVC.dol)
    python3 build.py wii        # Wii dev DOL (build/wii/src/reVC.dol)
    python3 build.py all        # both

Needs a devkitPro install with the GameCube/Wii toolchains (see README).
Everything else the build needs ships in this repository.
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
DKP_GROUPS = ["gamecube-dev", "wii-dev"]


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
        print("Launching the devkitPro installer — select the GameCube and "
              "Wii development packages.")
        os.startfile(exe)  # noqa: attribute exists on Windows
    else:
        print("devkitPro found at C:/devkitPro; run the devkitPro updater "
              "to add gamecube-dev and wii-dev if they are missing.")


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
    sys.exit("devkitPro not found. Install it (with GameCube/Wii packages) "
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


def build(target, dkp, cmake, ninja):
    if target == "cube":
        toolchain = f"{dkp}/cmake/GameCube.cmake"
    else:
        toolchain = os.path.join(ROOT, "vendor", "portlibs", "cmake",
                                 "Wii.cmake")
    build_dir = os.path.join(ROOT, "build", target)
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
    print(f"\n  {target}: {dol}")


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


def build_sd(args):
    """Drive tools/gamecube/build_sd.py with assets/ conventions."""
    game = args.game or os.path.join(ROOT, "assets", "GTAVC")
    if not os.path.isdir(game):
        sys.exit(f"game data not found at {game}; copy your Vice City "
                 "install there or pass --game (see assets/README.md)")
    out = args.out or os.path.join(ROOT, "assets", "sd-tree")
    cmd = [sys.executable,
           os.path.join(ROOT, "tools", "gamecube", "build_sd.py"),
           "--game", game, "--out", out,
           "--txdconv", build_txdconv(), "--keep-sfx-raw"]
    audio = args.audio or os.path.join(ROOT, "assets", "audio-ogg")
    if os.path.isdir(audio):
        cmd += ["--audio", audio]
    movies = args.movies or os.path.join(ROOT, "assets", "movies")
    if os.path.isdir(movies):
        cmd += ["--preencoded-movies", movies]
    run(cmd)
    print(f"\n  SD card tree: {out}  (copy its CONTENTS to the card root)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", nargs="?", default="cube",
                        choices=("cube", "wii", "all", "sd"))
    parser.add_argument("--game", help="Vice City install "
                        "(default: assets/GTAVC)")
    parser.add_argument("--out", help="SD tree output "
                        "(default: assets/sd-tree)")
    parser.add_argument("--audio", help="converted audio dir "
                        "(default: assets/audio-ogg if present)")
    parser.add_argument("--movies", help="pre-encoded movies dir "
                        "(default: assets/movies if present)")
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
    if args.target == "sd":
        build_sd(args)
        return
    dkp = find_devkitpro()
    cmake = find_tool("cmake", dkp)
    ninja = find_tool("ninja", dkp)
    for target in ("cube", "wii") if args.target == "all" else (args.target,):
        build(target, dkp, cmake, ninja)


if __name__ == "__main__":
    main()
