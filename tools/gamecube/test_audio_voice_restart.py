#!/usr/bin/env python3
"""Exercise DSP voice reuse and the ARAM refill/stop boundary."""
from pathlib import Path
import subprocess
import tempfile
from test_runtime_guards import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = (ROOT/'src/audio/sampman_gamecube.cpp').read_text()
    code = r'''
#include <cassert>
#include <cstdint>
#include <mutex>
#include <future>
#include <chrono>
using u32 = uint32_t; using f32 = float; using bool8 = bool;
#define nil nullptr
#define FALSE false
static bool interrupts = true;
#define _CPU_ISR_Disable(level) do { level = interrupts; interrupts = false; } while(0)
#define _CPU_ISR_Restore(level) do { interrupts = level; } while(0)
struct AESNDPB {
    const void *buffer = nullptr;
    u32 dspCursor = 64, history = 42, bytes = 0, format = 0;
    bool once = false, loop = false, stream = true, stopped = true;
};
void AESND_PlayVoice(AESNDPB *v, u32 format, const void *buffer, u32 bytes,
                     f32, u32 delay, bool loop) {
    assert(!interrupts && delay == 0);
    v->buffer = buffer; v->bytes = bytes; v->format = format;
    v->dspCursor = v->history = 0;
    v->once = !loop; v->loop = loop; v->stopped = false;
}
void AESND_SetVoiceLoop(AESNDPB *v, bool loop) { assert(!interrupts); v->loop = loop; }
void AESND_SetVoiceStream(AESNDPB *v, bool stream) { assert(!interrupts); v->stream = stream; }
struct GcVoiceStream { bool active = true, bufReady = true; };
static std::mutex voiceMutex;
static std::mutex *gVoiceLock = &voiceMutex;
void LWP_MutexLock(std::mutex *m) { m->lock(); }
void LWP_MutexUnlock(std::mutex *m) { m->unlock(); }
'''
    for name in ['gcPlayVoice', 'gcVoiceStop']:
        at = source.index('\n'+name+'(')
        code += '\nstatic void\n'+function(source[at+1:], name)+'\n'
    code += r'''
int main() {
    uint8_t menu[128] = {}, radio[256] = {};
    AESNDPB voice;
    gcPlayVoice(&voice, 1, radio, sizeof(radio), 48000, true, false);
    assert(voice.stream && !voice.loop && !voice.once && !voice.stopped);
    voice.dspCursor = 53; voice.history = 11;
    gcPlayVoice(&voice, 0, menu, sizeof(menu), 48000, false, false);
    assert(voice.buffer == menu && voice.bytes == sizeof(menu));
    assert(voice.dspCursor == 0 && voice.history == 0);
    assert(voice.once && !voice.loop && !voice.stream && !voice.stopped);
    gcPlayVoice(&voice, 0, radio, sizeof(radio), 48000, false, true);
    assert(!voice.once && voice.loop && !voice.stream && interrupts);

    GcVoiceStream slot;
    std::promise<void> entered;
    voiceMutex.lock();
    auto stopped = std::async(std::launch::async, [&] {
        entered.set_value(); gcVoiceStop(&slot);
    });
    entered.get_future().wait();
    assert(stopped.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    assert(slot.active && slot.bufReady);
    voiceMutex.unlock(); stopped.get();
    assert(!slot.active && !slot.bufReady);
    gcVoiceStop(nullptr);
}
'''
    with tempfile.TemporaryDirectory(prefix='revc-voice-restart-') as d:
        cpp, exe = Path(d)/'test.cpp', Path(d)/'test'
        cpp.write_text(code)
        subprocess.run(['clang++', '-std=c++17', '-pthread', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PASS: voice reuse resets DSP position and playback mode; stopping waits for an in-flight refill')


if __name__ == '__main__':
    main()
