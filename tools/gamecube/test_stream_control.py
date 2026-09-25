#!/usr/bin/env python3
"""Exercise the production stream commands with a blocked-disc mock."""
from pathlib import Path
import os
import subprocess
import tempfile
from test_runtime_guards import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = (ROOT/'src/audio/sampman_gamecube.cpp').read_text()
    code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
using uint8 = uint8_t; using uint16 = uint16_t; using uint32 = uint32_t;
using int32 = int32_t; using uint64 = uint64_t; using u32 = uint32_t;
using bool8 = bool; using tTrack = unsigned;
using f32 = float;
enum { VOICE_MONO16, VOICE_STEREO16, GC_DSP_RATE = 48000 };
constexpr f32 GC_DSP_RATE_F = 48000.0f;
#define TRUE true
#define FALSE false
#define nil nullptr
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
enum { MAX_STREAMS = 3, LWP_THREAD_NULL = 0, GC_FIR_TAPS = 9,
       STREAM_CHUNK_BYTES = 16128, DIGITALRATE = 32000 };
static int gStreamDecThread = 1, currentThread = 2;
static bool interrupts = true;
#define _CPU_ISR_Disable(level) do { level = interrupts; interrupts = false; } while(0)
#define _CPU_ISR_Restore(level) do { interrupts = level; } while(0)
int LWP_GetSelf() { return currentThread; }
struct OggVorbis_File {};
struct AESNDPB { bool stopped = true; const void *buffer = nullptr; };
void gcPlayVoice(AESNDPB *p, u32, const void *buffer, u32, f32, bool8, bool8) {
    assert(!interrupts); p->buffer = buffer; p->stopped = false;
}
struct GcStreamGuard { GcStreamGuard(int) { assert(currentThread == 1); } };
static int gStreamLock[MAX_STREAMS];
static bool _bSampmanInitialised = true, gStreamPreloading;
static const char *StreamedNameTable[9] = {};
static uint8 gStreamSilence[STREAM_CHUNK_BYTES];
void AESND_SetVoiceStop(AESNDPB *v, bool stop) { v->stopped = stop; }
void AESND_SetVoiceBuffer(AESNDPB *v, const void *p, int) { v->buffer = p; }
void AESND_SetVoiceStream(AESNDPB *, bool) {}
AESNDPB *AESND_AllocateVoiceWithArg(void (*)(AESNDPB*,u32,void*), void*) {
    static AESNDPB voices[3]; static int n; assert(n < 3); return &voices[n++];
}
void gcStreamCallback(AESNDPB*,u32,void*) {}
void DCFlushRange(void*,int) {}
void ov_clear(OggVorbis_File*) { assert(currentThread == 1); }
void *memalign(size_t align, size_t bytes) { return std::aligned_alloc(align, bytes); }
void gcTrackPath(uint32 track, char *out, size_t cap) { snprintf(out, cap, "track-%u.ogg", track); }
class cSampleManager {
public:
    bool8 StartStreamedFile(tTrack, uint32, uint8);
    void StopStreamedFile(uint8);
    void PreloadStreamedFile(tTrack, uint8);
    void StartPreloadedStreamedFile(uint8);
    void PauseStream(bool8, uint8);
    bool8 IsStreamPlaying(uint8);
    int32 GetStreamedFilePosition(uint8);
};
static cSampleManager SampleManager;
'''
    a = source.index('struct GcStream {')
    b = source.index('static void gcApplyStreamVolume', a)
    code += source[a:b]
    code += r'''
static int discOpens, discCloses, decodeCalls;
static bool replaceDuringOpen, replaceDuringDecode, missing;
static void gcApplyStreamVolume(GcStream*, uint8) {}
static bool gcSringOpen(GcStream*, const char*) {
    assert(currentThread == 1); discOpens++;
    if(replaceDuringOpen) {
        replaceDuringOpen = false; currentThread = 2;
        SampleManager.StartStreamedFile(7, 700, 0);
        currentThread = 1;
    }
    return !missing;
}
static void gcSringClose(GcStream*) { assert(currentThread == 1); discCloses++; }
struct GcSring { bool active = true, sync = true; uint32 fetch = 0, fileSize = 1000000; };
static GcSring gSring[MAX_STREAMS];
static uint32 gcSringAvail(GcSring*) { return 65536; }
static uint32 gcStreamDecode(GcStream*, uint8 *dst) {
    assert(currentThread == 1); decodeCalls++;
    memset(dst, 0, STREAM_CHUNK_BYTES);
    if(replaceDuringDecode) {
        replaceDuringDecode = false; currentThread = 2;
        SampleManager.StopStreamedFile(0); currentThread = 1;
    }
    return STREAM_CHUNK_BYTES;
}
static void gcAudioDie(const char*,const char*) { std::abort(); }
static void gcStreamPump(GcStream*, bool prime = false);
'''
    funcs = [
        ('static void', 'gcStreamPump'), ('static void', 'gcStreamArm'),
        ('int', 'gcStreamPrimed'),
        ('void', 'cSampleManager::PreloadStreamedFile'),
        ('void', 'cSampleManager::PauseStream'),
        ('void', 'cSampleManager::StartPreloadedStreamedFile'),
        ('bool8', 'cSampleManager::StartStreamedFile'),
        ('void', 'cSampleManager::StopStreamedFile'),
        ('int32', 'cSampleManager::GetStreamedFilePosition'),
        ('bool8', 'cSampleManager::IsStreamPlaying'),
        ('static void', 'gcStreamApplyRequest'),
    ]
    for result, name in funcs:
        # Select the definition, skipping forward declarations and calls.
        a = source.index('\n'+name+'(') if name != 'gcStreamPrimed' else source.index('extern "C" int gcStreamPrimed(')-1
        code += '\n'+result+'\n'+function(source[a+1:], name)+'\n'
    code += r'''
void worker() { currentThread = 1; gcStreamApplyRequest(0); currentThread = 2; }
void prime() {
    currentThread = 1; gStreams[0].opening = 0;
    gcStreamPump(&gStreams[0], true); gcStreamApplyRequest(0); currentThread = 2;
}
int main() {
    assert(SampleManager.StartStreamedFile(1, 1234, 0));
    assert(discOpens == 0 && discCloses == 0);
    assert(SampleManager.IsStreamPlaying(0) && !gcStreamPrimed(0));
    assert(SampleManager.GetStreamedFilePosition(0) == 1234);
    SampleManager.StartStreamedFile(2, 2345, 0);
    worker(); assert(discOpens == 1 && strcmp(gStreams[0].path, "track-2.ogg") == 0);
    assert(!gcStreamPrimed(0)); prime();
    assert(gcStreamPrimed(0) && gStreams[0].armed && !gStreams[0].voice->stopped);
    int before = decodeCalls;
    SampleManager.PauseStream(true, 0); assert(gStreams[0].voice->stopped);
    worker(); assert(!SampleManager.IsStreamPlaying(0));
    SampleManager.PauseStream(false, 0); worker();
    assert(!gStreams[0].voice->stopped && decodeCalls == before);
    SampleManager.StopStreamedFile(0);
    assert(gStreams[0].voice->stopped && !SampleManager.IsStreamPlaying(0));
    assert(SampleManager.GetStreamedFilePosition(0) == 0); worker();

    SampleManager.PreloadStreamedFile(3, 0); worker();
    assert(!SampleManager.IsStreamPlaying(0) && !gcStreamPrimed(0));
    prime(); assert(gcStreamPrimed(0) && !gStreams[0].armed && gStreams[0].bufReady);
    SampleManager.StartPreloadedStreamedFile(0); worker();
    assert(gStreams[0].armed && !gStreams[0].voice->stopped);
    before = decodeCalls; SampleManager.StartPreloadedStreamedFile(0); worker();
    assert(decodeCalls == before);

    SampleManager.PreloadStreamedFile(3, 0); worker(); prime();
    SampleManager.PauseStream(true, 0);
    SampleManager.StartPreloadedStreamedFile(0); worker();
    assert(gStreams[0].voice->stopped && !SampleManager.IsStreamPlaying(0));
    SampleManager.PauseStream(false, 0); worker();
    assert(gStreams[0].armed && !gStreams[0].voice->stopped);

    SampleManager.StartStreamedFile(4, 400, 0); replaceDuringOpen = true; worker();
    assert(!gcStreamPrimed(0) && !gcStreamCanPlay(0) && gStreams[0].voice->stopped);
    worker(); assert(strcmp(gStreams[0].path, "track-7.ogg") == 0); prime();
    SampleManager.StartStreamedFile(5, 0, 0); worker();
    replaceDuringDecode = true; prime();
    assert(gStreams[0].voice->stopped && !SampleManager.IsStreamPlaying(0)); worker();

    SampleManager.PreloadStreamedFile(6, 0);
    SampleManager.StartPreloadedStreamedFile(0); worker(); prime();
    assert(SampleManager.IsStreamPlaying(0) && gStreams[0].armed);
    gStreams[0].playing = false; assert(!SampleManager.IsStreamPlaying(0));
    missing = true; SampleManager.StartStreamedFile(8, 0, 0); worker();
    assert(!SampleManager.IsStreamPlaying(0) && gcStreamPrimed(0));
    assert(!SampleManager.StartStreamedFile(99, 0, 0));
    for(auto &st : gStreams) for(auto p : st.buf) free(p);
    assert(interrupts);
    puts("PASS: nonblocking radio commands, replacement during I/O, preload, pause, EOF and missing files");
}
'''
    with tempfile.TemporaryDirectory(prefix='revc-stream-control-') as d:
        cpp, exe = Path(d)/'test.cpp', Path(d)/'test'
        cpp.write_text(code)
        subprocess.run(['clang++', '-std=c++17', '-g', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})


if __name__ == '__main__':
    main()
