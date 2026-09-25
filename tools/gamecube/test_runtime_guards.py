#!/usr/bin/env python3
"""Compile the actual stream-volume and physical-bound guards with host stubs."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = source.index(name + "(")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    audio = (ROOT / "src/audio/sampman_gamecube.cpp").read_text()
    physical = (ROOT / "src/entities/Physical.cpp").read_text()
    world = (ROOT / "src/core/World.h").read_text()
    defines = "\n".join(re.findall(
        r"^#define (?:SECTOR_SIZE_[XY]|NUMSECTORS_[XY]|WORLD_(?:SIZE|MIN|MAX)_[XY]) .+$",
        world, re.M))
    helper = audio.index("static void\ngcApplyStreamVolume(")
    code = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
using std::isfinite;
using uint8 = uint8_t; using uint32 = uint32_t;
using u32 = uint32_t; using u16 = uint16_t; using bool8 = bool;
#define nil nullptr
static bool interrupts = true;
#define _CPU_ISR_Disable(level) do { level = interrupts; interrupts = false; } while(0)
#define _CPU_ISR_Restore(level) do { interrupts = level; } while(0)
struct AESNDPB { unsigned left = 255, right = 255; };
void AESND_SetVoiceVolume(AESNDPB *voice, u16 left, u16 right) {
    assert(!interrupts); voice->left = left; voice->right = right;
}
struct GcStream { AESNDPB *voice; uint8 volume, pan; bool8 effectVolume; };
enum { MAX_STREAMS = 3 };
static GcStream gStreams[MAX_STREAMS];
static uint8 gEffectsVolume = 127, gMusicVolume = 127;
static uint8 gEffectsFade = 127, gMusicFade = 127;
struct cSampleManager { void SetStreamedVolumeAndPan(uint8, uint8, bool8, uint8); };
struct CRect { float left, top, right, bottom; };
'''
    code += defines + "\nstatic void\n" + function(audio[helper:], "gcApplyStreamVolume")
    code += "\nvoid\n" + function(audio, "cSampleManager::SetStreamedVolumeAndPan")
    code += "\nstatic bool\n" + function(physical, "ValidPhysicalBounds")
    code += r'''
int main() {
    cSampleManager manager;
    manager.SetStreamedVolumeAndPan(127, 63, true, 0);
    assert(interrupts);
    AESNDPB ambience;
    gStreams[0].voice = &ambience;
    gEffectsFade = 0;
    gcApplyStreamVolume(&gStreams[0], 0);
    assert(ambience.left == 0 && ambience.right == 0 && interrupts);
    gEffectsFade = 127;
    gcApplyStreamVolume(&gStreams[0], 0);
    assert(ambience.left == 255 && ambience.right == 255);
    gMusicFade = 0;
    manager.SetStreamedVolumeAndPan(127, 63, false, 0);
    assert(ambience.left == 0 && ambience.right == 0);
    gMusicFade = 127; gMusicVolume = 64;
    gcApplyStreamVolume(&gStreams[0], 0);
    assert(ambience.left == 128 && ambience.right == 128);
    AESNDPB dialogue;
    gStreams[1].voice = &dialogue; gEffectsFade = 0;
    manager.SetStreamedVolumeAndPan(127, 63, true, 1);
    assert(dialogue.left == 255 && dialogue.right == 255);
    manager.SetStreamedVolumeAndPan(0, 0, false, MAX_STREAMS);
    assert(interrupts);
    assert(ValidPhysicalBounds({447, 643, 452, 649}));
    assert(!ValidPhysicalBounds({447, 643, std::numeric_limits<float>::infinity(), 649}));
    assert(!ValidPhysicalBounds({447, 643, std::numeric_limits<float>::max(), 649}));
    assert(!ValidPhysicalBounds({447, std::numeric_limits<float>::quiet_NaN(), 452, 649}));
    assert(!ValidPhysicalBounds({452, 643, 447, 649}));
}
'''
    with tempfile.TemporaryDirectory(prefix="revc-runtime-test-") as tmp:
        source = Path(tmp) / "guards.cpp"
        binary = Path(tmp) / "guards"
        source.write_text(code)
        subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS: deferred stream mute, restored gains, dialogue, and invalid physical bounds")


if __name__ == "__main__":
    main()
