#!/usr/bin/env python3
"""One-command DOL build for macOS, Linux and Windows.

    python3 build.py            # GameCube DOL (build/cube/src/reVC.dol)
    python3 build.py wii        # Wii dev DOL (build/wii/src/reVC.dol)
    python3 build.py all        # both

Needs a devkitPro install with the GameCube/Wii toolchains (see README).
Everything else the build needs ships in this repository.
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", nargs="?", default="cube",
                        choices=("cube", "wii", "all"))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        assert callable(build) and ROOT
        print("build.py self-test passed")
        return
    dkp = find_devkitpro()
    cmake = find_tool("cmake", dkp)
    ninja = find_tool("ninja", dkp)
    for target in ("cube", "wii") if args.target == "all" else (args.target,):
        build(target, dkp, cmake, ninja)


if __name__ == "__main__":
    main()
