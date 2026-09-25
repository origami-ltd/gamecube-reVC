#!/usr/bin/env python3
"""Round-trip the GameCube INI store (src/extras/ini_gc.h) through a real file."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

CODE = r'''
#include "ini_gc.h"
#include <cassert>
#include <cstdlib>
int main(int argc, char **argv) {
    mINI::INIFile ini(argv[1]);
    mINI::INIStructure cfg;
    char big[50]; snprintf(big, sizeof(big), "%f", 0.75f);
    cfg["Audio"]["MusicVolume"] = "100";
    cfg["Display"]["Brightness"] = big;
    cfg["Audio"]["SfxVolume"] = "64";
    cfg["FrontendOptions"]["Old"] = "1";
    cfg["Audio"]["MusicVolume"] = "90";
    cfg["FrontendOptions"].remove("Old");
    assert(ini.generate(cfg));

    FILE *f = fopen(argv[1], "a");
    fputs("; comment\n[Extra] ; trailing\n  Key  =  spaced value  \n", f);
    fclose(f);

    mINI::INIStructure back;
    assert(ini.read(back));
    auto audio = back.get("Audio");
    assert(audio.size() == 2 && audio.has("MusicVolume") && !audio.has("musicvolume"));
    assert(strtoul(audio.get("MusicVolume").c_str(), nullptr, 0) == 90);
    assert(strtof(back.get("Display").get("Brightness").c_str(), nullptr) == 0.75f);
    assert(back.get("FrontendOptions").size() == 0);
    assert(strcmp(back.get("Extra").get("Key").c_str(), "spaced value") == 0);
    assert(strcmp(back.get("Nope").get("Key").c_str(), "") == 0);
    assert(!mINI::INIFile("/nonexistent/dir/x.ini").read(back));
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='revc-ini-') as d:
        cpp, exe = Path(d)/'test.cpp', Path(d)/'test'
        cpp.write_text(CODE)
        subprocess.run(['clang++', '-std=c++14', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I', str(ROOT/'src/extras'), str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe), str(Path(d)/'reVC.ini')], check=True)
    print('PASS: GameCube INI store writes, reads back, removes and ignores comments')


if __name__ == '__main__':
    main()
