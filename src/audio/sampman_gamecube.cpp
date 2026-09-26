// GameCube sample manager: AESND for mixing, ARAM for the sample banks.
//
// The port had no audio at all — REVC_AUDIO=NULL compiled sampman_null.cpp,
// forty-seven empty methods — so this is a subsystem being written, not a bug
// being fixed.
//
// Two hardware facts shape the whole design.
//
// libogc's AESND takes PCM only (VOICE_MONO8/STEREO8/MONO16/STEREO16 and their
// unsigned forms; see aesndlib.h). The DSP's hardware ADPCM decode lives in
// Nintendo's AX microcode, which libogc does not ship, so ADPCM has to be
// decoded on the CPU. It is still worth it — ADPCM decode is roughly an order
// of magnitude cheaper than Vorbis, and the bank shrinks 3.5x — but it is not
// free, and calling it free was wrong.
//
// ARAM cannot be addressed. The CPU reaches it only through block DMA, so a
// sample plays from MEM1: the bank lives in ARAM and the few kilobytes a voice
// needs are pulled across when it starts. That fits how VC uses sound — short
// one-shots from a bank that is otherwise idle — and it keeps 324MB of sample
// data out of a 16MB arena.
//
// Rates are left exactly as the game authored them. Measured over sfx.sdt:
// 9941 samples, 81% at 12kHz, 13% at 16kHz, and eleven at 32kHz. Resampling
// those up to a uniform rate would multiply the bank for fidelity that was
// never recorded.
#include "common.h"

#ifdef AUDIO_GAMECUBE

#include "sampman.h"
#include "AudioManager.h"
#include "MusicManager.h"
#include "Frontend.h"
#include "CdStream.h"

#include <gccore.h>
#include <aesndlib.h>
#include <ogc/aram.h>
#include <ogc/arqueue.h>
#include <ogc/cache.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <tremor/ivorbisfile.h>
#include <ctype.h>
#include <unistd.h>
#include "vendor/librw/src/lodepng/lodepng.h"


// Fail-loud audio: any sound that cannot be served stops the game on the
// spot, with the reason on the card first. That was the right trade while
// hunting the mute - a silent miss hides in a play session, a park screen
// naming the failure does not.
//
// OFF for shipping and for real hardware. It turned a missing sound into a
// dead console at the end of a load, which tells the player nothing
// and costs them the session. Audio that cannot be served is now a line on
// the gecko and silence in that one channel; the game keeps running.
// Re-enable it when hunting an audio bug, not otherwise.

static void
gcAudioDie(const char *what, const char *detail)
{
	// A sound that cannot be served is one line and silence on that channel;
	// the game keeps running.
	printf("audio-miss %s %s\n", what, detail ? detail : "");
}

cSampleManager SampleManager;
bool8 _bSampmanInitialised = FALSE;

uint32 BankStartOffset[MAX_SFX_BANKS];
uint32 nNumMP3s;

// One AESND voice per game channel. VC drives channels by index and expects
// them to be independent, which maps to a voice each.
struct GcChannel {
	AESNDPB *voice;
	void    *pcm;        // sample data the voice reads from
	bool8    pcm48;      // pcm holds 48kHz-converted data, so scale the freq
	uint32   pcmFreq;    // game pitch baked into pcm48; live changes scale from it
	bool8    pcmOwned;   // pcm is this channel's own buffer (ped/talk copies);
	                     // FALSE = pointer into the resident bank, never freed
	uint32   pcmBytes;
	uint32   allocBytes; // what memalign actually handed us
	uint32   sample;     // which sfx is loaded
	uint32   freq;
	uint32   volume;     // 0..127 as the game supplies it
	uint32   pan;        // 0..127, 63 centre (2D fallback)
	f32      posX, posY, posZ;   // camera-space position, from the game
	f32      distMax, distMin;   // rolloff window, from the game
	bool8    has3D;              // a position was given for this play
	uint32   loopCount;
	uint32   loopStart;  // in source samples; the engine sustain lives here
	int32    loopEnd;    // -1 = to the end of the sample
	bool8    used;
	volatile bool8 playing;   // cleared by the AESND callback when the buffer ends
	struct GcVoiceStream *vs;  // ARAM streaming state, one per voice (B42)
	bool8    streamed;        // this play is fed from ARAM, no whole-sample PCM
};
static GcChannel gChannels[MAXCHANNELS + MAX2DCHANNELS];

// AESND has 32 voices, hard cap. Three are the streams (radio, mission
// dialogue, cutscene); the rest are game channels. GetMaximumSupportedChannels
// reports this, so the game indexes channels 0..28 and never touches a
// voiceless slot — returning 0 there is what turned audio off entirely
// (AudioManager reads <=1 as "no hardware" and terminates itself).
enum { GC_CHANNEL_VOICES = MAX_VOICES - MAX_STREAMS };

// Two channels start OUTSIDE the engine's volume-ranked cull and so cannot
// be capped by GetMaximumSupportedChannels: CHANNEL_PLAYER_VEHICLE_ENGINE
// sits at m_nActiveSamples (the engine's own -- in cAudioManager::Initialise
// reserves that slot from what we report), and the police radio lives at the
// FIXED slot CHANNEL_POLICE_RADIO (43) — which had no voice at all, so the
// first patrol car near the player parked the game with channel-no-voice.
// One voice is held back for the police radio; the generics get the rest.
enum { GC_GENERIC_VOICES = GC_CHANNEL_VOICES - 1 };

// AESND has no "is this voice still going" query, so the voice tells us. The
// callback runs on the audio thread and only ever clears the flag, which is
// why a plain volatile bool is enough — there is no read-modify-write to race.
struct GcVoiceStream;
static void gcVoiceStreamCallback(AESNDPB *pb, GcChannel *c);
static void
gcVoiceCallback(AESNDPB *pb, u32 state, void *arg)
{
	if(state == VOICE_STATE_STOPPED)
		((GcChannel*)arg)->playing = FALSE;
	else if(state == VOICE_STATE_STREAM)
		gcVoiceStreamCallback(pb, (GcChannel*)arg);
}

static void
gcPlayVoice(AESNDPB *voice, u32 format, const void *buffer, u32 bytes,
    f32 frequency, bool8 stream, bool8 loop)
{
	u32 level;
	_CPU_ISR_Disable(level);
	AESND_PlayVoice(voice, format, buffer, bytes, frequency, 0, stream || loop);
	AESND_SetVoiceLoop(voice, loop && !stream);
	AESND_SetVoiceStream(voice, stream);
	_CPU_ISR_Restore(level);
}

// The sample index, read once from sfx.sdt. tSample is what the game already
// uses: offset, size, frequency, loop start and loop end.
static tSample *gSampleIndex;
static uint32   gNumSamples;

// Optional lossless mini-DVD bank. sfx.pak stores every PCM sample as an
// independently seekable delta/byte-shuffled DEFLATE block. The original SD
// layout with sfx.raw remains supported for development cards.
static bool8  gPackedSfx;

// Stream-decode thread state; the machinery lives next to gStreams below.
unsigned gStreamStarvedTotal;   // silence chunks served to starved voices
unsigned gStreamDecPumps;       // chunks decoded by the decode thread
static lwp_t gStreamDecThread = LWP_THREAD_NULL;
static lwp_t gVoiceDecThread = LWP_THREAD_NULL;
static mutex_t gStreamLock[MAX_STREAMS];
static volatile bool8 gStreamDecQuit;
static void *gcStreamDecMain(void *);
static void *gcVoiceDecMain(void *);
static mutex_t gSringLock = LWP_MUTEX_NULL;   // audio I/O ring (B68), defined further down
static void gcSringService(bool8 wait);
struct GcStream; static void gcStreamOpenStep(GcStream *st, int32 idx);
struct GcStreamGuard {
	mutex_t m;
	GcStreamGuard(mutex_t mm) : m(mm) { LWP_MutexLock(m); }
	~GcStreamGuard() { LWP_MutexUnlock(m); }
};
static uint32 gPackedSfxBytes;
static uint32 gPackedSfxDataStart;
enum { GC_SFX_PACK_HEADER = 16, GC_SFX_PACK_ENTRY = 8 };
static const uint8 gSfxPackMagic[8] = { 'G','C','S','F','X','P','2',0 };

static uint32
gcReadBe32(const uint8 *p)
{
	return (uint32)p[0] << 24 | (uint32)p[1] << 16 |
	       (uint32)p[2] << 8 | p[3];
}

// Bank residency in ARAM. A bank is a contiguous run of sfx.raw, so one ARAM
// allocation and one DMA per bank.
struct GcBank {
	uint32 aramAddr;
	uint32 bytes;
	bool8  loaded;
};

// Where each bank sample sits in ARAM. Native rate, host-endian,
// byte-for-byte the size the game's own sfx.raw carries — no inflation.
static uint32 gBankSampleAddr[SAMPLEBANK_PED_START];
static GcBank gBanks[MAX_SFX_BANKS];

// The DSP's own output rate, taken from libogc rather than assumed: the
// GameCube clocks it at 54MHz/1124 = 48042.7Hz.
// Anything handed to the DSP at a different rate is resampled by
// sample-repeat inside the ucode, with no interpolation, which aliases
// audibly - so channels convert once on the way in, to THIS rate, and the
// voice is then played at it. The ratio is exactly 1.0 and the ucode's
// resampler never runs. 48kHz 16-bit is the hardware ceiling; there is no
// higher-quality path on this machine.
#define GC_DSP_RATE_F  ((f32)DSP_DEFAULT_FREQ)
enum { GC_DSP_RATE = (uint32)(54000000.0/1124.0 + 0.5) };
// Ceiling on a converted channel buffer. Above it the sample plays native
// (the DSP's stair-step is the lesser evil against a 24MB arena).
// A converted buffer is ~2.2x the native sample. 512KB covers 99.9% of the
// bank (measured over sfx.sdt); the handful above it play native.
enum { GC_CH_RESAMPLE_CAP = 512*1024 };   // FIR for everything that fits (AESND's ucode resamples by sample-repeat: gravel on speech) — B41 bridge until the ARAM streaming voices land
// ...but 29 channels must not each hold one, so conversions also draw on a
// shared MEM1 budget. Past it a sound plays native rather than failing: the
// DSP's stair-step is the graceful degradation, an allocation failure is not.
// Channel PCM arena (B38). B36's tour logged 337 refusals of a 176K channel
// buffer with 1.7MB free: heap fragmentation, and every refusal is a sound
// the game re-requests the next frame (crackle). One block carved at init,
// first-fit spans with coalescing, nothing else ever allocates in it — a
// voice's buffer either fits here or the voice does not play.
enum { GC_CONV_BUDGET = 256*1024 };   // player talk and frontend shared PCM only (B47: ped comments stream from ARAM)
static uint8 *gPcmArena;
struct GcPcmSpan { uint32 addr, size; bool8 used; };
static GcPcmSpan gPcmSpans[192];
static int32 gPcmSpanCount;
static void
gcPcmArenaInit(void)
{
	if(gPcmArena) return;
	gPcmArena = (uint8*)memalign(32, GC_CONV_BUDGET);
	if(gPcmArena == nil) return;
	gPcmSpans[0].addr = (uint32)gPcmArena; gPcmSpans[0].size = GC_CONV_BUDGET; gPcmSpans[0].used = FALSE;
	gPcmSpanCount = 1;
	printf("AUDIO pcm arena %uK\n", (unsigned)(GC_CONV_BUDGET/1024));
}
static void*
gcPcmAlloc(uint32 size)
{
	if(gPcmArena == nil) return memalign(32, size);   // arena refused at init: plain heap
	size = (size + 31) & ~31u;
	for(int32 i = 0; i < gPcmSpanCount; i++){
		GcPcmSpan *sp = &gPcmSpans[i];
		if(sp->used || sp->size < size) continue;
		if(sp->size > size){
			if(gPcmSpanCount >= (int32)ARRAY_SIZE(gPcmSpans)) return nil;
			memmove(sp+2, sp+1, sizeof(GcPcmSpan)*(gPcmSpanCount-i-1));
			sp[1].addr = sp->addr + size; sp[1].size = sp->size - size; sp[1].used = FALSE;
			sp->size = size; gPcmSpanCount++;
		}
		sp->used = TRUE;
		return (void*)sp->addr;
	}
	return nil;
}
static void
gcPcmFree(void *p)
{
	if(p == nil) return;
	if(gPcmArena == nil || (uint8*)p < gPcmArena || (uint8*)p >= gPcmArena + GC_CONV_BUDGET){ free(p); return; }
	for(int32 i = 0; i < gPcmSpanCount; i++){
		if(gPcmSpans[i].addr != (uint32)p || !gPcmSpans[i].used) continue;
		gPcmSpans[i].used = FALSE;
		if(i+1 < gPcmSpanCount && !gPcmSpans[i+1].used){
			gPcmSpans[i].size += gPcmSpans[i+1].size;
			memmove(&gPcmSpans[i+1], &gPcmSpans[i+2], sizeof(GcPcmSpan)*(gPcmSpanCount-i-2)); gPcmSpanCount--;
		}
		if(i > 0 && !gPcmSpans[i-1].used){
			gPcmSpans[i-1].size += gPcmSpans[i].size;
			memmove(&gPcmSpans[i], &gPcmSpans[i+1], sizeof(GcPcmSpan)*(gPcmSpanCount-i-1)); gPcmSpanCount--;
		}
		return;
	}
}
// Anything bigger than this goes back to the pool the moment its sound is
// done. Measured before this existed: 1051 of 1338 sounds could not be
// converted because the pool was full of buffers belonging to sounds that
// had already finished, so 79% of the game went through the DSP's
// sample-repeat resampler - which is the robotic timbre, and in the menu it
// is loud enough to read as noise.
enum { GC_CONV_RECLAIM = 32*1024 };
static uint32 gConvBytes;
// How the conversion budget is actually doing, reported by the heartbeat.
// A sound that cannot be converted plays through the DSP's sample-repeat
// resampler, which measured 47x to 185x the reference's energy above the
// source's Nyquist - that is the robotic timbre, so the fallback count is
// the number that says how much of it is left.
static uint32 gConvOk, gConvFallback, gConvPeak;


// Frontend stereo pairs are unexpectedly long (the highlight alone is 1.14s
// at 8.1kHz). Rapid navigation overlaps many copies; converting every voice
// separately used to fill the 2MB pool after ten moves. The PCM is immutable,
// so concurrent voices share one conversion per frontend sample. Service
// releases it as soon as the last borrowing voice stops.
struct GcSharedPcm {
	void   *pcm;
	uint32 bytes;
	uint32 allocBytes;
	uint32 freq;
};
static GcSharedPcm gFrontendPcm[SFX_FE_ERROR_RIGHT - SFX_INFO_LEFT + 1];

static uint8 gEffectsVolume = 127, gMusicVolume = 127;
static uint8 gEffectsFade = 127, gMusicFade = 127;

// Ped comments: seven rotating PED_BLOCKSIZE slots filled straight from
// sfx.raw on demand, plus one dedicated player-talk buffer — the OAL layout.
// ponytail: plain MEM1 malloc (~630KB); move to ARAM staging if the
// arena ever needs it back.
static uint8 *gPedBuf;
extern "C" { extern volatile const char *gMainWhere; }   // gamecube.cpp watchdog checkpoint
extern "C" { volatile unsigned gDecTick, gAudioCbTick; }   // MemoryWatcher heartbeats
extern volatile uint32 gMainTick;   // gamecube.cpp
extern "C" { extern volatile unsigned gxLastDraw, gxLastGeoFlags, gxLastGeoVerts, gxDmaBusy, gCdTick, gCdState, gIsoRdBusy; }   // watchdog inputs
// B47: the seven slots live in ARAM ("SFX straight from the disc, cached in
// ARAM" — user); the voices stream them as 16-bit PCM blocks. sfx.raw only;
// the DEFLATE pack (sfx.pak) still lands in MEM1 slots.
static uint32 gPedAram, gPedSlotStride;
static int32  gPedSlotSfx[MAX_PEDSFX];
static uint8  gCurrentPedSlot;
static uint8 *gPlayerTalkData;
// B123: sfx.adp packs EVERY sample (pack_sfx_adpcm.py --all). The resident
// bank stays in ARAM as before; ped comments and player talk are read from
// the pack on demand, and the 340 MB sfx.raw leaves the disc. Entry offsets
// are implied by sfx.sdt: 512-byte blocks of 1017 samples, in table order.
static bool8  gAdpAll;
static uint32 gAdpDataStart, gSfxAdpLba, gSfxAdpSize;
static bool8  gPedSlotAdpcm[MAX_PEDSFX];
static uint32 gPedSlotBytes[MAX_PEDSFX];
static inline int16 gcImaNibble(uint8 nib, int32 *pred, int32 *idx);
static uint32 gcAdpBlocks(uint32 n) { return (gSampleIndex[n].nSize/2 + 1016)/1017; }
static uint32 gcAdpOffset(uint32 n) { uint32 blocks = 0; for(uint32 j = 0; j < n; j++) blocks += gcAdpBlocks(j); return gAdpDataStart + blocks*512; }
static uint32 gPlayerTalkSfx = 0xFFFFFFFF;

// Read one sample into DSP-native big-endian PCM. The packed path reconstructs
// the exact sfx.raw words; it is compression, never a format conversion.
static bool8
gcReadSampleData(uint32 nSfx, uint8 *dst, uint32 capacity)
{
	if(gSampleIndex == nil || nSfx >= gNumSamples ||
	   gSampleIndex[nSfx].nSize > capacity)
		return FALSE;
	uint32 rawSize = gSampleIndex[nSfx].nSize;

	if(!gPackedSfx && gAdpAll){
		// B123: decode the sample's ADPCM blocks from the pack straight into dst as native int16.
		DVD_FS_GUARD;
		FILE *f = fopen("dvd:/audio/sfx.adp", "rb");
		if(f == nil || fseek(f, (long)gcAdpOffset(nSfx), SEEK_SET) != 0){ if(f) fclose(f); return FALSE; }
		uint32 total = rawSize/2, out = 0, blocks = gcAdpBlocks(nSfx);
		int16 *o = (int16*)dst;
		uint8 blk[512];
		bool8 ok = TRUE;
		for(uint32 b = 0; ok && b < blocks && out < total; b++){
			ok = fread(blk, 1, sizeof(blk), f) == sizeof(blk);
			if(!ok) break;
			int32 pred = (int16)((uint16)blk[0] | ((uint16)blk[1] << 8)), idx = blk[2] > 88 ? 88 : blk[2];
			o[out++] = (int16)pred;
			for(uint32 i = 4; i < sizeof(blk) && out < total; i++){
				o[out++] = gcImaNibble(blk[i] & 15, &pred, &idx);
				if(out < total) o[out++] = gcImaNibble(blk[i] >> 4, &pred, &idx);
			}
		}
		fclose(f);
		return ok;
	}
	if(!gPackedSfx){
		DVD_FS_GUARD;
		FILE *f = fopen("dvd:/audio/sfx.raw", "rb");
		if(f == nil)
			return FALSE;
		bool8 ok = fseek(f, (long)gSampleIndex[nSfx].nOffset, SEEK_SET) == 0 &&
		    fread(dst, 1, rawSize, f) == rawSize;
		fclose(f);
		if(!ok)
			return FALSE;
		for(uint32 b = 0; b + 1 < rawSize; b += 2){
			uint8 t = dst[b]; dst[b] = dst[b+1]; dst[b+1] = t;
		}
		return TRUE;
	}

	uint8 entry[GC_SFX_PACK_ENTRY];
	uint8 *packed = nil;
	uint32 packedSize = 0;
	bool8 storedRaw = FALSE;
	{
		DVD_FS_GUARD;
		FILE *f = fopen("dvd:/audio/sfx.pak", "rb");
		if(f == nil || fseek(f, GC_SFX_PACK_HEADER + nSfx*GC_SFX_PACK_ENTRY,
		                      SEEK_SET) != 0 ||
		   fread(entry, 1, sizeof(entry), f) != sizeof(entry)){
			if(f) fclose(f);
			return FALSE;
		}
		uint32 offset = gcReadBe32(entry);
		uint32 sizeFlags = gcReadBe32(entry + 4);
		storedRaw = (sizeFlags & 0x80000000u) != 0;
		packedSize = sizeFlags & 0x7FFFFFFFu;
		if(offset < gPackedSfxDataStart || packedSize == 0 ||
		   offset > gPackedSfxBytes || packedSize > gPackedSfxBytes - offset){
			fclose(f);
			return FALSE;
		}
		packed = (uint8*)memalign(32, packedSize);
		bool8 ok = packed != nil && fseek(f, (long)offset, SEEK_SET) == 0 &&
		    fread(packed, 1, packedSize, f) == packedSize;
		fclose(f);
		if(!ok){
			free(packed);
			return FALSE;
		}
	}

	if(storedRaw){
		if(packedSize != rawSize){
			free(packed);
			return FALSE;
		}
		for(uint32 i = 0; i < rawSize/2; i++){
			dst[i*2] = packed[i*2 + 1];
			dst[i*2 + 1] = packed[i*2];
		}
		free(packed);
		return TRUE;
	}

	uint8 *predicted = nil;
	size_t predictedSize = 0;
	unsigned error = lodepng_zlib_decompress(&predicted, &predictedSize,
	    packed, packedSize, &lodepng_default_decompress_settings);
	free(packed);
	if(error || predictedSize != rawSize){
		free(predicted);
		return FALSE;
	}
	uint32 count = rawSize/2;
	uint16 previous = 0;
	for(uint32 i = 0; i < count; i++){
		uint16 zigzag = predicted[i] | (uint16)predicted[count + i] << 8;
		int32 delta = (zigzag >> 1) ^ -(int32)(zigzag & 1);
		previous = (uint16)(previous + delta);
		dst[i*2] = previous >> 8;
		dst[i*2 + 1] = previous & 0xFF;
	}
	free(predicted);
	return TRUE;
}

// One bounded read shared by ped comments and player talk.
static uint32 align32(uint32 v);
static void gcBankWrite(uint32 dst, const void *src, uint32 n);
// A ped comment from sfx.raw straight into its ARAM slot: 32K pieces through
// one staging buffer, byte-swapped like the old MEM1 path, DMA'd as they land.
static bool8
gcReadSampleToAram(uint32 nSfx, uint32 aram)
{
	if(gSampleIndex == nil || nSfx >= gNumSamples ||
	   gSampleIndex[nSfx].nSize > PED_BLOCKSIZE)
		return FALSE;
	static uint8 stage[32*1024] __attribute__((aligned(32)));
	uint32 size = gSampleIndex[nSfx].nSize, done = 0;
	DVD_FS_GUARD;
	FILE *f = fopen("dvd:/audio/sfx.raw", "rb");
	if(f == nil)
		return FALSE;
	bool8 ok = fseek(f, (long)gSampleIndex[nSfx].nOffset, SEEK_SET) == 0;
	while(ok && done < size){
		uint32 chunk = size - done > sizeof(stage) ? (uint32)sizeof(stage) : size - done;
		ok = fread(stage, 1, chunk, f) == chunk;
		if(!ok) break;
		for(uint32 b = 0; b + 1 < chunk; b += 2){
			uint8 t = stage[b]; stage[b] = stage[b+1]; stage[b+1] = t;
		}
		uint32 w = align32(chunk);
		if(w > chunk) memset(stage + chunk, 0, w - chunk);
		gcBankWrite(aram + done, stage, w);
		done += chunk;
	}
	fclose(f);
	return ok;
}

static bool8
gcReadSample(uint32 nSfx, uint8 *dst)
{
	char d[48];
	if(gSampleIndex == nil || nSfx >= gNumSamples ||
	   gSampleIndex[nSfx].nSize > PED_BLOCKSIZE){
		snprintf(d, sizeof(d), "sfx=%u idx=%d", (unsigned)nSfx, gSampleIndex != nil);
		gcAudioDie("sample-request-bad", d);
		return FALSE;
	}
	if(!gcReadSampleData(nSfx, dst, PED_BLOCKSIZE)){
		snprintf(d, sizeof(d), "sfx=%u", (unsigned)nSfx);
		gcAudioDie(gPackedSfx ? "sfx.pak-read" : "sfx.raw-read", d);
		return FALSE;
	}
	return TRUE;
}

static inline uint32
align32(uint32 v)
{
	return (v + 31) & ~31u;
}

// The bank lives in ARAM, with a one-way stack lifetime.
static uint32
gcBankAlloc(uint32 bytes)
{
	// AR_Alloc neither bounds-checks ARAM nor fails; the guard lives here.
	static uint32 used;
	if(used + bytes > AR_GetInternalSize() - 0x4000)
		return 0;
	used += bytes;
	return AR_Alloc(bytes);
}
static void
gcBankWrite(uint32 dst, const void *src, uint32 n)
{
	DCFlushRange((void*)src, n);
	ARQRequest request;
	ARQ_PostRequest(&request, 0x47534155, ARQ_MRAMTOARAM, ARQ_PRIO_LO,
	    dst, (u32)MEM_VIRTUAL_TO_PHYSICAL(src), n);
}
static void
gcBankRead(void *dst, uint32 src, uint32 n)
{
	DCInvalidateRange(dst, n);
	ARQRequest request;
	ARQ_PostRequest(&request, 0x47534155, ARQ_ARAMTOMRAM, ARQ_PRIO_LO,
	    src, (u32)MEM_VIRTUAL_TO_PHYSICAL(dst), n);
}

// ---------------------------------------------------------------- lifecycle

static void gcStreamsShutdown(void);   // defined with the stream machinery
static void gcVoiceSlotsInit(void);    // defined with the ARAM voice machinery
static void gcLoadTrackLengths(void);  // same


bool8
cSampleManager::Initialise(void)
{
	if(_bSampmanInitialised)
		return TRUE;

	if(!AR_CheckInit()){
		// AR_Alloc records each block length via *__ARBlockLen++ with no
		// null or bounds check — AR_Init(nil, 0) hands it a null pointer and
		// the first allocation writes through address zero (measured: boot
		// died on an unknown instruction with the exception vectors gone).
		// 300, not 16: the CdStream ARAM cache allocs up to 256 slots through
		// the SAME array when this init wins the race (both sides guard with
		// AR_CheckInit, so whoever runs first sizes for both).
		static u32 aramBlocks[300];
		AR_Init(aramBlocks, 300);
	}
	// B115 (user): dvd:/noaudio.txt = no audio at all — the PC "no device"
	// path (cAudioManager stays uninitialised, every DMAudio call is a no-op).
	// ARAM is initialised above regardless, the texel store needs it.
	{
		FILE *f = fopen("dvd:/noaudio.txt", "rb");
		if(f){
			fclose(f);
			printf("AUDIO: disabled by dvd:/noaudio.txt\n");
			return FALSE;
		}
	}

	// AUDIO REMOVED FOR THE MEMORY TEST (user directive 09-01): no sample
	// bank in ARAM, no ped buffer, no AESND voices, no decode thread, no
	// Vorbis radio streams, no per-channel PCM staging. Returning FALSE is the
	// PC "no audio device" path: cAudioManager stays uninitialised, so
	// MusicManager never starts and every DMAudio call is a no-op. ARAM is
	// still initialised above with a real block table — the CdStream cache
	// would otherwise AR_Init(nil, 0) and the first AR_Alloc writes through
	// address zero. FMV audio (gcmovie's own AESND lifetime) is separate.
	//
	// Audio is back (user, 09-01 evening): the resident bank rides ARAM as
	// IMA ADPCM (3.8MB in the 4MB the texel tier leaves), decoded on prepare;
	// the Tremor streams stay; the streaming budget yields the MEM1.

	AESND_Init();
	AESND_Pause(false);
	gcPcmArenaInit();

	// Only as many as the budget allows: allocating all 44 slots would eat
	// every voice and leave the streams none. Generics first, then the one
	// held-back voice goes to the police radio's fixed slot (see
	// GC_GENERIC_VOICES above).
	for(int32 i = 0; i < GC_GENERIC_VOICES; i++){
		gChannels[i].voice = AESND_AllocateVoiceWithArg(gcVoiceCallback, &gChannels[i]);
		if(gChannels[i].voice)
			AESND_SetVoiceStop(gChannels[i].voice, true);
	}
	{
		GcChannel *pc = &gChannels[CHANNEL_POLICE_RADIO];
		if(pc->voice == nil)
			pc->voice = AESND_AllocateVoiceWithArg(gcVoiceCallback, pc);
		if(pc->voice)
			AESND_SetVoiceStop(pc->voice, true);
	}
	gcVoiceSlotsInit();

	// Not fatal when the bank is absent. The card does not carry audio yet,
	// and the null backend this replaces always reported success — failing
	// init here would turn "no sound" into "no boot", which is a strictly
	// worse way to be missing audio.
	if(!InitialiseSampleBanks()){
		gcAudioDie("sfx.sdt-open-or-read", "dvd:/audio/sfx.sdt");
	}else if(!LoadSampleBank(SFX_BANK_0)){
		// The OAL and Miles backends load the main bank inside their own
		// Initialise; nothing game-side does it on the PC path. Without this
		// no channel ever passes the loaded check and every effect is silent.
		gcAudioDie("bank0-load", "see BANK line above");
	}

	gcLoadTrackLengths();

	{
		static bool8 locksInit;
		if(!locksInit){
			locksInit = TRUE;
			for(int32 i = 0; i < MAX_STREAMS; i++)
				LWP_MutexInit(&gStreamLock[i], true);
		}
	}
	if(gStreamDecThread == LWP_THREAD_NULL){
		gStreamDecQuit = FALSE;
		// Above the game thread so a ready chunk preempts rendering, below
		// the CdStream worker so model loads keep the disc.
		if(LWP_CreateThread(&gStreamDecThread, gcStreamDecMain, nil, nil,
		    64*1024, 72) != 0)
			gStreamDecThread = LWP_THREAD_NULL;
	}
	if(gVoiceDecThread == LWP_THREAD_NULL &&
	   LWP_CreateThread(&gVoiceDecThread, gcVoiceDecMain, nil, nil, 16*1024, 73) != 0){
		gVoiceDecThread = LWP_THREAD_NULL;
		gcAudioDie("voice-worker", "thread creation failed");
	}

	_bSampmanInitialised = TRUE;
	return TRUE;
}

void
cSampleManager::Terminate(void)
{
	if(!_bSampmanInitialised)
		return;
	gStreamDecQuit = TRUE;
	if(gVoiceDecThread != LWP_THREAD_NULL){
		LWP_JoinThread(gVoiceDecThread, nil);
		gVoiceDecThread = LWP_THREAD_NULL;
	}
	if(gStreamDecThread != LWP_THREAD_NULL){
		LWP_JoinThread(gStreamDecThread, nil);
		gStreamDecThread = LWP_THREAD_NULL;
	}
	for(int32 i = 0; i < (int32)ARRAY_SIZE(gChannels); i++){
		if(gChannels[i].voice){
			AESND_FreeVoice(gChannels[i].voice);
			gChannels[i].voice = nil;
		}
		if(gChannels[i].pcmOwned){
			gConvBytes -= gChannels[i].allocBytes;
			free(gChannels[i].pcm);
		}
		gChannels[i].pcm = nil;
		gChannels[i].pcmBytes = 0;
		gChannels[i].allocBytes = 0;
		gChannels[i].pcmOwned = FALSE;
	}
	for(uint32 i = 0; i < ARRAY_SIZE(gFrontendPcm); i++){
		if(gFrontendPcm[i].pcm){
			gConvBytes -= gFrontendPcm[i].allocBytes;
			free(gFrontendPcm[i].pcm);
		}
		memset(&gFrontendPcm[i], 0, sizeof(gFrontendPcm[i]));
	}
	// Streams too: a later Initialise re-runs AESND_Init and a held voice
	// pointer from this life would dangle.
	gcStreamsShutdown();
	free(gSampleIndex);
	gSampleIndex = nil;
	AESND_Pause(true);
	_bSampmanInitialised = FALSE;
}

// ------------------------------------------------------------------- banks

bool8
cSampleManager::InitialiseSampleBanks(void)
{
	// Every file call in this backend runs under the same lock the streaming
	// worker holds. libfat is one shared resource with no locking of its own,
	// and audio is the only user that reads from disc outside CdStream — the
	// two threads racing inside libfat is what corrupts the card.
	DVD_FS_GUARD;
	// sfx.sdt is a flat array of tSample. Reading it whole costs 200KB and
	// removes a disc seek from every single lookup afterwards.
	FILE *f = fopen("dvd:/audio/sfx.sdt", "rb");
	if(f == nil)
		return FALSE;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	gNumSamples = (uint32)(len/sizeof(tSample));
	if(gNumSamples > TOTAL_AUDIO_SAMPLES) gNumSamples = TOTAL_AUDIO_SAMPLES;
	gSampleIndex = m_aSamples;   // the 200K static the PC backend fills; this was a second malloc'd copy
	if(fread(gSampleIndex, sizeof(tSample), gNumSamples, f) != gNumSamples){
		gSampleIndex = nil; fclose(f); return FALSE;
	}
	fclose(f);

	// sfx.sdt is the PC file: little-endian throughout. Read raw on the
	// big-endian Gekko every offset, size and frequency is garbage — the
	// measured symptom was bank 0 sizing itself at 1.58GB of a 340MB file,
	// so no bank ever loaded and every channel effect was silent.
	for(uint32 i = 0; i < gNumSamples; i++){
		uint32 *w = (uint32*)&gSampleIndex[i];
		for(uint32 j = 0; j < sizeof(tSample)/4; j++)
			w[j] = __builtin_bswap32(w[j]);
	}

	// Prefer the exact, losslessly packed bank used by the mini-DVD. Cards
	// built before it existed keep working through sfx.raw.
	gPackedSfx = FALSE;
	gPackedSfxBytes = 0;
	gPackedSfxDataStart = 0;
	FILE *packed = fopen("dvd:/audio/sfx.pak", "rb");
	if(packed){
		uint8 header[GC_SFX_PACK_HEADER];
		bool8 valid = fread(header, 1, sizeof(header), packed) == sizeof(header) &&
		    memcmp(header, gSfxPackMagic, sizeof(gSfxPackMagic)) == 0 &&
		    gcReadBe32(header + 8) == gNumSamples;
		gPackedSfxDataStart = valid ? gcReadBe32(header + 12) : 0;
		if(fseek(packed, 0, SEEK_END) == 0)
			gPackedSfxBytes = (uint32)ftell(packed);
		else
			valid = FALSE;
		fclose(packed);
		uint32 tableEnd = GC_SFX_PACK_HEADER + gNumSamples*GC_SFX_PACK_ENTRY;
		if(!valid || gPackedSfxDataStart < tableEnd ||
		   gPackedSfxDataStart > gPackedSfxBytes)
			return FALSE;
		gPackedSfx = TRUE;
	}

	BankStartOffset[SFX_BANK_0] = 0;
	return TRUE;
}

// The resident bank as IMA ADPCM (tools/gamecube/pack_sfx_adpcm.py): the
// same 512-byte blocks gcWavDecode reads for VOICE, one run per sample in
// ARAM. Sample rates and loop points come from sfx.sdt exactly as before:
// the decode yields the PCM sample count, so nothing downstream changes.
static uint32 gBankAdpcmAddr[SAMPLEBANK_PED_START];
static uint32 gBankAdpcmBytes[SAMPLEBANK_PED_START];
static bool8  gBankAdpcm;
static const uint8 gSfxAdpcmMagic[8] = { 'G','C','S','F','X','A','1',0 };

static bool8
gcLoadAdpcmBank(void)
{
	if(gBanks[0].loaded)
		return TRUE;
	uint8 hdr[16];
	uint8 *table = nil;
	uint32 count = 0, dataStart = 0, fileBytes = 0, addr = 0, bytes = 0;
	bool8 ok = FALSE;
	{
		DVD_FS_GUARD;
		FILE *f = fopen("dvd:/audio/sfx.adp", "rb");
		if(f == nil)
			return FALSE;
		ok = fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
		    memcmp(hdr, gSfxAdpcmMagic, sizeof(gSfxAdpcmMagic)) == 0;
		if(ok){
			count = gcReadBe32(hdr + 8);
			dataStart = gcReadBe32(hdr + 12);
			ok = count >= SAMPLEBANK_PED_START && count <= gNumSamples;   // B123: == SAMPLEBANK_PED_START (resident only) or every sample
		}
		if(ok){
			table = (uint8*)malloc(count*8);
			ok = table != nil && fread(table, 1, count*8, f) == count*8 &&
			    fseek(f, 0, SEEK_END) == 0;
		}
		if(ok){
			fileBytes = (uint32)ftell(f);
			ok = fileBytes > dataStart;
			bytes = align32(fileBytes - dataStart);
			if(ok && count > SAMPLEBANK_PED_START){   // B123: ARAM gets the resident samples only
				uint32 pedStart = gcReadBe32(table + SAMPLEBANK_PED_START*8);
				if(pedStart > dataStart && pedStart <= fileBytes) bytes = align32(pedStart - dataStart);
			}
		}
		if(ok){
			addr = gcBankAlloc(bytes);
			ok = addr != 0;
		}
		if(ok){
			enum { STAGE = 64*1024 };
			uint8 *stage = (uint8*)memalign(32, STAGE);
			ok = stage != nil && fseek(f, (long)dataStart, SEEK_SET) == 0;
			for(uint32 done = 0; ok && done < bytes; ){
				uint32 chunk = bytes - done > STAGE ? STAGE : bytes - done;
				size_t got = fread(stage, 1, chunk, f);
				if(got == 0){ ok = FALSE; break; }
				if(got < chunk) memset(stage + got, 0, chunk - got);   // file tail vs 32-alignment
				gcBankWrite(addr + done, stage, chunk);
				done += chunk;
			}
			free(stage);
		}
		fclose(f);
	}
	if(ok){
		for(uint32 i = 0; i < count && i < SAMPLEBANK_PED_START; i++){
			uint32 off = gcReadBe32(table + i*8);
			gBankAdpcmAddr[i] = addr + (off - dataStart);
			gBankAdpcmBytes[i] = gcReadBe32(table + i*8 + 4);
			gBankSampleAddr[i] = gBankAdpcmAddr[i];
		}
		gAdpDataStart = dataStart;
		gAdpAll = count >= gNumSamples;
		if(gAdpAll && gcReadBe32(table + (count-1)*8) != gcAdpOffset(count-1)){
			printf("BANK adp: implied offset of sample %u disagrees with the table; ped comments stay on sfx.raw\n", (unsigned)count-1);
			gAdpAll = FALSE;
		}
		gBanks[0].aramAddr = addr;
		gBanks[0].bytes = bytes;
		gBanks[0].loaded = TRUE;
		gBankAdpcm = TRUE;
		printf("BANK 0 adpcm %uK in ARAM at %08x, pack holds %u samples%s\n", (unsigned)(bytes/1024), (unsigned)addr, (unsigned)count, gAdpAll ? " (all: ped comments from sfx.adp)" : "");
	}
	free(table);
	return ok;
}

bool8
cSampleManager::LoadSampleBank(uint8 nBank)
{
	if(nBank >= MAX_SFX_BANKS || gSampleIndex == nil)
		return FALSE;
	if(nBank == 0 && gcLoadAdpcmBank())
		return TRUE;    // else the PCM bank from sfx.raw / sfx.pak, as before
	if(gBanks[nBank].loaded)
		return TRUE;

	// A bank is the run of samples from its start offset to the next bank's.
	// The resident bank ends where the ped-comment region begins: everything
	// from SAMPLEBANK_PED_START up is streamed per-sample into the rotating
	// ped slots (see InitialiseChannel's routing), never served from here.
	// Falling back to gNumSamples made "bank 0" span all of sfx.raw — 340MB,
	// which ARAM cannot hold — instead of its real 14MB.
	uint32 first = BankStartOffset[nBank];
	uint32 last = nBank+1 < MAX_SFX_BANKS && BankStartOffset[nBank+1] ?
	    BankStartOffset[nBank+1] : SAMPLEBANK_PED_START;
	if(first >= gNumSamples)
		return FALSE;
	if(last > gNumSamples)
		last = gNumSamples;

	uint32 byteStart = gSampleIndex[first].nOffset;
	uint32 byteEnd = gSampleIndex[last-1].nOffset + gSampleIndex[last-1].nSize;
	uint32 bytes = align32(byteEnd - byteStart);

	// gcBankAlloc is an ARAM stack: UnloadSampleBank cannot return memory, so
	// an unload/reload cycle (audio Terminate/Initialise around a cutscene
	// skip) must reuse the old allocation or the second alloc of a 14MB bank
	// exhausts the pool and sound never comes back.
	uint32 addr;
	if(gBanks[nBank].aramAddr && gBanks[nBank].bytes >= bytes)
		addr = gBanks[nBank].aramAddr;
	else{
		addr = gcBankAlloc(bytes);
		{
			// To the card, not gecko: the gecko capture truncates lines.
			DVD_FS_GUARD;
			char bl[96];
			snprintf(bl, sizeof(bl), "BANK %s %uK aram addr=%08x\n",
			    addr ? "ok" : "FAIL", (unsigned)(bytes/1024), (unsigned)addr);
		}
		if(addr == 0){
			return FALSE;
		}
	}

	// Stream through a small staging buffer into audio memory. The staging
	// buffer is one transfer, not the whole bank — the point of ARAM is that
	// 14.3MB never has to sit in MEM1.
	enum { STAGE = 64*1024 };
	uint8 *stage = (uint8*)memalign(32, STAGE);
	if(stage == nil){ return FALSE; }
	bool8 loaded = TRUE;
	{
		DVD_FS_GUARD;
		FILE *raw = fopen(gPackedSfx ? "dvd:/audio/sfx.pak" :
		                              "dvd:/audio/sfx.raw", "rb");
		if(raw == nil)
			loaded = FALSE;
		else{
			uint32 fileOffset = byteStart;
			if(gPackedSfx){
				uint8 firstEntry[GC_SFX_PACK_ENTRY] = {0};
				uint8 lastEntry[GC_SFX_PACK_ENTRY] = {0};
				loaded = fseek(raw, GC_SFX_PACK_HEADER + first*GC_SFX_PACK_ENTRY,
				                  SEEK_SET) == 0 &&
				    fread(firstEntry, 1, sizeof(firstEntry), raw) == sizeof(firstEntry) &&
				    fseek(raw, GC_SFX_PACK_HEADER + (last-1)*GC_SFX_PACK_ENTRY,
				          SEEK_SET) == 0 &&
				    fread(lastEntry, 1, sizeof(lastEntry), raw) == sizeof(lastEntry);
				uint32 firstOffset = gcReadBe32(firstEntry);
				uint32 firstFlags = gcReadBe32(firstEntry + 4);
				uint32 lastOffset = gcReadBe32(lastEntry);
				uint32 lastFlags = gcReadBe32(lastEntry + 4);
				loaded = loaded && (firstFlags & 0x80000000u) &&
				    (lastFlags & 0x80000000u) &&
				    (firstFlags & 0x7FFFFFFFu) == gSampleIndex[first].nSize &&
				    (lastFlags & 0x7FFFFFFFu) == gSampleIndex[last-1].nSize &&
				    firstOffset == gPackedSfxDataStart + byteStart &&
				    lastOffset + (lastFlags & 0x7FFFFFFFu) ==
				        gPackedSfxDataStart + byteEnd;
				fileOffset = firstOffset;
			}
			loaded = loaded && fseek(raw, (long)fileOffset, SEEK_SET) == 0;
			for(uint32 done = 0; loaded && done < bytes; ){
				uint32 chunk = bytes - done > STAGE ? STAGE : align32(bytes - done);
				size_t got = fread(stage, 1, chunk, raw);
				if(got != chunk){ loaded = FALSE; break; }
				// sfx.raw is little-endian; AESND reads big-endian PCM.
				for(uint32 b = 0; b + 1 < chunk; b += 2){
					uint8 t = stage[b]; stage[b] = stage[b+1]; stage[b+1] = t;
				}
				gcBankWrite(addr + done, stage, chunk);
				done += chunk;
			}
			fclose(raw);
		}
	}
	free(stage);
	if(!loaded)
		return FALSE;

	// The run is contiguous, so a sample's address is its file offset
	// rebased onto the bank.
	for(uint32 i = first; i < last; i++)
		gBankSampleAddr[i] = addr + (gSampleIndex[i].nOffset - byteStart);

	gBanks[nBank].aramAddr = addr;
	gBanks[nBank].bytes = bytes;
	gBanks[nBank].loaded = TRUE;
	return TRUE;
}

void
cSampleManager::UnloadSampleBank(uint8 nBank)
{
	if(nBank >= MAX_SFX_BANKS)
		return;
	// AR_Alloc is a stack allocator, so a bank can only be released when it is
	// the most recent one. Marking it unloaded is enough for the game's
	// purposes; the ARAM is reclaimed when the stack unwinds to it.
	gBanks[nBank].loaded = FALSE;
}

int8
cSampleManager::IsSampleBankLoaded(uint8 nBank)
{
	return nBank < MAX_SFX_BANKS && gBanks[nBank].loaded ? LOADING_STATUS_LOADED
	                                                     : LOADING_STATUS_NOT_LOADED;
}

int32
cSampleManager::GetBankContainingSound(uint32 offset)
{
	for(int32 i = MAX_SFX_BANKS-1; i >= 0; i--)
		if(offset >= BankStartOffset[i])
			return i;
	return SFX_BANK_0;
}

// ------------------------------------------------------------------ samples

uint32
cSampleManager::GetSampleBaseFrequency(uint32 nSample)
{
	return gSampleIndex && nSample < gNumSamples ?
	    gSampleIndex[nSample].nFrequency : 22050;
}

uint32
cSampleManager::GetSampleLength(uint32 nSample)
{
	// In samples, not bytes: the bank is 16-bit mono.
	return gSampleIndex && nSample < gNumSamples ?
	    gSampleIndex[nSample].nSize/2 : 0;
}

uint32
cSampleManager::GetSampleLoopStartOffset(uint32 nSample)
{
	return gSampleIndex && nSample < gNumSamples ?
	    gSampleIndex[nSample].nLoopStart : 0;
}

int32
cSampleManager::GetSampleLoopEndOffset(uint32 nSample)
{
	return gSampleIndex && nSample < gNumSamples ?
	    gSampleIndex[nSample].nLoopEnd : -1;
}

// ----------------------------------------------------------------- channels

// Reconstruction filter for the per-play conversion.
//
// Linear interpolation was not enough: measured against the same sample
// decoded on a host, an 8kHz effect still carried 4.4x the reference's
// energy above its own Nyquist. Linear only attenuates the spectral images
// an upsample creates (a triangular kernel, sinc-squared rolloff) - it does
// not remove them, and what is left is audible as the metallic edge.
//
// A windowed sinc does remove them. 8 taps over 64 sub-phases: 2KB of table,
// built once, and one 8-term dot product per output sample - a few hundred
// microseconds for a typical effect, paid on the play that needs it.
enum { GC_FIR_TAPS = 8, GC_FIR_PHASES = 64 };
static f32 gFirTable[GC_FIR_PHASES][GC_FIR_TAPS];
static int16 gFirTabI[GC_FIR_PHASES][GC_FIR_TAPS];   // Q15 copy: integer MAC, 3-4x cheaper than the float loop on the 750
static bool8 gFirReady;

static void
gcBuildFir(void)
{
	if(gFirReady)
		return;
	for(int32 ph = 0; ph < GC_FIR_PHASES; ph++){
		f32 frac = (f32)ph/(f32)GC_FIR_PHASES;
		f32 sum = 0.0f;
		for(int32 t = 0; t < GC_FIR_TAPS; t++){
			// Distance from this tap to the point being reconstructed.
			f32 x = (f32)(t - (GC_FIR_TAPS/2 - 1)) - frac;
			f32 s;
			if(x > -1e-6f && x < 1e-6f)
				s = 1.0f;
			else{
				f32 pix = (f32)M_PI*x;
				s = sinf(pix)/pix;
			}
			// Blackman window over the tap span keeps the stopband down
			// without ringing the transients every gunshot is made of.
			f32 w = 0.42f - 0.5f*cosf((f32)(2.0*M_PI)*((f32)t + 0.5f)/(f32)GC_FIR_TAPS)
			      + 0.08f*cosf((f32)(4.0*M_PI)*((f32)t + 0.5f)/(f32)GC_FIR_TAPS);
			gFirTable[ph][t] = s*w;
			sum += s*w;
		}
		// Unity gain at DC for every phase, so the level never wobbles.
		if(sum > 0.0001f || sum < -0.0001f)
			for(int32 t = 0; t < GC_FIR_TAPS; t++)
				gFirTable[ph][t] /= sum;
		for(int32 t = 0; t < GC_FIR_TAPS; t++){
			f32 q = gFirTable[ph][t]*32768.0f;
			gFirTabI[ph][t] = (int16)(q > 32767.0f ? 32767.0f : q < -32768.0f ? -32768.0f : q + (q >= 0.0f ? 0.5f : -0.5f));
		}
	}
	gFirReady = TRUE;
}

static void
gcDiscardChannelPcm(GcChannel *c)
{
	if(c->pcmOwned){
		gConvBytes -= c->allocBytes;
		gcPcmFree(c->pcm);
	}
	c->pcm = nil;
	c->pcmBytes = 0;
	c->allocBytes = 0;
	c->pcmOwned = FALSE;
	c->pcm48 = FALSE;
	c->pcmFreq = 0;
}

static void
gcReleaseIdleFrontendPcm(void)
{
	for(uint32 i = 0; i < ARRAY_SIZE(gFrontendPcm); i++){
		GcSharedPcm *shared = &gFrontendPcm[i];
		if(shared->pcm == nil)
			continue;
		bool8 active = FALSE;
		for(uint32 ch = 0; ch < ARRAY_SIZE(gChannels); ch++)
			if(gChannels[ch].playing && gChannels[ch].pcm == shared->pcm){
				active = TRUE;
				break;
			}
		if(active)
			continue;
		for(uint32 ch = 0; ch < ARRAY_SIZE(gChannels); ch++)
			if(gChannels[ch].pcm == shared->pcm){
				gChannels[ch].pcm = nil;
				gChannels[ch].pcmBytes = 0;
				gChannels[ch].pcm48 = FALSE;
				gChannels[ch].pcmFreq = 0;
			}
		gConvBytes -= shared->allocBytes;
		gcPcmFree(shared->pcm);
		memset(shared, 0, sizeof(*shared));
	}
}

// A stopped voice cannot read its buffer again. Evict those cached copies
// before accepting AESND's metallic native-rate fallback.
static void
gcMakeConversionRoom(GcChannel *keep, uint32 need)
{
	uint32 held = keep->pcmOwned ? keep->allocBytes : 0;
	if(need <= held || gConvBytes - held + need <= GC_CONV_BUDGET)
		return;
	for(uint32 i = 0; i < ARRAY_SIZE(gChannels); i++){
		GcChannel *c = &gChannels[i];
		if(c != keep && !c->playing && c->pcmOwned)
			gcDiscardChannelPcm(c);
		held = keep->pcmOwned ? keep->allocBytes : 0;
		if(gConvBytes - held + need <= GC_CONV_BUDGET)
			return;
	}
}

// Prepare the sample only after SetChannelFrequency has supplied the pitch.
// The old path converted at the file's base rate, then asked AESND to play the
// result at 61-70kHz for pitched effects — which simply re-enabled the DSP's
// sample-repeat resampler. Bake the requested pitch into the FIR conversion
// and hand AESND a 1:1 DSP-rate buffer instead. This also covers ped/player
// speech, which used to bypass conversion entirely.
static inline int16 gcImaNibble(uint8 nib, int32 *pred, int32 *idx);

// ADPCM bank: DMA a few blocks at a time from ARAM and decode straight into
// the channel's PCM buffer — the same buffer the resampler reads next.
static void
gcBankDecodeAdpcm(uint32 nSfx, int16 *dst, uint32 samples)
{
	static uint8 stage[4096] __attribute__((aligned(32)));
	uint32 addr = gBankAdpcmAddr[nSfx], bytes = gBankAdpcmBytes[nSfx];
	uint32 done = 0, out = 0;
	while(done < bytes && out < samples){
		uint32 chunk = bytes - done > sizeof(stage) ? (uint32)sizeof(stage) : align32(bytes - done);
		gcBankRead(stage, addr + done, chunk);
		for(uint32 b = 0; b + 512 <= chunk && out < samples; b += 512){
			const uint8 *blk = stage + b;
			int32 pred = (int16)((uint16)blk[0] | ((uint16)blk[1] << 8));
			int32 idx = blk[2];
			if(idx > 88) idx = 88;
			dst[out++] = (int16)pred;
			for(uint32 i = 4; i < 512 && out < samples; i++){
				dst[out++] = gcImaNibble(blk[i] & 15, &pred, &idx);
				if(out < samples)
					dst[out++] = gcImaNibble(blk[i] >> 4, &pred, &idx);
			}
		}
		done += chunk;
	}
	while(out < samples)
		dst[out++] = 0;
}

// ---------------------------------------------------------------- ARAM voices
//
// "Usa ARAM" (user, 09-01): a bank sample never sits whole in MEM1 any more.
// Each voice streams it — 512-byte ADPCM blocks DMA'd from ARAM one at a
// time, decoded into a small ring, FIR-resampled to the DSP rate into a pair
// of 48ms chunks the AESND callback swaps, exactly the pump the radio streams
// use. MEM1 per voice is the struct below (~14K); the FIR's quality stays
// (AESND's ucode resamples by sample-repeat, which is the gravel on speech
// heard in B38/B39). Loop points are source-sample positions: a jump resets
// the decode cursor to the block holding loopStart — every block is
// self-contained, which is why the bank uses this block format.
enum { GC_VCHUNK = 1152*4 };   // 48ms: 24ms starved 7 chunks in 3 min (B48) once the decode thread also had three Vorbis streams to feed
enum { GC_VBLOCK = 512, GC_VBLOCK_SAMPLES = 1017, GC_VRING_HIST = 8 };
struct GcVoiceStream {
	GcChannel *chan;
	volatile bool8 active;
	uint32  aram, adpcmBytes;
	uint32  totalSamples;
	uint32  loopStart, loopEnd;
	bool8   looping;
	uint32  srcRate;
	bool8   pcm16;            // 16-bit PCM blocks (ped comments) rather than ADPCM
	uint32  blockBytes, blockSamples;   // 512/1017 for ADPCM, 1024/512 for PCM16
	uint32  nextBlock;
	uint32  ringCount;
	uint32  ringSrcBase;      // source index of ring[GC_VRING_HIST]
	int16   ring[GC_VRING_HIST + GC_VBLOCK_SAMPLES*2];
	uint64  pos;              // 16.16 source position
	int32   fill, play;
	volatile bool8 bufReady, eof;
	volatile uint32 starved;
	uint8   blk[1024] __attribute__((aligned(32)));
	uint8   pcm[2][GC_VCHUNK] __attribute__((aligned(32)));
};
static GcVoiceStream gVoiceStreams[GC_CHANNEL_VOICES];
static mutex_t gVoiceLock = LWP_MUTEX_NULL;
unsigned gVoiceStarvedTotal, gVoicePumps;

// Every channel that got an AESND voice gets a stream slot (Initialise).
static void
gcVoiceSlotsInit(void)
{
	if(gVoiceLock == LWP_MUTEX_NULL)
		LWP_MutexInit(&gVoiceLock, true);
	int32 k = 0;
	for(uint32 i = 0; i < ARRAY_SIZE(gChannels) && k < GC_CHANNEL_VOICES; i++)
		if(gChannels[i].voice)
			gChannels[i].vs = &gVoiceStreams[k++];
}

static void
gcVoiceDecodeBlock(GcVoiceStream *vs)
{
	uint32 bs = vs->blockSamples, bb = vs->blockBytes;
	if(vs->ringCount >= bs*2){
		// Drop the front block; its last GC_VRING_HIST samples become the history.
		memmove(vs->ring, vs->ring + bs,
		    (GC_VRING_HIST + vs->ringCount - bs)*sizeof(int16));
		vs->ringSrcBase += bs;
		vs->ringCount -= bs;
	}
	uint32 off = vs->nextBlock*bb;
	if(off >= vs->adpcmBytes)
		return;
	gcBankRead(vs->blk, vs->aram + off, bb);
	uint32 first = vs->nextBlock*bs;
	uint32 n = vs->totalSamples > first ? vs->totalSamples - first : 0;
	if(n > bs) n = bs;
	int16 *dst = vs->ring + GC_VRING_HIST + vs->ringCount;
	uint32 out = 0;
	if(vs->pcm16){
		memcpy(dst, vs->blk, n*sizeof(int16));   // big-endian int16 in ARAM = native
		out = n;
	}else{
		int32 pred = (int16)((uint16)vs->blk[0] | ((uint16)vs->blk[1] << 8));
		int32 idx = vs->blk[2];
		if(idx > 88) idx = 88;
		if(out < n) dst[out++] = (int16)pred;
		for(uint32 i = 4; i < GC_VBLOCK && out < n; i++){
			dst[out++] = gcImaNibble(vs->blk[i] & 15, &pred, &idx);
			if(out < n) dst[out++] = gcImaNibble(vs->blk[i] >> 4, &pred, &idx);
		}
	}
	if(vs->ringCount == 0)
		vs->ringSrcBase = first;
	vs->ringCount += out;
	vs->nextBlock++;
}

static void
gcVoiceSeek(GcVoiceStream *vs, uint32 si)
{
	vs->nextBlock = si / vs->blockSamples;
	vs->ringCount = 0;
	vs->ringSrcBase = vs->nextBlock*vs->blockSamples;
	memset(vs->ring, 0, GC_VRING_HIST*sizeof(int16));
}

// Fill the free chunk: 2304 output samples at the DSP rate from the source.
static void
gcVoicePump(GcVoiceStream *vs)
{
	if(!vs->active || vs->bufReady || vs->eof)
		return;
	int16 *out = (int16*)vs->pcm[vs->fill];
	const uint32 outS = GC_VCHUNK/2;
	uint32 step = (vs->srcRate << 16)/GC_DSP_RATE;
	if(step == 0) step = 1;
	gcBuildFir();
	uint32 k = 0;
	for(; k < outS; k++){
		uint32 si = (uint32)(vs->pos >> 16);
		if(vs->looping && si >= vs->loopEnd){
			vs->pos -= ((uint64)(vs->loopEnd - vs->loopStart)) << 16;
			si = (uint32)(vs->pos >> 16);
			gcVoiceSeek(vs, si);
		}else if(!vs->looping && si + 1 >= vs->totalSamples){
			break;
		}
		while(si + GC_FIR_TAPS/2 + 1 > vs->ringSrcBase + vs->ringCount &&
		      vs->nextBlock*vs->blockBytes < vs->adpcmBytes)
			gcVoiceDecodeBlock(vs);
		if(vs->ringCount == 0)
			break;
		int32 base = (int32)(si - vs->ringSrcBase);
		const f32 *tap = gFirTable[(vs->pos >> 10) & (GC_FIR_PHASES-1)];
		f32 acc = 0.0f;
		for(int32 t = 0; t < GC_FIR_TAPS; t++){
			int32 i = base + t - (GC_FIR_TAPS/2 - 1);
			if(i < -GC_VRING_HIST) i = -GC_VRING_HIST;
			else if(i >= (int32)vs->ringCount) i = (int32)vs->ringCount - 1;
			acc += tap[t]*(f32)vs->ring[GC_VRING_HIST + i];
		}
		int32 v = (int32)(acc + (acc >= 0.0f ? 0.5f : -0.5f));
		if(v > 32767) v = 32767; else if(v < -32768) v = -32768;
		out[k] = (int16)v;
		vs->pos += step;
	}
	if(k < outS){
		memset(out + k, 0, (outS - k)*sizeof(int16));
		vs->eof = TRUE;
	}
	DCFlushRange(vs->pcm[vs->fill], GC_VCHUNK);
	vs->fill ^= 1;
	vs->bufReady = TRUE;
	gVoicePumps++;
}

static void
gcVoiceStop(GcVoiceStream *vs)
{
	if(vs == nil) return;
	LWP_MutexLock(gVoiceLock);
	vs->active = FALSE;
	vs->bufReady = FALSE;
	LWP_MutexUnlock(gVoiceLock);
}

// Called from StartChannel: the channel's prepare left the ARAM run in vs.
static void
gcVoiceArm(GcChannel *c, bool8 looping)
{
	GcVoiceStream *vs = c->vs;
	gMainWhere = "voice-arm";
	LWP_MutexLock(gVoiceLock);
	if(c->voice) AESND_SetVoiceStop(c->voice, true);   // the DSP may still run the old stream while vs is rewritten
	vs->chan = c;
	vs->looping = looping;
	vs->loopStart = c->loopStart < vs->totalSamples ? c->loopStart : 0;
	vs->loopEnd = c->loopEnd > 0 && (uint32)c->loopEnd <= vs->totalSamples ? (uint32)c->loopEnd : vs->totalSamples;
	if(vs->loopEnd <= vs->loopStart){ vs->loopStart = 0; vs->loopEnd = vs->totalSamples; }
	vs->pos = 0;
	vs->nextBlock = 0; vs->ringCount = 0; vs->ringSrcBase = 0;
	memset(vs->ring, 0, GC_VRING_HIST*sizeof(int16));
	vs->fill = vs->play = 0;
	vs->bufReady = FALSE; vs->eof = FALSE; vs->starved = 0;
	vs->active = TRUE;
	gcVoicePump(vs);                       // chunk 0
	// The pump owns looping (source loop points); AESND's own loop flag
	// would repeat the first 48ms chunk forever and never call back.
	const void *first = vs->pcm[vs->play];
	if(vs->bufReady){
		vs->play ^= 1;
		vs->bufReady = FALSE;
		gcVoicePump(vs);                   // chunk 1 waits for the first callback
	}
	LWP_MutexUnlock(gVoiceLock);
	c->playing = TRUE;
	gcPlayVoice(c->voice, VOICE_MONO16, first, GC_VCHUNK, GC_DSP_RATE_F, TRUE, FALSE);
}

static void
gcVoiceStreamCallback(AESNDPB *pb, GcChannel *c)
{
	gAudioCbTick++;
	GcVoiceStream *vs = c->vs;
	if(vs == nil || !vs->active){
		c->playing = FALSE;
		AESND_SetVoiceStop(pb, true);
		return;
	}
	if(vs->bufReady){
		AESND_SetVoiceBuffer(pb, vs->pcm[vs->play], GC_VCHUNK);
		vs->play ^= 1;
		vs->bufReady = FALSE;
	}else if(vs->eof){
		vs->active = FALSE;
		c->playing = FALSE;
		AESND_SetVoiceStop(pb, true);
	}else{
		static uint8 gVoiceSilence[GC_VCHUNK] __attribute__((aligned(32)));
		AESND_SetVoiceBuffer(pb, gVoiceSilence, GC_VCHUNK);
		vs->starved++;
		gVoiceStarvedTotal++;
	}
}

static bool8
gcPrepareChannel(GcChannel *c, uint32 nChannel)
{
	uint32 nSfx = c->sample;
	uint32 rawBytes = gSampleIndex[nSfx].nSize;
	uint32 baseFreq = gSampleIndex[nSfx].nFrequency ?
	    gSampleIndex[nSfx].nFrequency : 22050;
	uint32 targetFreq = c->freq ? c->freq : baseFreq;
	uint32 inS = rawBytes/2;
	uint32 outS = targetFreq < GC_DSP_RATE ?
	    (uint32)((uint64)inS*GC_DSP_RATE/targetFreq) : inS;
	uint32 outBytes = align32(outS*2);
	uint32 srcSkew = 0;
	uint32 readBytes = align32(rawBytes);
	uint32 srcAddr = 0;
	const uint8 *memSrc = nil;
	char d[64];
	GcSharedPcm *shared = nSfx >= SFX_INFO_LEFT && nSfx <= SFX_FE_ERROR_RIGHT ?
	    &gFrontendPcm[nSfx - SFX_INFO_LEFT] : nil;

	c->streamed = FALSE;
	if(shared && shared->pcm && shared->freq == targetFreq){
		gcDiscardChannelPcm(c);
		c->pcm = shared->pcm;
		c->pcmBytes = shared->bytes;
		c->pcm48 = TRUE;
		c->pcmFreq = targetFreq;
		gConvOk++;
		return TRUE;
	}

	if(nSfx < SAMPLEBANK_PED_START && gBankAdpcm && c->vs){
		// ARAM voice: nothing to allocate here; StartChannel arms the stream.
		gcDiscardChannelPcm(c);
		GcVoiceStream *vs = c->vs;
		gcVoiceStop(vs);
		vs->aram = gBankAdpcmAddr[nSfx];
		vs->adpcmBytes = gBankAdpcmBytes[nSfx];
		vs->totalSamples = inS;
		vs->srcRate = targetFreq;
		vs->pcm16 = FALSE; vs->blockBytes = GC_VBLOCK; vs->blockSamples = GC_VBLOCK_SAMPLES;
		c->pcm48 = TRUE;          // 1:1 with the DSP; pitch changes scale from pcmFreq
		c->pcmFreq = targetFreq;
		c->pcmBytes = 0;
		c->streamed = TRUE;
		gConvOk++;
		return TRUE;
	}
	if(nSfx < SAMPLEBANK_PED_START){
		srcAddr = gBankSampleAddr[nSfx];
		srcSkew = srcAddr & 31;
		readBytes = align32(srcSkew + rawBytes);
	}else if(nSfx == gPlayerTalkSfx && gPlayerTalkData)
		memSrc = gPlayerTalkData;
	else{
		int32 slot = SampleManager._GetPedCommentSlot(nSfx);
		if(slot >= 0 && gPedAram && c->vs){
			gcDiscardChannelPcm(c);
			GcVoiceStream *vs = c->vs;
			gcVoiceStop(vs);
			vs->aram = gPedAram + gPedSlotStride*slot;
			vs->totalSamples = inS;
			vs->srcRate = targetFreq;
			if(gPedSlotAdpcm[slot]){ vs->pcm16 = FALSE; vs->blockBytes = GC_VBLOCK; vs->blockSamples = GC_VBLOCK_SAMPLES; vs->adpcmBytes = gPedSlotBytes[slot]; }
			else { vs->pcm16 = TRUE; vs->blockBytes = 1024; vs->blockSamples = 512; vs->adpcmBytes = align32(rawBytes); }
			c->pcm48 = TRUE;
			c->pcmFreq = targetFreq;
			c->pcmBytes = 0;
			c->streamed = TRUE;
			gConvOk++;
			return TRUE;
		}
		if(slot < 0 || gPedBuf == nil){
			snprintf(d, sizeof(d), "sfx=%u slot=%d", (unsigned)nSfx, (int)slot);
			gcAudioDie("ped-comment-not-loaded", d);
			return FALSE;
		}
		memSrc = gPedBuf + PED_BLOCKSIZE*slot;
	}

	bool8 resample = targetFreq < GC_DSP_RATE && inS >= 2 &&
	                   outBytes <= GC_CH_RESAMPLE_CAP;
	if(resample){
		uint32 need = outBytes + 64;
		gcMakeConversionRoom(c, need);
		// No fallback (user, B37): the budget is advisory — gcMakeConversionRoom
		// already returned every idle buffer; a voice either resamples or, if
		// the allocation below fails, does not play. Native-pitch PCM never.
		(void)c->pcmOwned;
	}
	uint32 want = resample ? outBytes + 64 : readBytes;
	if(want < readBytes)
		want = readBytes;

	if(!c->pcmOwned){
		c->pcm = nil;
		c->allocBytes = 0;
	}
	if(c->allocBytes < want){
		gcDiscardChannelPcm(c);
		c->pcm = gcPcmAlloc(want);
		if(c->pcm == nil){
			// Fragmented heap (B33: a 94K request refused at 2MB free): return
			// every idle channel's buffer and ask once more.
			for(uint32 i = 0; i < ARRAY_SIZE(gChannels); i++)
				if(&gChannels[i] != c && !gChannels[i].playing && gChannels[i].pcmOwned)
					gcDiscardChannelPcm(&gChannels[i]);
			gcReleaseIdleFrontendPcm();
			c->pcm = gcPcmAlloc(want);
		}
		c->allocBytes = c->pcm ? want : 0;
		c->pcmOwned = c->pcm != nil;
		gConvBytes += c->allocBytes;
	}
	if(c->pcm == nil){
		snprintf(d, sizeof(d), "ch=%u %uB", (unsigned)nChannel, (unsigned)want);
		gcAudioDie("channel-pcm-alloc", d);
		return FALSE;
	}

	uint8 *base = (uint8*)c->pcm;
	uint32 tail = align32(want - readBytes);
	if(tail + readBytes > want)
		tail = 0;
	if(nSfx < SAMPLEBANK_PED_START){
		if(gBankAdpcm)
			gcBankDecodeAdpcm(nSfx, (int16*)(base + tail + srcSkew), rawBytes/2);
		else
			gcBankRead(base + tail, srcAddr - srcSkew, readBytes);
	}else
		memcpy(base + tail, memSrc, rawBytes);
	const int16 *sp = (const int16*)(base + tail + srcSkew);

	if(resample){
		int16 *dst = (int16*)base;
		uint32 step = (targetFreq << 16)/GC_DSP_RATE;
		// 64-bit: in 32 bits this wrapped past ~285K of output at 22kHz and the
		// sample restarted from its first frame mid-buffer — the "double voice".
		uint64 pos = 0;
		gcBuildFir();
		for(uint32 k = 0; k < outS; k++, pos += step){
			int32 i0 = (int32)(pos >> 16);
			uint32 ph = (pos >> 10) & (GC_FIR_PHASES-1);
			const f32 *tap = gFirTable[ph];
			f32 acc = 0.0f;
			for(int32 t = 0; t < GC_FIR_TAPS; t++){
				int32 si = i0 + t - (GC_FIR_TAPS/2 - 1);
				if(si < 0) si = 0;
				else if(si >= (int32)inS) si = (int32)inS - 1;
				acc += tap[t]*(f32)sp[si];
			}
			int32 v = (int32)(acc + (acc >= 0.0f ? 0.5f : -0.5f));
			if(v > 32767) v = 32767;
			else if(v < -32768) v = -32768;
			dst[k] = (int16)v;
		}
		c->pcmBytes = outS*2;
		c->pcm48 = TRUE;
		c->pcmFreq = targetFreq;
		gConvOk++;
		if(gConvBytes > gConvPeak) gConvPeak = gConvBytes;
		if(shared && shared->pcm == nil){
			shared->pcm = c->pcm;
			shared->bytes = c->pcmBytes;
			shared->allocBytes = c->allocBytes;
			shared->freq = targetFreq;
			DCFlushRange(shared->pcm, shared->bytes);
			c->pcmOwned = FALSE; // cache owns it; voices only borrow it
			c->allocBytes = 0;
		}
	}else{
		if(tail || srcSkew)
			memmove(base, sp, rawBytes);
		c->pcmBytes = align32(rawBytes);
		c->pcm48 = FALSE;
		c->pcmFreq = 0;
		if(targetFreq != GC_DSP_RATE)
			gConvFallback++;
	}
	return TRUE;
}

bool8
cSampleManager::InitialiseChannel(uint32 nChannel, uint32 nSfx, uint8 nBank)
{
	char d[64];
	if(nChannel >= ARRAY_SIZE(gChannels) || gSampleIndex == nil ||
	   nSfx >= gNumSamples){
		snprintf(d, sizeof(d), "ch=%u sfx=%u", (unsigned)nChannel, (unsigned)nSfx);
		gcAudioDie("channel-request-bad", d);
		return FALSE;
	}
	GcChannel *c = &gChannels[nChannel];
	if(c->voice == nil){
		snprintf(d, sizeof(d), "ch=%u", (unsigned)nChannel);
		gcAudioDie("channel-no-voice", d);
		return FALSE;
	}
	if(gSampleIndex[nSfx].nSize == 0){
		snprintf(d, sizeof(d), "sfx=%u", (unsigned)nSfx);
		gcAudioDie("sample-zero-bytes", d);
		return FALSE;
	}

	if(nSfx < SAMPLEBANK_PED_START){
		nBank = SFX_BANK_0;
		if(!gBanks[nBank].loaded){
			snprintf(d, sizeof(d), "sfx=%u", (unsigned)nSfx);
			gcAudioDie("bank0-not-loaded", d);
			return FALSE;
		}
	}else{
		if(gSampleIndex[nSfx].nSize > PED_BLOCKSIZE){
			snprintf(d, sizeof(d), "sfx=%u %uB", (unsigned)nSfx,
			    (unsigned)gSampleIndex[nSfx].nSize);
			gcAudioDie("ped-sample-oversize", d);
			return FALSE;
		}
		if(nSfx != gPlayerTalkSfx || gPlayerTalkData == nil){
			int32 slot = _GetPedCommentSlot(nSfx);
			if(slot < 0 || (gPedBuf == nil && gPedAram == 0)){
				snprintf(d, sizeof(d), "sfx=%u slot=%d", (unsigned)nSfx, (int)slot);
				gcAudioDie("ped-comment-not-loaded", d);
				return FALSE;
			}
		}
	}

	AESND_SetVoiceStop(c->voice, true);
	gcVoiceStop(c->vs);
	c->playing = FALSE;
	c->sample = nSfx;
	c->freq = gSampleIndex[nSfx].nFrequency;
	c->pcmBytes = 0;       // prepared at StartChannel, after pitch is known
	c->pcm48 = FALSE;
	c->pcmFreq = 0;
	// Centre unless the game asks otherwise. The OAL backend discards pan
	// altogether (CChannel::SetPan only sets bForce2D; its positional line is
	// commented out as "kinda pointless"), so a sound that never calls
	// SetChannelPan is centred there. Here the field defaulted to 0, which
	// this backend reads as hard left: mono came out of one speaker.
	c->pan = 63;
	c->has3D = FALSE;    // until the game gives this play a position
	c->used = TRUE;
	return TRUE;
}

void
cSampleManager::SetChannelFrequency(uint32 nChannel, uint32 nFreq)
{
	if(nChannel >= ARRAY_SIZE(gChannels))
		return;
	GcChannel *c = &gChannels[nChannel];
	c->freq = nFreq;
	// Initial pitch is baked by gcPrepareChannel. Later engine/doppler
	// changes still have to reach a live voice; scale around that clean
	// prepared rate instead of around the file's unrelated base rate.
	if(c->playing && c->voice){
		f32 f = (f32)nFreq;
		if(c->pcm48 && c->pcmFreq)
			f = GC_DSP_RATE_F*(f32)nFreq/(f32)c->pcmFreq;
		AESND_SetVoiceFrequency(c->voice, f);
	}
}

void
cSampleManager::SetChannelVolume(uint32 nChannel, uint32 nVolume)
{
	if(nChannel < ARRAY_SIZE(gChannels))
		gChannels[nChannel].volume = nVolume;
}

void
cSampleManager::SetChannelEmittingVolume(uint32 nChannel, uint32 nVolume)
{
	SetChannelVolume(nChannel, nVolume);
}

void
cSampleManager::SetChannelPan(uint32 nChannel, uint32 nPan)
{
	if(nChannel < ARRAY_SIZE(gChannels))
		gChannels[nChannel].pan = nPan;
}

void
cSampleManager::SetChannelLoopCount(uint32 nChannel, uint32 nLoopCount)
{
	if(nChannel < ARRAY_SIZE(gChannels))
		gChannels[nChannel].loopCount = nLoopCount;
}

void
cSampleManager::SetChannelLoopPoints(uint32 nChannel, uint32 nLoopStart, int32 nLoopEnd)
{
	if(nChannel >= ARRAY_SIZE(gChannels))
		return;
	// The game passes byte offsets; the OAL backend divides by the sample
	// size for the same reason (DIGITALBITS/8).
	gChannels[nChannel].loopStart = nLoopStart/2;
	gChannels[nChannel].loopEnd = nLoopEnd < 0 ? -1 : nLoopEnd/2;
}

bool8
cSampleManager::GetChannelUsedFlag(uint32 nChannel)
{
	if(nChannel >= ARRAY_SIZE(gChannels))
		return FALSE;
	return gChannels[nChannel].playing;
}

// Push a channel's current volume and pan onto its live voice.
//
// StartChannel used to be the only place this happened, so a fade that
// arrived AFTER a sound started never reached it: opening the pause menu
// sets the effects fade to zero and every already-playing effect kept going
// at full volume behind the menu. Service refreshes live channels now, which
// is what the OAL backend does.
static void
gcApplyChannelVolume(GcChannel *c)
{
	if(c->voice == nil)
		return;
	// The OAL backend is the reference: SetVolume does
	// SetGain(vol / MAX_VOLUME), i.e. a plain 0..1 gain applied equally to
	// both ears, and it ignores pan entirely. Match that at full scale
	// (AESND takes 0..255 a side), and let pan only ATTENUATE the far side.
	// The previous formula multiplied by 4 and clamped, so anything off
	// centre ran up to twice as loud as the game asked for and clipped -
	// the "volumes todos loucos" and the deafening menu.
	uint32 vol = c->volume*gEffectsVolume/127;
	vol = vol*gEffectsFade/127;
	if(vol > 127) vol = 127;
	uint32 base = vol*255/127;
	uint32 pan = c->pan > 127 ? 127 : c->pan;

	// Distance and placement, matching what the PC backend gets from OpenAL:
	// AL_INVERSE_DISTANCE_CLAMPED with a rolloff of 1, reference distance =
	// the min the game passes, clamped at the max. The position is already in
	// camera space, so +X is to the right and the azimuth IS the pan.
	if(c->has3D && c->distMax > 0.0f){
		f32 ref = c->distMin > 0.01f ? c->distMin : 0.01f;
		f32 dist = sqrtf(c->posX*c->posX + c->posY*c->posY + c->posZ*c->posZ);
		f32 clamped = dist < ref ? ref : (dist > c->distMax ? c->distMax : dist);
		f32 gain = ref/(ref + (clamped - ref));
		uint32 g = (uint32)(base*gain + 0.5f);
		base = g > 255 ? 255 : g;
		if(dist > 0.01f){
			f32 s = c->posX/dist;              // -1 hard left .. +1 hard right
			if(s < -1.0f) s = -1.0f;
			else if(s > 1.0f) s = 1.0f;
			int32 p = (int32)(63.5f + s*63.5f);
			pan = (uint32)(p < 0 ? 0 : (p > 127 ? 127 : p));
		}
	}
	uint32 lf = 127 - pan, rf = pan;      // 0..127 each, 63/64 at centre
	uint32 l32 = lf >= 63 ? base : base*lf/63;
	uint32 r32 = rf >= 63 ? base : base*rf/63;
	AESND_SetVoiceVolume(c->voice, (u16)(l32 > 255 ? 255 : l32),
	                               (u16)(r32 > 255 ? 255 : r32));
}

void
cSampleManager::StartChannel(uint32 nChannel)
{
	if(nChannel >= ARRAY_SIZE(gChannels))
		return;
	GcChannel *c = &gChannels[nChannel];
	if(c->voice == nil || !c->used){
		char d[64];
		snprintf(d, sizeof(d), "ch=%u v=%d u=%d", (unsigned)nChannel,
		    c->voice != nil, (int)c->used);
		gcAudioDie("start-unprepared-channel", d);
		return;
	}
	if(c->pcmBytes == 0 && !gcPrepareChannel(c, nChannel))
		return;

	// Volume and pan live in one place now (gcApplyChannelVolume), so a
	// fade that arrives mid-sound reaches the voice too.
	if(c->pcmOwned)
		DCFlushRange(c->pcm, c->pcmBytes);
	gcApplyChannelVolume(c);
	AESND_SetVoiceFormat(c->voice, VOICE_MONO16);
	// gcPrepareChannel baked the initial requested pitch into this buffer, so
	// normal one-shots run at the DSP's exact 1:1 rate.
	f32 voiceFreq = (f32)c->freq;
	if(c->pcm48 && c->pcmFreq)
		voiceFreq = GC_DSP_RATE_F*(f32)c->freq/(f32)c->pcmFreq;
	AESND_SetVoiceFrequency(c->voice, voiceFreq);
	bool8 looping = c->loopCount != 1;
	AESND_SetVoiceLoop(c->voice, looping);

	// AESND loops whole buffers, so a sub-buffer loop is expressed by handing
	// it only that part of the buffer. Without this the bike engine looped its
	// attack along with its sustain and restarted from the top every cycle -
	// the user heard it as the engine never looping at all.
	if(c->streamed && c->vs){
		gcVoiceArm(c, looping);
		return;
	}
	AESND_SetVoiceStream(c->voice, false);
	uint8 *bufStart = (uint8*)c->pcm;
	uint32 bufBytes = c->pcmBytes;
	if(looping && (c->loopStart > 0 || c->loopEnd > 0)){
		uint32 s = c->loopStart;
		uint32 e = c->loopEnd > 0 ? (uint32)c->loopEnd : 0;
		// The buffer may have been converted to the DSP's rate, so the loop
		// points - which the game gives against the sample's own rate - move
		// with it.
		if(c->pcm48 && c->pcmFreq){
			uint32 f = c->pcmFreq;
			s = (uint32)((uint64)s*GC_DSP_RATE/f);
			if(e) e = (uint32)((uint64)e*GC_DSP_RATE/f);
		}
		uint32 total = c->pcmBytes/2;
		if(e == 0 || e > total) e = total;
		if(s < e){
			bufStart = (uint8*)c->pcm + (align32(s*2) & ~31u);
			uint32 span = (e - s)*2;
			uint32 avail = c->pcmBytes - (uint32)(bufStart - (uint8*)c->pcm);
			bufBytes = span > avail ? avail : span;
			bufBytes &= ~31u;          // AESND wants a 32-byte multiple
			if(bufBytes == 0){
				bufStart = (uint8*)c->pcm;
				bufBytes = c->pcmBytes;
			}
		}
	}
	c->playing = TRUE;
	gcPlayVoice(c->voice, VOICE_MONO16, bufStart, bufBytes, voiceFreq, FALSE, looping);
}

void
cSampleManager::StopChannel(uint32 nChannel)
{
	if(nChannel >= ARRAY_SIZE(gChannels))
		return;
	GcChannel *c = &gChannels[nChannel];
	if(c->voice)
		AESND_SetVoiceStop(c->voice, true);
	gcVoiceStop(c->vs);
	c->playing = FALSE;
	c->used = FALSE;
	// Hand a large buffer back to the pool. Small ones stay put: they are the
	// common case and churning them would just fragment the arena.
	if(c->pcmOwned && c->allocBytes > GC_CONV_RECLAIM)
		gcDiscardChannelPcm(c);
}

// ------------------------------------------------------------------ volumes

void
cSampleManager::SetEffectsMasterVolume(uint8 nVolume)
{
	// Mirror into the class member as well as the mixer's static. The member
	// is what the rest of the engine reads back through GetEffectsVolume /
	// GetMusicVolume, and this backend was only ever writing the static - see
	// SetMusicMasterVolume below for what that cost.
	m_nEffectsVolume = nVolume;
	gEffectsVolume = nVolume;
}

void
cSampleManager::SetMusicMasterVolume(uint8 nVolume)
{
	// THIS is why no radio station ever played. MusicManager::ServiceGameMode
	// gates the entire radio branch on SampleManager.GetMusicVolume() != 0,
	// and that getter returns the class member - which this backend never
	// wrote, so it sat at its zero-init value forever and
	// m_bGameplayAllowsRadio was set FALSE on every single frame. The station
	// branch, and with it every call that could ever hand a station to
	// StartStreamedFile, was unreachable. Ambience kept playing because its
	// branch does not consult music volume, which is exactly why the logs
	// showed city/water/int_a and never a station.
	m_nMusicVolume = nVolume;
	gMusicVolume = nVolume;
}

// B115: a fade to zero takes effect NOW. The fades used to be read only when
// the next volume was set — by MusicManager/AudioManager in the game loop,
// which does not run during a blocking load — so the frontend track and any
// live channel kept their old volume through the whole loading bar.
static void gcSilenceNow(void);   // defined with the stream table
void
cSampleManager::SetEffectsFadeVolume(uint8 nVolume)
{
	gEffectsFade = nVolume;
	if(nVolume == 0) gcSilenceNow();
}

void
cSampleManager::SetMusicFadeVolume(uint8 nVolume)
{
	gMusicFade = nVolume;
	if(nVolume == 0) gcSilenceNow();
}

// Translate a track id to its file. Backslash to slash, and whatever
// extension it carries becomes .ogg because convert_audio.py re-encodes both
// .adf and .mp3.
static void
gcTrackPath(uint32 nFile, char *path, size_t cap)
{
	strcpy(path, "dvd:/");
	const char *src = StreamedNameTable[nFile];
	char *d = path + 5;
	for(; *src && d < path + cap - 5; src++)
		*d++ = *src == '\\' ? '/' : (char)tolower((unsigned char)*src);
	*d = '\0';
	// MUSIC and mission audio are Vorbis; VOICE ships native (IMA ADPCM
	// .wav, exactly the game's own file) and keeps its extension.
	char *dot = strrchr(path, '.');
	if(dot)
		strcpy(dot, nFile < 9 ? ".ogg" : ".wav");   // B122 (user): the nine radio stations (table 0-8) are Vorbis 32kHz stereo; every other stream is IMA ADPCM .wav at its native rate and channel count
}

// Per-TRACK lengths in ms. MusicManager reads these through
// GetStreamedFileLength(track) at its own Initialise and mods station
// positions by them — zero meant pos %= 0 and garbage station positions.
// Measured once by opening every stream file, then cached on the card; the
// cache is duration-based so re-encodes at other rates keep it valid.
static uint32 gTrackLengthMs[TOTAL_STREAMED_SOUNDS];


// ------------------------------------------------------------------ streams
//
// Radio, mission dialogue and cutscenes. Three of them (MAX_STREAMS), each an
// AESND voice in streaming mode fed from a pair of MEM1 buffers: one playing
// while the other is refilled from disc on the Service() call the game already
// makes every frame.
//
// The pump is codec-agnostic on purpose. Whatever the disc holds — DSP-ADPCM
// decoded on the CPU, or Vorbis — arrives here as 16-bit stereo PCM at
// DIGITALRATE, which is 32000 and already what the engine's own mixer assumed.
// Only gcStreamDecode changes with the format, so the buffering, the voice
// handling and the position bookkeeping do not have to be written twice.
//
// Double buffering rather than a ring: AESND hands the whole buffer to the DSP
// and calls back when it wants the next one, so two is exactly the number the
// hardware asks for.
enum {
	// A multiple of AESND's 1152-byte staging block, and nothing else. The
	// PPC side refills its DSP staging in 1152-byte bites and ZERO-PADS the
	// last bite of every MRAM buffer; 16384 bytes left a 896-byte pad — 7ms
	// of silence per 128ms chunk, heard as a 7.8Hz flutter and measured as
	// playback 5.6% slow. 16128 = 14 bites exactly.
	STREAM_CHUNK_BYTES   = 1152*14,                 // 126ms at 32kHz stereo16
	STREAM_CHUNK_SAMPLES = STREAM_CHUNK_BYTES/4
};

struct GcStream {
	OggVorbis_File vf;
	bool8    vfOpen;
	AESNDPB *voice;
	uint8    volume, pan;
	bool8    effectVolume;
	uint8   *buf[2];
	int32    fill;          // which buffer the pump decodes into next
	int32    play;          // which buffer the callback hands over next
	volatile bool8 bufReady; // buf[play] holds a full decoded chunk
	volatile bool8 eof;      // decoder is dry; the DSP still has chunks to play
	volatile uint32 starved; // callback fired with nothing ready
	volatile uint32 cbCount; // stream callbacks seen, cadence diagnostics
	FILE    *file;
	uint32   dataStart;     // byte offset of the first sample in the file
	uint32   posSamples;    // for GetStreamedFilePosition
	uint32   lenSamples;
	uint32   rate;          // the file's own sample rate; the voice follows it
	uint32   channels;      // 1 or 2, from the file as well
	volatile uint8 opening; // B73: 1 = wait for data then open, 2 = wait for data at the seek target then seek+prime+arm
	uint32   openPos;       // ms position requested by StartStreamedFile
	bool8    armed;
	bool8    openHold;      // preload: prime but do not start
	bool8    adpcm;         // native IMA ADPCM .wav (voice) rather than Vorbis
	uint8    adpcmSpill[4224];  // decoded samples that did not fit the last chunk
	uint32   adpcmSpillBytes;
	// Native-rate codec output waiting for the shared FIR resampler. Extra
	// frames hold the history carried across decode-chunk boundaries.
	uint8    srcBuf[STREAM_CHUNK_BYTES + GC_FIR_TAPS*4];
	uint32   srcFrames;
	uint32   srcPos;       // 16.16 cursor inside srcBuf
	bool8    srcEof;
	uint16   blockAlign;    // ADPCM block size in bytes
	uint32   dataBytes;     // ADPCM payload length
	bool8    playing;
	bool8    paused;
	bool8    looping;
	char     path[80];      // for fail-loud reporting
};
static GcStream gStreams[MAX_STREAMS];

struct GcStreamRequest {
	uint32 generation, applied;
	tTrack track;
	uint32 position;
	bool8 wanted, hold, paused;
};
static GcStreamRequest gStreamRequests[MAX_STREAMS];
static void gcStreamApplyRequest(uint8 nStream);
static void gcStreamArm(GcStream *st, uint8 nStream);

static bool
gcStreamDeferControl(void)
{
	return gStreamDecThread != LWP_THREAD_NULL && LWP_GetSelf() != gStreamDecThread;
}

static void
gcStreamRequest(uint8 nStream, tTrack track, uint32 position, bool8 wanted, bool8 hold)
{
	u32 level;
	_CPU_ISR_Disable(level);
	GcStreamRequest *r = &gStreamRequests[nStream];
	r->track = track;
	r->position = position;
	r->wanted = wanted;
	r->hold = hold;
	r->paused = FALSE;
	r->generation++;
	if(gStreams[nStream].voice)
		AESND_SetVoiceStop(gStreams[nStream].voice, true);
	_CPU_ISR_Restore(level);
}

static bool
gcStreamCanPlay(uint8 nStream)
{
	u32 level;
	_CPU_ISR_Disable(level);
	const GcStreamRequest *r = &gStreamRequests[nStream];
	bool canPlay = r->generation == r->applied && r->wanted && !r->hold && !r->paused;
	_CPU_ISR_Restore(level);
	return canPlay;
}
static void gcApplyStreamVolume(GcStream *st, uint8 nStream);
// B115: see SetEffectsFadeVolume. Streams: the next SetStreamedVolumeAndPan
// (every frame in game mode) restores them; channels re-read gEffectsFade
// through gcApplyChannelVolume on the next Service.
static void
gcSilenceNow(void)
{
	// Same rule as every other volume change: mission streams 1/2 skip the
	// fade. Zeroing them here cut intro1 a frame after it started (DO_FADE IN
	// begins at fade 0 on the frame the line starts) and nothing restored it
	// until the next line.
	for(int32 i = 0; i < MAX_STREAMS; i++)
		gcApplyStreamVolume(&gStreams[i], (uint8)i);
	for(uint32 i = 0; i < ARRAY_SIZE(gChannels); i++)
		if(gChannels[i].voice && gChannels[i].playing) AESND_SetVoiceVolume(gChannels[i].voice, 0, 0);
}

static void gcStreamPump(GcStream *st, bool prime = false);
// 09-26 diag: RMS of each decoded chunk of the mission streams (1, 2), to tell
// a line decoded as silence from one decoded fine and silenced later.
static uint16 gMaRms[3][64];
static uint8 gMaRmsN[3];

static void *
gcStreamDecMain(void *)
{
	while(!gStreamDecQuit){
		gDecTick++;
		if(gSringLock != LWP_MUTEX_NULL && LWP_MutexTryLock(gSringLock) == 0){
			gcSringService(FALSE);
			LWP_MutexUnlock(gSringLock);
		}
		// Watchdog (B56): this thread keeps running through the silent freeze
		// (MemoryWatcher, 09-01), so it can say where the main thread stopped.
		if((gDecTick & 63) == 0){
			static uint32 lastMain, same;
			if(gMainTick == lastMain){
				if(++same == 8 || (same > 8 && (same & 31) == 0))
					printf("WATCHDOG main frozen %us at [%s] tick %u draw %x geo %x/%u dma %u cd %u/%u dvd %u\n",
					    (unsigned)(same/4), (const char*)gMainWhere, (unsigned)gMainTick, gxLastDraw, gxLastGeoFlags, gxLastGeoVerts,
					    gxDmaBusy, gCdTick, gCdState, gIsoRdBusy);
			}else{ lastMain = gMainTick; same = 0; }
		}
		for(int32 i = 0; i < MAX_STREAMS; i++){
			gcStreamApplyRequest(i);
			GcStream *st = &gStreams[i];
			if(st->opening){
				GcStreamGuard g(gStreamLock[i]);
				if(st->opening) gcStreamOpenStep(st, i);
				continue;
			}
			if(!st->playing || st->paused || st->bufReady || st->eof)
				continue;
			GcStreamGuard g(gStreamLock[i]);
			if(st->playing && !st->paused && !st->bufReady && !st->eof){
				gcStreamPump(st);
				gStreamDecPumps++;
			}
		}
		usleep(4000);
	}
	return nil;
}

static void *
gcVoiceDecMain(void *)
{
	while(!gStreamDecQuit){
		for(int32 i = 0; i < GC_CHANNEL_VOICES; i++){
			GcVoiceStream *vs = &gVoiceStreams[i];
			if(!vs->active || vs->bufReady || vs->eof)
				continue;
			LWP_MutexLock(gVoiceLock);
			if(vs->active && !vs->bufReady && !vs->eof)
				gcVoicePump(vs);
			LWP_MutexUnlock(gVoiceLock);
		}
		usleep(4000);
	}
	return nil;
}

// B112: one silent chunk for every "nothing to play" case — starved callback,
// a voice stopped between streams, a voice armed before its first chunk.
static uint8 gStreamSilence[STREAM_CHUNK_BYTES] __attribute__((aligned(32)));
static void
gcStreamCallback(AESNDPB *pb, u32 state, void *arg)
{
	// The DSP finished its buffer and wants the next one NOW. Waiting for the
	// next game frame to provide it stretches every chunk by half a frame —
	// measured 13% slow against the source — so the swap happens right here,
	// from a chunk the game thread decoded ahead of time. No file I/O on this
	// thread; if the pump has not caught up, AESND replays the stale chunk
	// and the counter says so.
	GcStream *st = (GcStream*)arg;
	if(state != VOICE_STATE_STREAM)
		return;
	st->cbCount++;
	if(!gcStreamCanPlay((uint8)(st - gStreams))){
		AESND_SetVoiceStop(pb, true);
		return;
	}
	if(st->bufReady){
		AESND_SetVoiceBuffer(pb, st->buf[st->play], STREAM_CHUNK_BYTES);
		st->play ^= 1;
		st->bufReady = FALSE;
	}else if(st->eof){
		// Everything decoded has now been handed over and played. This is the
		// real end of the sound.
		st->playing = FALSE;
		AESND_SetVoiceStop(pb, true);
	}else{
		// Starved. Replaying the stale chunk machine-gunned 84ms of old
		// audio in a loop — the "radio static" heard the first time the
		// menu opens, while its TXD loads monopolise the FS lock and the
		// pump cannot refill. A dropout must SOUND like a dropout.
		AESND_SetVoiceBuffer(pb, gStreamSilence, STREAM_CHUNK_BYTES);
		st->starved++;
		gStreamStarvedTotal++;
	}
}

// Tremor pulls straight from the file. Tremor is the fixed-point Vorbis
// decoder: the Gekko's FPU is fast, but the reference libvorbis leans on
// doubles, and integer decode is what consoles use.
// ponytail: direct reads are free under Dolphin; a real Mini-DVD wants a bulk
// read-ahead ring here to kill the per-decode seeks.
// ---------------------------------------------------------------- audio I/O ring (B68)
//
// The streams used to fread the disc 32K at a time between the world's reads.
// On the real drive that is a 128ms seek per chunk, queued behind 700K TXD
// pulls, against 250ms of PCM in hand: "snd starved 43" a census and the
// volume pumping the user heard. Each stream now owns a 256K compressed
// read-ahead ring in ARAM, filled 32K at a time by the CdStream worker on a
// channel of its own, so one seek buys 8-16 seconds of audio and the decoder
// never touches the disc. Ped comments ride the same channel, asynchronously.
enum { GC_SRING_BYTES = 256*1024, GC_SRING_BLK = 32*1024, GC_AUDIO_CH = MAX_CDCHANNELS };
struct GcSring {
	uint32 aram;                 // ring base in ARAM (allocated once)
	uint32 fileLba, fileSize;    // the file's extent on the disc
	uint32 rd, wr, fetch;        // absolute file offsets: consumed, landed, requested
	uint32 base;                 // first offset filled since the last flush (retained data = [max(base, wr-ring), wr))
	bool8  active;               // a file is open on this ring
	bool8  sync;                 // main thread owns it (open/seek): waits for the disc
	uint8  bounce[8*1024] __attribute__((aligned(32)));
};
static GcSring gSring[MAX_STREAMS];
static uint8   gSringStage[36*1024] __attribute__((aligned(32)));   // 32K ring block, or 17 sectors of a ped line
static int32   gSringIo = -1;        // read in flight: stream index, -2 = ped comment, -1 idle
static volatile bool8 gSringAbort;   // B178, gcSringCancel: stop the burst after the piece in hand
static bool8   gSringStale;          // B178: the read in flight was cancelled; discard it on completion
static uint32  gSringIoOff, gSringIoLen;
static struct { bool8 active, adpcm; uint32 sfx, slot, aram, base, off, remain, skip; } gPedIo;
static uint32  gSfxRawLba, gSfxRawSize;
extern "C" int fsLookupLba(const char *path, u32 *lba, u32 *size);

static inline uint32 gcSringAvail(GcSring *r) { return r->wr > r->rd ? r->wr - r->rd : 0; }

// A finished read lands: ring block to ARAM, or a ped-line piece to its slot.
static void
gcSringComplete(void)
{
	if(gSringStale){   // cancelled while in flight: its ring was reset since
		gSringStale = FALSE;
		gSringAbort = FALSE;
	}else if(gSringIo >= 0){
		GcSring *r = &gSring[gSringIo];
		if(r->active && gSringIoOff == r->wr)
			r->wr += gSringIoLen;   // B177: gcSringSink already put the burst in ARAM
	}else if(gSringIo == -2 && gPedIo.active){
		uint8 *s = gSringStage + gPedIo.skip;
		uint32 use = gSringIoLen;
		if(!gPedIo.adpcm)   // PCM from sfx.raw is little-endian; ADPCM blocks are byte streams
			for(uint32 b = 0; b + 1 < use; b += 2){ uint8 t = s[b]; s[b] = s[b+1]; s[b+1] = t; }
		if(gPedIo.skip) memmove(gSringStage, s, use);
		uint32 w = align32(use);
		if(w > use) memset(gSringStage + use, 0, w - use);
		gcBankWrite(gPedIo.aram + gPedIo.off, gSringStage, w);
		gPedIo.off += use; gPedIo.remain -= use;
		if(gPedIo.remain == 0){ gPedSlotSfx[gPedIo.slot] = (int32)gPedIo.sfx; gPedIo.active = FALSE; }
	}
	gSringIo = -1;
}

// B177: the ring used to top itself up the moment 32K of room opened, so in
// steady state every 32K of audio was its own disc command between two
// streamer reads: two long seeks (~100 ms each on the drive) per 32K. Now a
// ring waits until GC_SRING_REFILL is free (96K left = 2.2 s of 44 kHz ADPCM,
// 7 s of radio) and refills in one chained burst: the worker reads 32K,
// hands it to this sink, reads the next sector with no seek in between.
enum { GC_SRING_REFILL = 160*1024 };
static uint32 gSringSinkOff;   // file offset of the next piece; set before the post, worker-owned after

static int
gcSringSink(const void *data, unsigned int bytes, void *ctx)
{
	if(gSringAbort)
		return 0;
	GcSring *r = (GcSring*)ctx;
	gcBankWrite(r->aram + (gSringSinkOff % GC_SRING_BYTES), data, bytes);
	gSringSinkOff += bytes;
	return 1;
}

// Channel idle: the ped line first (the game is waiting for it), else the
// hungriest ring that still has file left and room for a burst.
static void
gcSringIssue(void)
{
	if(gSringIo != -1) return;
	if(gPedIo.active){
		uint32 fileOff = gPedIo.base + gPedIo.off;
		uint32 skip = fileOff & 2047;
		uint32 use = gPedIo.remain > GC_SRING_BLK ? GC_SRING_BLK : gPedIo.remain;
		uint32 nsec = (skip + use + 2047)/2048;
		gPedIo.skip = skip; gSringIoLen = use;
		if(CdStreamReadAbs(GC_AUDIO_CH, gSringStage, (gPedIo.adpcm ? gSfxAdpLba : gSfxRawLba) + fileOff/2048, nsec) == STREAM_SUCCESS){ gSringIo = -2; return; }
		gPedIo.active = FALSE;   // could not queue: the game asks again
		return;
	}
	int32 best = -1; uint32 bestFill = ~0u;
	for(int32 i = 0; i < MAX_STREAMS; i++){
		GcSring *r = &gSring[i];
		if(!r->active || r->fetch >= r->fileSize || r->fetch - r->rd + GC_SRING_REFILL > GC_SRING_BYTES) continue;
		uint32 fill = gcSringAvail(r);
		if(fill < bestFill){ bestFill = fill; best = i; }
	}
	if(best < 0) return;
	GcSring *r = &gSring[best];
	// Whole 32K slots only: a piece may never overwrite bytes not yet consumed.
	uint32 len = r->fileSize - r->fetch, room = (GC_SRING_BYTES - (r->fetch - r->rd)) & ~(GC_SRING_BLK-1);
	if(len > room) len = room;
	gSringIoOff = r->fetch; gSringIoLen = len; gSringSinkOff = r->fetch;
	if(CdStreamReadAbsChunked(GC_AUDIO_CH, gSringStage, GC_SRING_BLK/2048, r->fileLba + r->fetch/2048,
	                          (len + 2047)/2048, gcSringSink, r) == STREAM_SUCCESS){
		gSringIo = best; r->fetch += len;
	}
}

// Poll (decode thread) or wait (main thread, at open and seek). Caller holds gSringLock.
static void
gcSringService(bool8 wait)
{
	if(gSringIo != -1){
		int32 st = CdStreamGetStatus(GC_AUDIO_CH);
		if(st == STREAM_READING || st == STREAM_WAITING){
			if(!wait) return;
		}
		CdStreamSync(GC_AUDIO_CH);
		gcSringComplete();
	}
	gcSringIssue();
}

static void
gcSringCancel(int32 idx)   // caller holds gSringLock: drop this stream's read in flight
{
	// B178: never wait here. Open/seek/close run on the main loop, which blocked
	// 300-540 ms behind whatever DVD read the worker had in hand (b171 already,
	// b178 worse with bursts: 'SLOW script 363ms where=cd-sync'). The burst stops
	// after its current piece and the next service pass discards the result.
	if(gSringIo == idx){ gSringAbort = TRUE; gSringStale = TRUE; }
}

static uint32
gcSringRead(GcStream *st, void *dst, uint32 n)
{
	int32 idx = (int32)(st - gStreams);
	GcSring *r = &gSring[idx];
	uint8 *d = (uint8*)dst; uint32 got = 0;
	while(n){
		uint32 avail = gcSringAvail(r);
		if(avail == 0){
			if(r->fetch >= r->fileSize && r->rd >= r->wr && r->wr >= r->fileSize) break;   // true end of file
			if(!r->sync) break;                 // decode thread: short read, the pump guard keeps this rare
			// Poll with the lock released between checks. Waiting inside
			// gcSringService(TRUE) held gSringLock through a whole disc read
			// (300-450 ms under a busy drive) and parked the main loop the
			// moment it needed the ring — a ped comment, a stream request.
			LWP_MutexLock(gSringLock); gcSringService(FALSE); LWP_MutexUnlock(gSringLock);
			if(gcSringAvail(r) == 0) usleep(2000);
			continue;
		}
		uint32 idxb = r->rd % GC_SRING_BYTES;
		uint32 a0 = idxb & ~31u;
		uint32 c = n; if(c > avail) c = avail;
		if(c > GC_SRING_BYTES - idxb) c = GC_SRING_BYTES - idxb;
		if(idxb - a0 + c > sizeof(r->bounce)) c = sizeof(r->bounce) - (idxb - a0);
		uint32 a1 = align32(idxb + c); if(a1 > GC_SRING_BYTES) a1 = GC_SRING_BYTES;
		gcBankRead(r->bounce, r->aram + a0, a1 - a0);
		memcpy(d, r->bounce + (idxb - a0), c);
		d += c; r->rd += c; n -= c; got += c;
	}
	return got;
}

static int
gcSringSeek(GcStream *st, ogg_int64_t offset, int whence)
{
	int32 idx = (int32)(st - gStreams);
	GcSring *r = &gSring[idx];
	ogg_int64_t t = whence == SEEK_SET ? offset : whence == SEEK_CUR ? (ogg_int64_t)r->rd + offset : (ogg_int64_t)r->fileSize + offset;
	if(t < 0) t = 0;
	if(t > (ogg_int64_t)r->fileSize) t = r->fileSize;
	{
		uint32 lo = r->fetch > GC_SRING_BYTES ? r->fetch - GC_SRING_BYTES : 0;   // B177: a burst in flight is already overwriting up to fetch
		if(lo < r->base) lo = r->base;
		if((uint32)t >= lo && (uint32)t <= r->wr){ r->rd = (uint32)t; return 0; }   // inside the retained window: free
	}
	LWP_MutexLock(gSringLock);
	gcSringCancel(idx);
	r->fetch = (uint32)t & ~(uint32)(GC_SRING_BLK-1);
	r->wr = r->fetch; r->base = r->fetch; r->rd = (uint32)t;
	LWP_MutexUnlock(gSringLock);
	return 0;
}
static inline long gcSringTell(GcStream *st) { return (long)gSring[st - gStreams].rd; }

static bool8
gcSringOpen(GcStream *st, const char *path)
{
	int32 idx = (int32)(st - gStreams);
	GcSring *r = &gSring[idx];
	u32 lba, size;
	if(!fsLookupLba(path, &lba, &size)) return FALSE;
	if(gSringLock == LWP_MUTEX_NULL) LWP_MutexInit(&gSringLock, false);
	if(r->aram == 0){ r->aram = gcBankAlloc(GC_SRING_BYTES); if(r->aram == 0) return FALSE; }
	LWP_MutexLock(gSringLock);
	gcSringCancel(idx);
	r->fileLba = lba; r->fileSize = size; r->rd = r->wr = r->fetch = r->base = 0;
	r->active = TRUE; r->sync = FALSE;   // B73: nobody waits on the disc; the decode thread opens when data has landed
	LWP_MutexUnlock(gSringLock);
	return TRUE;
}
static void
gcSringClose(GcStream *st)
{
	int32 idx = (int32)(st - gStreams);
	if(gSringLock == LWP_MUTEX_NULL) return;
	LWP_MutexLock(gSringLock);
	gcSringCancel(idx);
	gSring[idx].active = FALSE;
	LWP_MutexUnlock(gSringLock);
}

static size_t
gcVorbisRead(void *ptr, size_t size, size_t nmemb, void *datasource)
{
	GcStream *st = (GcStream*)datasource;
	if(!gSring[st - gStreams].active)
		return 0;
	// B67: Tremor hands a NULL buffer when its own malloc failed (heap
	// exhausted after a scene load or the pause menu). fread(NULL) wrote at
	// address 0 and Tremor then read the whole file forever on the main
	// thread: the "freeze on unpause / at the cutscene". EOF it instead.
	if(ptr == nil){
		printf("AUDIO: vorbis buffer alloc failed on %s, stream aborted\n", st->path);
		return 0;
	}
	return size ? gcSringRead(st, ptr, (uint32)(size*nmemb))/size : 0;
}

static int
gcVorbisSeek(void *datasource, ogg_int64_t offset, int whence)
{
	GcStream *st = (GcStream*)datasource;
	if(!gSring[st - gStreams].active)
		return -1;
	return gcSringSeek(st, offset, whence);
}

static int
gcVorbisClose(void *)
{
	return 0;
}

static long
gcVorbisTell(void *datasource)
{
	GcStream *st = (GcStream*)datasource;
	return gSring[st - gStreams].active ? (long)gSring[st - gStreams].rd : -1;
}

static ov_callbacks gcVorbisCallbacks = {
	gcVorbisRead, gcVorbisSeek, gcVorbisClose, gcVorbisTell
};

// ---------------------------------------------------------------- voice
//
// Mission speech ships EXACTLY as the game shipped it: IMA ADPCM, mono,
// mostly 22050 Hz, 512-byte blocks — 39MB for all 1120 lines. The previous
// pipeline re-encoded it to 48kHz Vorbis, which upsampled the game's own
// data 2.2x and put Tremor's decode state (100-200KB) in a 24MB MEM1 arena
// for every line of dialogue. Decoding ADPCM costs a 89-entry table, two
// ints of state per block, and no allocation whatsoever.
static const int16 gImaStep[89] = {
	7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,
	80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,
	494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
	2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,
	8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
	27086,29794,32767
};
static const int8 gImaIndex[16] = {
	-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8
};

// One 4-bit nibble -> one sample, advancing predictor and step index.
static inline int16
gcImaNibble(uint8 nib, int32 *pred, int32 *idx)
{
	int32 step = gImaStep[*idx];
	int32 diff = step >> 3;
	if(nib & 1) diff += step >> 2;
	if(nib & 2) diff += step >> 1;
	if(nib & 4) diff += step;
	if(nib & 8) diff = -diff;
	int32 p = *pred + diff;
	if(p > 32767) p = 32767;
	else if(p < -32768) p = -32768;
	*pred = p;
	int32 i = *idx + gImaIndex[nib & 15];
	if(i < 0) i = 0;
	else if(i > 88) i = 88;
	*idx = i;
	return (int16)p;
}

// Samples one ADPCM block yields (mono): the header sample plus two per
// payload byte.
static inline uint32
gcAdpcmBlockSamples(uint32 blockAlign)
{
	return blockAlign > 4 ? 1 + (blockAlign - 4)*2 : 0;
}
// B122: frames per IMA block for 1 or 2 channels. A stereo block carries one
// 4-byte header per channel, then 4-byte words alternating L/R (8 nibbles each).
static inline uint32
gcAdpcmBlockFrames(uint32 blockAlign, uint32 channels)
{
	if(channels == 2) return blockAlign > 8 ? 1 + (blockAlign - 8) : 0;
	return gcAdpcmBlockSamples(blockAlign);
}

// Parse the RIFF header: rate, channels, block size and where data starts.
static bool8
gcWavOpen(GcStream *st)
{
	uint8 h[64];
	if(gcSringSeek(st, 0, SEEK_SET) != 0 || gcSringRead(st, h, 12) != 12)
		return FALSE;
	if(memcmp(h, "RIFF", 4) != 0 || memcmp(h+8, "WAVE", 4) != 0)
		return FALSE;
	uint32 fmtTag = 0;
	bool8 haveFmt = FALSE;
	for(;;){
		uint8 ck[8];
		if(gcSringRead(st, ck, 8) != 8)
			return FALSE;
		uint32 sz = (uint32)ck[4] | ((uint32)ck[5]<<8) | ((uint32)ck[6]<<16) | ((uint32)ck[7]<<24);
		if(memcmp(ck, "fmt ", 4) == 0){
			uint32 n = sz > sizeof(h) ? sizeof(h) : sz;
			if(gcSringRead(st, h, n) != n)
				return FALSE;
			fmtTag       = (uint32)h[0] | ((uint32)h[1]<<8);
			st->channels = (uint32)h[2] | ((uint32)h[3]<<8);
			st->rate     = (uint32)h[4] | ((uint32)h[5]<<8) |
			               ((uint32)h[6]<<16) | ((uint32)h[7]<<24);
			st->blockAlign = (uint16)((uint32)h[12] | ((uint32)h[13]<<8));
			haveFmt = TRUE;
			if(sz > n && gcSringSeek(st, (long)(sz - n), SEEK_CUR) != 0)
				return FALSE;
		}else if(memcmp(ck, "data", 4) == 0){
			if(!haveFmt)
				return FALSE;
			st->dataStart = (uint32)gcSringTell(st);
			st->dataBytes = sz;
			break;
		}else if(gcSringSeek(st, (long)((sz + 1) & ~1u), SEEK_CUR) != 0)
			return FALSE;
	}
	if((st->channels != 1 && st->channels != 2) || st->rate == 0)
		return FALSE;           // B122: voice lines are mono, converted music/cutscene streams keep their stereo
	if(fmtTag == 17 && st->blockAlign > 4*st->channels){
		st->adpcm = TRUE;
		uint32 bs = gcAdpcmBlockFrames(st->blockAlign, st->channels);
		st->lenSamples = (st->dataBytes / st->blockAlign) * bs;
	}else if(fmtTag == 1){
		st->adpcm = FALSE;      // plain PCM: 28 of the 1120 files are 16-bit
		st->blockAlign = 0;
		st->lenSamples = st->dataBytes/(2*st->channels);
	}else
		return FALSE;
	return TRUE;
}

// Decode ADPCM (or byteswap PCM) from the file into dst. Mirrors
// gcStreamDecode's contract: returns bytes produced, 0 at end of file.
static uint32
gcWavDecode(GcStream *st, uint8 *dst)
{
	uint32 done = 0;
	if(!st->adpcm){
		// 16-bit PCM straight through, little-endian file to big-endian DSP.
		size_t got = gcSringRead(st, dst, STREAM_CHUNK_BYTES);
		for(size_t b = 0; b + 1 < got; b += 2){
			uint8 t = dst[b]; dst[b] = dst[b+1]; dst[b+1] = t;
		}
		done = (uint32)got;
	}else{
		// A block decodes to 1017 samples (2034 bytes) and the chunk is
		// 16128, so blocks do NOT divide the chunk: stopping at the last
		// whole block and zero-filling the remainder punched ~43ms of
		// silence into every 323ms of speech. Carry the overflow instead.
		uint8 blk[1024];
		int16 tmp[2100];
		uint32 ba = st->blockAlign > sizeof(blk) ? (uint32)sizeof(blk) : st->blockAlign;
		if(st->adpcmSpillBytes){
			uint32 n = st->adpcmSpillBytes > STREAM_CHUNK_BYTES ?
			    STREAM_CHUNK_BYTES : st->adpcmSpillBytes;
			memcpy(dst, st->adpcmSpill, n);
			done += n;
			st->adpcmSpillBytes -= n;
			if(st->adpcmSpillBytes)
				memmove(st->adpcmSpill, st->adpcmSpill + n, st->adpcmSpillBytes);
		}
		while(done < STREAM_CHUNK_BYTES){
			if(gcSringRead(st, blk, ba) != ba)
				break;
			uint32 n = 0;
			if(st->channels == 2){
				// B122: stereo block — two headers, then 4-byte words alternating L/R.
				int32 pl = (int16)((uint16)blk[0] | ((uint16)blk[1] << 8)), il = blk[2] > 88 ? 88 : blk[2];
				int32 pr = (int16)((uint16)blk[4] | ((uint16)blk[5] << 8)), ir = blk[6] > 88 ? 88 : blk[6];
				tmp[n++] = (int16)pl; tmp[n++] = (int16)pr;
				for(uint32 i = 8; i + 8 <= ba && n + 16 <= ARRAY_SIZE(tmp); i += 8){
					int16 l[8], r[8];
					for(uint32 k = 0; k < 4; k++){
						l[2*k] = gcImaNibble(blk[i+k] & 15, &pl, &il);   l[2*k+1] = gcImaNibble(blk[i+k] >> 4, &pl, &il);
						r[2*k] = gcImaNibble(blk[i+4+k] & 15, &pr, &ir); r[2*k+1] = gcImaNibble(blk[i+4+k] >> 4, &pr, &ir);
					}
					for(uint32 k = 0; k < 8; k++){ tmp[n++] = l[k]; tmp[n++] = r[k]; }
				}
			}else{
				int32 pred = (int16)((uint16)blk[0] | ((uint16)blk[1] << 8));
				int32 idx = blk[2];
				if(idx > 88) idx = 88;
				tmp[n++] = (int16)pred;
				for(uint32 i = 4; i < ba && n + 2 <= ARRAY_SIZE(tmp); i++){
					tmp[n++] = gcImaNibble(blk[i] & 15, &pred, &idx);
					tmp[n++] = gcImaNibble(blk[i] >> 4, &pred, &idx);
				}
			}
			uint32 bytes = n*2;
			uint32 fit = STREAM_CHUNK_BYTES - done;
			if(bytes <= fit){
				memcpy(dst + done, tmp, bytes);
				done += bytes;
			}else{
				memcpy(dst + done, tmp, fit);
				st->adpcmSpillBytes = bytes - fit;
				memcpy(st->adpcmSpill, (uint8*)tmp + fit, st->adpcmSpillBytes);
				done = STREAM_CHUNK_BYTES;
			}
		}
	}
	if(done < STREAM_CHUNK_BYTES)
		memset(dst + done, 0, STREAM_CHUNK_BYTES - done);
	st->posSamples += done/(2*st->channels);   // 16-bit frames
	return done;
}

static void
gcLoadTrackLengths(void)
{
	enum { N = TOTAL_STREAMED_SOUNDS };
	DVD_FS_GUARD;
	FILE *cf = fopen("dvd:/audio/lengths.cache", "rb");
	if(cf){
		size_t got = fread(gTrackLengthMs, sizeof(uint32), N, cf);
		fclose(cf);
		if(got == N)
			return;
	}
	for(uint32 i = 0; i < N && i < ARRAY_SIZE(StreamedNameTable); i++){
		char path[80];
		gcTrackPath(i, path, sizeof(path));
		FILE *f = fopen(path, "rb");
		if(f == nil)
			continue;
		const char *lext = strrchr(path, '.');
		if(lext && (lext[1] == 'w' || lext[1] == 'W')){
			// Native voice: length comes from the RIFF header, no decoder.
			GcStream *probe = &gStreams[0];   // idle at Initialise; was a 21K static
			memset(probe, 0, sizeof(*probe));
			probe->file = f;
			if(gcWavOpen(probe) && probe->rate)
				gTrackLengthMs[i] = (uint32)((uint64)probe->lenSamples*1000/probe->rate);
			memset(probe, 0, sizeof(*probe));
			fclose(f);
			continue;
		}
		OggVorbis_File vf;
		if(ov_open(f, &vf, nil, 0) == 0){
			ogg_int64_t ms = ov_time_total(&vf, -1);  // Tremor returns ms
			if(ms > 0)
				gTrackLengthMs[i] = (uint32)ms;
			ov_clear(&vf);   // closes f
		}else
			fclose(f);
	}
	cf = fopen("dvd:/audio/lengths.cache", "wb");
	if(cf){
		fwrite(gTrackLengthMs, sizeof(uint32), N, cf);
		fclose(cf);
	}
}

// Decode one native-rate codec chunk. Returns actual bytes before zero padding;
// 0 means end of track.
static uint32
gcStreamDecodeNative(GcStream *st, uint8 *dst)
{
	if(gSring[st - gStreams].active && !st->vfOpen)
		return gcWavDecode(st, dst);
	if(!st->vfOpen)
		return 0;
	// ov_read hands back 16-bit stereo, which is what the voice wants, but it
	// returns one packet at a time — loop until the buffer is full or the
	// track ends.
	uint32 done = 0;
	while(done < STREAM_CHUNK_BYTES){
		int bitstream = 0;
		long n = ov_read(&st->vf, (char*)dst + done,
		                 (int)(STREAM_CHUNK_BYTES - done), &bitstream);
		if(n <= 0)
			break;
		done += (uint32)n;
	}
	if(done < STREAM_CHUNK_BYTES)
		memset(dst + done, 0, STREAM_CHUNK_BYTES - done);
	st->posSamples += done/(2*st->channels);
	return done;
}

// Every stream — 22.05kHz speech/ambience as well as 44.1kHz radio — reaches
// AESND at the DSP's exact rate. Otherwise the ucode repeats samples, the same
// metallic mechanism already measured on effects. Keep nine source frames at
// decode boundaries so the existing polyphase FIR stays continuous.
static uint32
gcStreamDecode(GcStream *st, uint8 *dst)
{
	if(st->rate == GC_DSP_RATE)
		return gcStreamDecodeNative(st, dst);
	uint32 channels = st->channels == 1 ? 1 : 2;
	uint32 frameBytes = channels*2;
	uint32 outFrames = STREAM_CHUNK_BYTES/frameBytes;
	uint32 step = (st->rate << 16)/GC_DSP_RATE;
	uint32 made = 0;
	int16 *out = (int16*)dst;
	gcBuildFir();

	while(made < outFrames){
		uint32 i0;
		for(;;){
			i0 = st->srcPos >> 16;
			uint32 right = GC_FIR_TAPS - (GC_FIR_TAPS/2 - 1) - 1;
			if(st->srcFrames && (st->srcEof || i0 + right < st->srcFrames))
				break;
			if(st->srcEof)
				break;

			uint32 keep = st->srcFrames < GC_FIR_TAPS ?
			    st->srcFrames : GC_FIR_TAPS;
			uint32 first = st->srcFrames - keep;
			if(keep)
				memmove(st->srcBuf,
				    st->srcBuf + first*frameBytes, keep*frameBytes);
			uint32 shift = first << 16;
			st->srcPos = st->srcPos >= shift ? st->srcPos - shift : 0;
			uint32 got = gcStreamDecodeNative(st,
			    st->srcBuf + keep*frameBytes);
			st->srcFrames = keep + got/frameBytes;
			if(got == 0)
				st->srcEof = TRUE;
		}

		if(st->srcFrames == 0 ||
		   (st->srcEof && st->srcPos >= (st->srcFrames << 16)))
			break;
		i0 = st->srcPos >> 16;
		uint32 ph = (st->srcPos >> 10) & (GC_FIR_PHASES-1);
		const f32 *tap = gFirTable[ph];
		for(uint32 ch = 0; ch < channels; ch++){
			f32 acc = 0.0f;
			for(int32 t = 0; t < GC_FIR_TAPS; t++){
				int32 si = (int32)i0 + t - (GC_FIR_TAPS/2 - 1);
				if(si < 0) si = 0;
				else if(si >= (int32)st->srcFrames)
					si = (int32)st->srcFrames - 1;
				acc += tap[t]*(f32)((int16*)st->srcBuf)[si*channels + ch];
			}
			int32 v = (int32)(acc + (acc >= 0.0f ? 0.5f : -0.5f));
			if(v > 32767) v = 32767;
			else if(v < -32768) v = -32768;
			out[made*channels + ch] = (int16)v;
		}
		made++;
		st->srcPos += step;
	}
	uint32 bytes = made*frameBytes;
	if(bytes < STREAM_CHUNK_BYTES)
		memset(dst + bytes, 0, STREAM_CHUNK_BYTES - bytes);
	return bytes;
}


// Keep one decoded chunk ahead of the DSP. The callback consumes it with a
// pointer swap; this refills on the decode thread (gcStreamDecMain).
static void
gcStreamPump(GcStream *st, bool prime)
{
	if(st->opening || !st->playing || (st->paused && !prime) || st->bufReady || st->eof || st->voice == nil)
		return;
	uint8 *dst = st->buf[st->fill];
	if(dst == nil)
		return;
	// Taken after the early-outs, so an idle stream does not contend with the
	// streaming worker sixty times a second for nothing.
	{
		GcSring *r = &gSring[st - gStreams];
		if(r->active && !r->sync && r->fetch < r->fileSize && gcSringAvail(r) < 16*1024)
			return;   // let the ring fill; the DSP still holds two chunks
	}
	uint32 got = gcStreamDecode(st, dst);
	{
		uint32 n = (uint32)(st - gStreams);
		if(got && (n == 1 || n == 2) && gMaRmsN[n] < 64){
			const int16 *pcm = (const int16*)dst;
			uint32 k = got/2;
			double acc = 0;
			for(uint32 i = 0; i < k; i++) acc += (double)pcm[i]*pcm[i];
			gMaRms[n][gMaRmsN[n]++] = (uint16)sqrt(acc/(k ? k : 1));
		}
	}
	if(got == 0){
		// The DECODER is dry, which is not the same as the SOUND being over:
		// priming decodes two chunks before a line starts, and a short line of
		// speech can be shorter than that, so stopping the voice here cut off
		// audio that had been decoded but not yet played - heard as a click
		// where the line should have been. Mark it and let the callback finish
		// what it already has; it stops the voice when it runs out.
		st->eof = TRUE;
		// EOF at 90%+ of the samples is a track ending; EOF before that is a
		// truncated or unreadable file — the "cutscene speech died mid-scene"
		// class. Loud, with position and length on record.
		if(!st->looping && st->lenSamples &&
		   st->posSamples < st->lenSamples - st->lenSamples/10){
			char d[120];
			snprintf(d, sizeof(d), "%s at %u/%u samples", st->path,
			    (unsigned)st->posSamples, (unsigned)st->lenSamples);
			gcAudioDie("stream-early-end", d);
		}
		return;
	}
	DCFlushRange(dst, STREAM_CHUNK_BYTES);
	st->fill ^= 1;
	st->bufReady = TRUE;
}

// Hand the voice its stream and first chunk, and let it run.
//
// This has to be repeatable. A preloaded line is opened and primed and then
// held; resuming it used to be AESND_SetVoiceStop(voice, false) alone, and
// that does not re-arm a stream voice - traced on the intro, the slot read
// as playing while its position sat frozen at the primed 16128 samples and
// never advanced, so no callback ever came, no audio came out, and the
// mission-audio state machine wrote the line off and moved to the next one.
// TRUE while PreloadStreamedFile is opening a line: it must prime the
// buffers but not hand one to the voice. Arming twice - once at preload and
// again at start - advanced the play pointer past the first chunk, so every
// preloaded line began 8064 samples in. The user heard it exactly: all the
// intro audio playing from the middle onwards, never whole.
static bool8 gStreamPreloading;

static void
gcStreamArm(GcStream *st, uint8 nStream)
{
	if(st->voice == nil || st->armed || !gcStreamCanPlay(nStream))
		return;
	const void *first = gStreamSilence;
	if(!st->bufReady)
		gcStreamPump(st);
	if(st->bufReady){
		first = st->buf[st->play];
		st->play ^= 1;
		st->bufReady = FALSE;
		gcStreamPump(st);        // the next chunk waits for the first callback
	}else{
		// B112: no chunk yet — never unstop the voice on whatever buffer it
		// held last. That buffer was the PREVIOUS stream's tail (the office
		// scene's last chunk played at the start of the next scene: the "door
		// slam for no reason"). Silence until the first callback asks.
		first = gStreamSilence;
	}
	gcApplyStreamVolume(st, nStream);
	u32 level;
	_CPU_ISR_Disable(level);
	if(gcStreamCanPlay(nStream)){
		st->armed = TRUE;
		gcPlayVoice(st->voice, st->channels == 1 ? VOICE_MONO16 : VOICE_STEREO16,
		    first, STREAM_CHUNK_BYTES, st->rate == GC_DSP_RATE ? (f32)st->rate : GC_DSP_RATE_F,
		    TRUE, FALSE);
	}
	_CPU_ISR_Restore(level);
}

// B73: the deferred half of StartStreamedFile, on the decode thread. Waits for
// the ring, opens, positions with one raw seek, primes and arms. Never blocks.
static void
gcStreamOpenStepSync(GcStream *st, int32 idx)
{
	GcSring *r = &gSring[idx];
	if(st->opening == 1){
		bool8 ok;
		if(st->adpcm || (strrchr(st->path, '.') && (strrchr(st->path, '.')[1] == 'w' || strrchr(st->path, '.')[1] == 'W'))){
			ok = gcWavOpen(st);
			if(ok && st->openPos && st->rate){
				uint32 want = (uint32)((uint64)st->openPos*st->rate/1000);
				if(st->lenSamples) want %= st->lenSamples;
				uint32 off = st->adpcm ? (want/gcAdpcmBlockFrames(st->blockAlign, st->channels))*st->blockAlign : want*2*st->channels;
				if(gcSringSeek(st, (ogg_int64_t)(st->dataStart + off), SEEK_SET) == 0) st->posSamples = want;
			}
			if(!ok){ printf("AUDIO: wav open failed %s\n", st->path); st->opening = 0; st->playing = FALSE; return; }
			st->opening = 3;
		}else{
			if(mallinfo().fordblks < 192*1024){ printf("AUDIO: no heap for stream %d, %s skipped\n", (int)idx, st->path); st->opening = 0; st->playing = FALSE; return; }
			int ovrc = ov_open_callbacks(st, &st->vf, nil, 0, gcVorbisCallbacks);
			if(ovrc < 0){ printf("AUDIO: vorbis open failed %s rc=%d\n", st->path, ovrc); st->opening = 0; st->playing = FALSE; return; }
			st->vfOpen = TRUE;
			vorbis_info *vi = ov_info(&st->vf, -1);
			st->rate = vi ? (uint32)vi->rate : DIGITALRATE;
			st->channels = vi && vi->channels == 1 ? 1 : 2;
			st->lenSamples = (uint32)ov_pcm_total(&st->vf, -1);
			if(st->openPos && st->lenSamples){
				ogg_int64_t want = (ogg_int64_t)st->openPos*(st->rate/1000);
				want %= (ogg_int64_t)st->lenSamples;
				ogg_int64_t rawOff = (ogg_int64_t)r->fileSize * want / (ogg_int64_t)st->lenSamples;
				gcSringSeek(st, rawOff, SEEK_SET);   // flush to the target; step 2 waits for it to land
				st->opening = 2;
				return;
			}
			st->opening = 3;
		}
	}
	if(st->opening == 2){
		uint32 need2 = 64*1024; if(gcSringAvail(r) < need2 && !(r->fetch >= r->fileSize)) return;
		if(ov_raw_seek(&st->vf, (ogg_int64_t)r->rd) == 0){
			st->posSamples = (uint32)ov_pcm_tell(&st->vf);
			gcStreamDecode(st, st->buf[0]);   // discard: the first block after a raw seek is noise
		}
		st->opening = 3;
	}
	if(st->opening == 3){
		if(gcSringAvail(r) < 32*1024 && !(r->fetch >= r->fileSize)) return;   // room for two chunks of decode
		AESND_SetVoiceFormat(st->voice, st->channels == 1 ? VOICE_MONO16 : VOICE_STEREO16);
		AESND_SetVoiceFrequency(st->voice, st->rate == GC_DSP_RATE ? (f32)st->rate : GC_DSP_RATE_F);
		gcApplyStreamVolume(st, (uint8)idx);
		st->opening = 0;
		gcStreamPump(st, true);
		if(!st->openHold && !st->paused) gcStreamArm(st, (uint8)idx);
	}
}

// B83: the cutscene manager holds the picture until the dialogue stream is primed;
// with the async open the animation used to start seconds before the audio.
extern "C" int gcStreamPrimed(int n)
{
	if(n < 0 || n >= MAX_STREAMS) return TRUE;
	u32 level;
	_CPU_ISR_Disable(level);
	bool primed = gStreamRequests[n].generation == gStreamRequests[n].applied &&
	    !gStreams[n].opening && (!gStreams[n].playing || gStreams[n].armed || gStreams[n].bufReady);
	_CPU_ISR_Restore(level);
	return primed;
}

// B75: Tremor's ov_open scans BACKWARDS from the end of the file for the last
// page. On a ring that answers "no data yet" with a 0-byte read it takes that
// as EOF, steps back, reads 0 again, and spins forever at offset 0 -- and at
// priority 72 this thread starved the main loop: B73 froze on the first
// cutscene frame. While a stream is opening its ring reads therefore BLOCK
// (sync), which only ever parks this thread, never the game.
static void
gcStreamOpenStep(GcStream *st, int32 idx)
{
	GcSring *r = &gSring[idx];
	if(!r->active) { st->opening = 0; return; }
	uint32 need = r->fileSize < 96*1024 ? r->fileSize : 96*1024;
	if(gcSringAvail(r) < need && !(r->fetch >= r->fileSize && r->wr >= r->fileSize)) return;
	r->sync = TRUE;
	gcStreamOpenStepSync(st, idx);
	r->sync = FALSE;
}

void
cSampleManager::Service(void)
{
	gMainWhere = "audio-service";
	// AESND mixes on the DSP; what the CPU owes it each frame is the next
	// block of stream data. Reading from disc here rather than in the voice
	// callback keeps file I/O off the audio path.
	u64 t0 = gettime();
	// Volume and pan can change while a sound is already playing - the pause
	// menu drops the effects fade to zero, and without this refresh every
	// effect that was already running kept blaring behind the menu.
	for(uint32 i = 0; i < ARRAY_SIZE(gChannels); i++){
		GcChannel *c = &gChannels[i];
		if(c->playing){
			gcApplyChannelVolume(c);
			continue;
		}
		// Finished, and nobody asked for it to stop: the voice callback
		// cleared 'playing'. Give its buffer back so the next sound can be
		// converted instead of falling through to the DSP's resampler.
		if(c->pcmOwned && c->allocBytes > GC_CONV_RECLAIM)
			gcDiscardChannelPcm(c);
	}
	gcReleaseIdleFrontendPcm();
	// Streams are pumped by the decode thread now (gcStreamDecMain) — a
	// Vorbis chunk on this thread was a 10-16ms bite out of every eighth
	// frame, the metronome behind "constant stutters".

}

bool8
cSampleManager::IsMP3RadioChannelAvailable(void)
{
	// No user-track feature on the console: TRUE here made the game offer —
	// and sometimes tune — an "MP3 player" station with garbage behind it.
	return FALSE;
}

void
cSampleManager::UpdateEffectsVolume(void)
{
	;
}

void
cSampleManager::SetMP3BoostVolume(uint8 nVolume)
{
	;
}

void
cSampleManager::SetMonoMode(bool8 nMode)
{
	;
}

uint8
cSampleManager::IsMissionAudioLoaded(uint8 nSlot, uint32 nSample)
{
	return nSample == gPlayerTalkSfx ? LOADING_STATUS_LOADED
	                                 : LOADING_STATUS_NOT_LOADED;
}

bool8
cSampleManager::LoadMissionAudio(uint8 nSlot, uint32 nSample)
{
	if(gPlayerTalkData == nil){
		// MEM1 on both targets: this buffer is memcpy'd by the CPU, and on a
		// GameCube audio memory is ARAM, which the CPU cannot address.
		gPlayerTalkData = (uint8*)memalign(32, PED_BLOCKSIZE);
		if(gPlayerTalkData == nil)
			return FALSE;
	}
	if(!gcReadSample(nSample, gPlayerTalkData))
		return FALSE;
	gPlayerTalkSfx = nSample;
	return TRUE;
}

uint8
cSampleManager::IsPedCommentLoaded(uint32 nComment)
{
	return _GetPedCommentSlot(nComment) >= 0 ? LOADING_STATUS_LOADED
	                                         : LOADING_STATUS_NOT_LOADED;
}

int32
cSampleManager::_GetPedCommentSlot(uint32 nComment)
{
	// Only the three most recent slots count, like the OAL backend: older
	// slots are already being overwritten by the rotation.
	for(int32 i = 0; i < 3; i++){
		int32 slot = (int32)gCurrentPedSlot - i - 1;
		if(slot < 0)
			slot += MAX_PEDSFX;
		if(gPedSlotSfx[slot] == (int32)nComment)
			return slot;
	}
	return -1;
}

bool8
cSampleManager::LoadPedComment(uint32 nComment)
{
	if(CTimer::GetIsCodePaused())
		return FALSE;
	// no talking peds during cutscenes
	if(MusicManager.IsInitialised() &&
	   MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE)
		return FALSE;
	if(!gPackedSfx){
		if(gPedAram == 0){
			// Slots sized to the largest comment, as dca3 does: one of at most
			// PED_BLOCKSIZE PCM bytes is 39 IMA blocks of 512 — 137K of ARAM
			// for the seven, not 553K. The rest goes to the texel store.
			uint32 stride = gAdpAll ? ((PED_BLOCKSIZE/2 + 1016)/1017)*512 : align32(PED_BLOCKSIZE);
			gPedAram = gcBankAlloc(stride*MAX_PEDSFX);
			if(gPedAram == 0)
				return FALSE;
			gPedSlotStride = stride;
			for(int32 i = 0; i < MAX_PEDSFX; i++)
				gPedSlotSfx[i] = -1;
		}
		// B68: asynchronous. The line lands on the audio channel a few frames
		// later and IsPedCommentLoaded reports it; the caller asks every tick.
		if(gPedIo.active)
			return FALSE;
		if(gSampleIndex == nil || nComment >= gNumSamples || gSampleIndex[nComment].nSize > PED_BLOCKSIZE)
			return FALSE;
		bool8 adp = gAdpAll;
		if(adp && gSfxAdpLba == 0 && !fsLookupLba("dvd:/audio/sfx.adp", &gSfxAdpLba, &gSfxAdpSize))
			adp = FALSE;
		if(!adp && gPedSlotStride < gSampleIndex[nComment].nSize)
			return FALSE;   // raw PCM does not fit an ADPCM-sized slot
		if(!adp && gSfxRawLba == 0 && !fsLookupLba("dvd:/audio/sfx.raw", &gSfxRawLba, &gSfxRawSize))
			return FALSE;
		if(gSringLock == LWP_MUTEX_NULL) LWP_MutexInit(&gSringLock, false);
		LWP_MutexLock(gSringLock);
		gPedIo.active = TRUE; gPedIo.adpcm = adp; gPedIo.sfx = nComment; gPedIo.slot = gCurrentPedSlot;
		gPedIo.aram = gPedAram + gPedSlotStride*gCurrentPedSlot;
		if(adp){ gPedIo.base = gcAdpOffset(nComment); gPedIo.remain = gcAdpBlocks(nComment)*512; }
		else   { gPedIo.base = gSampleIndex[nComment].nOffset; gPedIo.remain = gSampleIndex[nComment].nSize; }
		gPedIo.off = 0; gPedIo.skip = 0;
		gPedSlotAdpcm[gCurrentPedSlot] = adp; gPedSlotBytes[gCurrentPedSlot] = gPedIo.remain;
		gPedSlotSfx[gCurrentPedSlot] = -1;
		if(++gCurrentPedSlot >= MAX_PEDSFX)
			gCurrentPedSlot = 0;
		LWP_MutexUnlock(gSringLock);
		return FALSE;
	}
	if(gPedBuf == nil){
		gPedBuf = (uint8*)memalign(32, PED_BLOCKSIZE*MAX_PEDSFX);
		if(gPedBuf == nil)
			return FALSE;
		for(int32 i = 0; i < MAX_PEDSFX; i++)
			gPedSlotSfx[i] = -1;
	}
	if(!gcReadSample(nComment, gPedBuf + PED_BLOCKSIZE*gCurrentPedSlot))
		return FALSE;
	gPedSlotSfx[gCurrentPedSlot] = (int32)nComment;
	if(++gCurrentPedSlot >= MAX_PEDSFX)
		gCurrentPedSlot = 0;
	return TRUE;
}

void
cSampleManager::SetChannelReverbFlag(uint32 nChannel, bool8 nReverbFlag)
{
	;
}

// The game hands every sound a CAMERA-SPACE position and a rolloff window,
// and this backend used to throw both away - the two functions below were
// empty. That is why nothing had distance attenuation or stereo placement:
// AudioManager deliberately does not call SetChannelPan when
// EXTERNAL_3D_SOUND is defined (which it is here), because positioning is
// the backend's job. Reflections showed it worst - AUDIO_REFLECTIONS spawns
// a copy at 0.5625x volume a few frames late, and with the position dropped
// it landed dead centre on top of the original at -5dB instead of the PC's
// -17dB out to one side, which is a slapback echo rather than a room.
void
cSampleManager::SetChannel3DPosition(uint32 nChannel, float fX, float fY, float fZ)
{
	if(nChannel >= ARRAY_SIZE(gChannels))
		return;
	GcChannel *c = &gChannels[nChannel];
	c->posX = fX;
	c->posY = fY;
	c->posZ = fZ;
	c->has3D = TRUE;
}

void
cSampleManager::SetChannel3DDistances(uint32 nChannel, float fMax, float fMin)
{
	if(nChannel >= ARRAY_SIZE(gChannels))
		return;
	gChannels[nChannel].distMax = fMax;
	gChannels[nChannel].distMin = fMin;
}

void
cSampleManager::PreloadStreamedFile(tTrack nFile, uint8 nStream)
{
	if(!_bSampmanInitialised || nStream >= MAX_STREAMS || (uint32)nFile >= ARRAY_SIZE(StreamedNameTable))
		return;
	gcStreamRequest(nStream, nFile, 0, TRUE, TRUE);
	if(!gcStreamDeferControl()) gcStreamApplyRequest(nStream);
}

void
cSampleManager::PauseStream(bool8 nPauseFlag, uint8 nStream)
{
	if(nStream >= MAX_STREAMS) return;
	u32 level;
	_CPU_ISR_Disable(level);
	gStreamRequests[nStream].paused = nPauseFlag;
	if(nPauseFlag && gStreams[nStream].voice)
		AESND_SetVoiceStop(gStreams[nStream].voice, true);
	_CPU_ISR_Restore(level);
}

void
cSampleManager::StartPreloadedStreamedFile(uint8 nStream)
{
	if(nStream >= MAX_STREAMS) return;
	u32 level;
	_CPU_ISR_Disable(level);
	gStreamRequests[nStream].hold = FALSE;
	_CPU_ISR_Restore(level);
}

bool8
cSampleManager::StartStreamedFile(tTrack nFile, uint32 nPos, uint8 nStream)
{
	if(!_bSampmanInitialised)
		return FALSE;
	if(nStream >= MAX_STREAMS)
		return FALSE;
	if((uint32)nFile >= ARRAY_SIZE(StreamedNameTable))
		return FALSE;
	if(gcStreamDeferControl()){
		gcStreamRequest(nStream, nFile, nPos, TRUE, FALSE);
		return TRUE;
	}
	GcStreamGuard sg(gStreamLock[nStream]);
	GcStream *st = &gStreams[nStream];
	StopStreamedFile(nStream);
	if(nStream == 1 || nStream == 2) gMaRmsN[nStream] = 0;   // diag: a fresh line
	st->srcFrames = 0;
	st->srcPos = 0;
	st->srcEof = FALSE;
	// B71: no FS guard here. The ring's first fill is done by the CdStream
	// worker, which takes the same lock; holding it across ov_open deadlocked
	// the worker against our own wait (read timeout, park).

	// StreamedNameTable in sampman.h already maps every track to its file —
	// "AUDIO\\WILD.ADF" and so on — so translate that rather than inventing a
	// second numbering that would silently drift from the game's own enum.
	if((uint32)nFile >= ARRAY_SIZE(StreamedNameTable))
		return FALSE;
	char path[80];
	gcTrackPath(nFile, path, sizeof(path));
	{
		char gl[96];
		snprintf(gl, sizeof(gl), "STRM start s%d %s pos=%u", (int)nStream, path, (unsigned)nPos);
		printf("%s\n", gl);   // B120: was built and never printed — 20 minutes of log with no stream line
	}
	strncpy(st->path, path, sizeof(st->path)-1);
	st->path[sizeof(st->path)-1] = '\0';
	if(!gcSringOpen(st, path)){
		// B119: a missing stream is silence, not a park (user: fail-loud off;
		// the radio stations are deliberately absent). One line per path.
		static uint32 said;
		if(said++ < 12) printf("STRM missing %s\n", path);
		return FALSE;
	}

	if(st->voice == nil){
		st->voice = AESND_AllocateVoiceWithArg(gcStreamCallback, st);
		if(st->voice == nil){ gcSringClose(st); return FALSE; }
	}
	for(int32 i = 0; i < 2; i++)
		if(st->buf[i] == nil){
			st->buf[i] = (uint8*)memalign(32, STREAM_CHUNK_BYTES);
			// Silence, not whatever MEM1 happened to hold. The DSP can read
			// this the instant the voice is armed, and uninitialised heap
			// played back as full-scale white noise.
			if(st->buf[i])
				memset(st->buf[i], 0, STREAM_CHUNK_BYTES);
		}
	if(st->buf[0] == nil || st->buf[1] == nil){
		gcSringClose(st); return FALSE;
	}

	// Voice is native: no Vorbis, no decode state, no allocation.
	const char *ext = strrchr(path, '.');
	st->adpcm = ext && (ext[1] == 'w' || ext[1] == 'W');   // native voice line; the header decides the real format
	// B73: nothing below touches the disc. The decode thread opens the file
	// once the ring holds its first blocks (gcStreamOpenStep), seeks, primes
	// and arms. StartStreamedFile used to stall the frame for every station
	// change and every cutscene line (headers + position seek at drive speed).
	st->armed = FALSE;
	st->fill = 0;
	st->play = 0;
	st->adpcmSpillBytes = 0;
	st->bufReady = FALSE;
	st->eof = FALSE;
	st->starved = 0;
	st->posSamples = 0;
	st->openPos = nPos;
	st->openHold = gStreamPreloading;
	st->paused = gStreamPreloading;
	st->playing = TRUE;
	st->opening = 1;
	return TRUE;
}

void
cSampleManager::StopStreamedFile(uint8 nStream)
{
	if(nStream >= MAX_STREAMS)
		return;
	if(gcStreamDeferControl()){
		gcStreamRequest(nStream, (tTrack)0, 0, FALSE, FALSE);
		return;
	}
	GcStreamGuard sg(gStreamLock[nStream]);
	GcStream *st = &gStreams[nStream];
	if(st->vfOpen){
		char gl[32];
		snprintf(gl, sizeof(gl), "STRM stop s%d", (int)nStream);
	}
	if(st->voice){
		AESND_SetVoiceStop(st->voice, true);
		AESND_SetVoiceBuffer(st->voice, gStreamSilence, STREAM_CHUNK_BYTES);   // B112: nothing stale left to resume
	}
	st->armed = FALSE;
	st->opening = 0;
	if(st->vfOpen){ ov_clear(&st->vf); st->vfOpen = FALSE; }
	gcSringClose(st);   // waits for the worker if a fill is in flight: no FS guard above it
	st->playing = FALSE;
	st->posSamples = 0;
}

// 09-26: a mission line that starts and is cut. One line per state change of
// a mission audio slot (AudioLogic.cpp), with what its stream was doing.
extern "C" void
gcMissionAudioTrace(const char *what, int slot, int sample)
{
	uint8 n = (uint8)(slot + 1);
	if(n >= MAX_STREAMS) return;
	GcStream *st = &gStreams[n];
	const GcStreamRequest *r = &gStreamRequests[n];
	printf("MAUDIO %s slot %d sample %d | s%d %s pos %u/%u eof %d playing %d armed %d hold %d paused %d starved %u cb %u vol %u fx %u f=%u\n",
	    what, slot, sample, (int)n, st->path, (unsigned)st->posSamples, (unsigned)st->lenSamples,
	    (int)st->eof, (int)st->playing, (int)st->armed, (int)r->hold, (int)r->paused,
	    (unsigned)st->starved, (unsigned)st->cbCount, (unsigned)st->volume, (unsigned)gEffectsVolume,
	    (unsigned)CTimer::GetFrameCounter());
	if(strncmp(what, "finished", 8) == 0 && n <= 2){
		char line[400];
		int k = snprintf(line, sizeof(line), "MAUDIO rms s%d:", (int)n);
		for(int i = 0; i < gMaRmsN[n] && k < (int)sizeof(line) - 8; i++)
			k += snprintf(line + k, sizeof(line) - k, " %u", (unsigned)gMaRms[n][i]);
		printf("%s\n", line);
		gMaRmsN[n] = 0;
	}
}

int32
cSampleManager::GetStreamedFilePosition(uint8 nStream)
{
	// In milliseconds, which is what the music manager expects.
	if(nStream >= MAX_STREAMS)
		return 0;
	u32 level;
	_CPU_ISR_Disable(level);
	GcStreamRequest request = gStreamRequests[nStream];
	_CPU_ISR_Restore(level);
	if(request.generation != request.applied)
		return request.wanted ? request.position : 0;
	GcStream *st = &gStreams[nStream];
	return (int32)((uint64)st->posSamples*1000/(st->rate ? st->rate : DIGITALRATE));
}

int32
cSampleManager::GetStreamedFileLength(uint8 nStream)
{
	// The parameter is a TRACK id, not a stream slot: MusicManager fills its
	// per-track table with this at init, and AudioLogic passes mission sfx
	// ids. The OAL backend's nStreamLength array has the same shape.
	return nStream < TOTAL_STREAMED_SOUNDS ? (int32)gTrackLengthMs[nStream] : 0;
}

bool8
cSampleManager::IsStreamPlaying(uint8 nStream)
{
	if(nStream >= MAX_STREAMS) return FALSE;
	u32 level;
	_CPU_ISR_Disable(level);
	const GcStreamRequest *r = &gStreamRequests[nStream];
	bool playing = r->wanted && !r->hold && !r->paused &&
	    (r->generation != r->applied || gStreams[nStream].playing);
	_CPU_ISR_Restore(level);
	return playing;
}

static void
gcStreamApplyRequest(uint8 nStream)
{
	u32 level;
	_CPU_ISR_Disable(level);
	GcStreamRequest request = gStreamRequests[nStream];
	_CPU_ISR_Restore(level);
	GcStream *st = &gStreams[nStream];
	if(request.generation != request.applied){
		if(request.wanted){
			gStreamPreloading = request.hold;
			SampleManager.StartStreamedFile(request.track, request.position, nStream);
			gStreamPreloading = FALSE;
		}else
			SampleManager.StopStreamedFile(nStream);
		_CPU_ISR_Disable(level);
		gStreamRequests[nStream].applied = request.generation;
		_CPU_ISR_Restore(level);
	}
	_CPU_ISR_Disable(level);
	request = gStreamRequests[nStream];
	bool pause = request.hold || request.paused;
	bool changed = st->paused != pause;
	st->openHold = request.hold;
	st->paused = pause;
	if(st->voice && (request.generation != request.applied || !request.wanted || pause))
		AESND_SetVoiceStop(st->voice, true);
	else if(st->voice && changed && st->armed && st->playing)
		AESND_SetVoiceStop(st->voice, false);
	_CPU_ISR_Restore(level);
	if(!st->opening && !st->armed && st->playing && gcStreamCanPlay(nStream))
		gcStreamArm(st, nStream);
}

static void
gcStreamsShutdown(void)
{
	for(int32 i = 0; i < MAX_STREAMS; i++){
		SampleManager.StopStreamedFile(i);
		memset(&gStreamRequests[i], 0, sizeof(gStreamRequests[i]));
		if(gStreams[i].voice){
			AESND_FreeVoice(gStreams[i].voice);
			gStreams[i].voice = nil;
		}
	}
}

// B155: what the mixer is playing right now — channel:sample/volume for the
// live voices and the stream paths — for the "city noise under the office
// cutscene" hunt (gamecube.cpp prints it every 5 s while a cutscene runs).
extern "C" int gcVoiceCensusLine(char *out, int cap)
{
	int n = 0, live = 0;
	for(uint32 i = 0; i < ARRAY_SIZE(gChannels); i++){
		GcChannel *c = &gChannels[i];
		if(c->voice == nil || !c->playing) continue;
		live++;
		if(live <= 16 && n < cap - 24)
			n += snprintf(out + n, cap - n, " %u:s%u/v%u", (unsigned)i, (unsigned)c->sample, (unsigned)c->volume);
	}
	for(int32 s = 0; s < MAX_STREAMS && n < cap - 40; s++)
		if(gStreams[s].playing)
			n += snprintf(out + n, cap - n, " strm%d:%s%s", (int)s, gStreams[s].path, gStreams[s].paused ? "(paused)" : "");
	if(n < cap - 12) snprintf(out + n, cap - n, " | %d live", live);
	return live;
}

// Diagnostics for the autoradio health line.
uint32
gGcStreamStarved(uint8 nStream)
{
	return nStream < MAX_STREAMS ? gStreams[nStream].starved : 0;
}

uint32
gGcStreamCallbacks(uint8 nStream)
{
	return nStream < MAX_STREAMS ? gStreams[nStream].cbCount : 0;
}

void
cSampleManager::SetStreamedFileLoopFlag(bool8 nLoopFlag, uint8 nChannel)
{
	if(nChannel < MAX_STREAMS)
		gStreams[nChannel].looping = nLoopFlag;
}

void
cSampleManager::SetSpeakerConfig(int32 nConfig)
{
	;
}

uint32
cSampleManager::GetMaximumSupportedChannels(void)
{
	// Generics only: the police radio's reserved voice must not be part of
	// what the engine's volume cull is allowed to spend (GC_GENERIC_VOICES).
	return GC_GENERIC_VOICES;
}

uint32
cSampleManager::GetNum3DProvidersAvailable()
{
	// Zero reads as "No audio hardware" in the frontend and greys the whole
	// audio page out. There is exactly one device and it is always present.
	return 1;
}

void
cSampleManager::SetNum3DProvidersAvailable(uint32 num)
{
	;
}

char *
cSampleManager::Get3DProviderName(uint8 id)
{
	static char name[] = "GAMECUBE DSP";
	return id == 0 ? name : nil;
}

void
cSampleManager::Set3DProviderName(uint8 id, char *name)
{
	;
}

int8
cSampleManager::GetCurrent3DProviderIndex(void)
{
	return 0;
}

int8
cSampleManager::SetCurrent3DProvider(uint8 nProvider)
{
	return 0;   // the DSP is provider 0, and it is not going anywhere
}

void
cSampleManager::ReleaseDigitalHandle(void)
{
	;
}

void
cSampleManager::ReacquireDigitalHandle(void)
{
	;
}

bool8
cSampleManager::CheckForAnAudioFileOnCD(void)
{
	return FALSE;
}

char
cSampleManager::GetCDAudioDriveLetter(void)
{
	return 0;
}

bool8
cSampleManager::UpdateReverb(void)
{
	return FALSE;
}

int8
cSampleManager::AutoDetect3DProviders()
{
	return 0;
}

cSampleManager::cSampleManager(void)
{
	;
}

cSampleManager::~cSampleManager(void)
{
	;
}

// The five-argument form is PS2-only; sampman.h picks one by GTA_PS2.
// MusicManager calls this every frame — it is how the radio fades, ducks for
// dialogue, and follows the music volume preference. Same 0..127 → 0..255
// linear split as StartChannel.
static void
gcApplyStreamVolume(GcStream *st, uint8 nStream)
{
	if(st->voice == nil)
		return;
	u32 level;
	_CPU_ISR_Disable(level);
	bool8 nEffectFlag = st->effectVolume;
	uint32 vol = st->volume*(nEffectFlag ? gEffectsVolume : gMusicVolume)/127;
	// Reference OAL behavior: mission streams 1/2 follow the effects slider
	// but deliberately bypass the effects fade. During scene transitions that
	// fade reaches zero; applying it here muted lines such as intro1 even while
	// the stream state and decoder advanced normally.
	if(!(nEffectFlag && (nStream == 1 || nStream == 2)))
		vol = vol*(nEffectFlag ? gEffectsFade : gMusicFade)/127;
	if(vol > 127) vol = 127;
	uint32 base = vol*255/127;
	uint32 pan = st->pan > 127 ? 127 : st->pan;
	// Same model as the channels: full scale at centre, pan attenuates only.
	uint32 lf = 127 - pan, rf = pan;
	uint32 l32 = lf >= 63 ? base : base*lf/63;
	uint32 r32 = rf >= 63 ? base : base*rf/63;
	AESND_SetVoiceVolume(st->voice,
	    (u16)(l32 > 255 ? 255 : l32), (u16)(r32 > 255 ? 255 : r32));
	_CPU_ISR_Restore(level);
}

void
cSampleManager::SetStreamedVolumeAndPan(uint8 nVolume, uint8 nPan, bool8 nEffectFlag, uint8 nStream)
{
	if(nStream >= MAX_STREAMS)
		return;
	GcStream *st = &gStreams[nStream];
	u32 level;
	_CPU_ISR_Disable(level);
	st->volume = nVolume;
	st->pan = nPan;
	st->effectVolume = nEffectFlag;
	gcApplyStreamVolume(st, nStream);
	_CPU_ISR_Restore(level);
}

#endif // AUDIO_GAMECUBE

// ---------------------------------------------------------- conformance test
//
// Armed by dvd:/audiotest.txt. Sweeps EVERY category of audio the game has -
// bank effects across the whole rate range, all nine radio stations, mission
// voice, ambience - measures what each one actually produces, and writes the
// numbers to mc:/audiotest.log before the game ever boots.
//
// RMS is the point. A stream that opens successfully and decodes silence
// looks identical to a working one in every other log; here it reads 0. A
// stream decoding garbage reads far above the source. tools/gamecube/
// audio_census.py computes the same figure on the host from the same files,
// so the two columns can be put side by side.
static const uint32 gAudioTestSfx[] = {
	0, 1, 11, 19, 33, 37, 43, 154, 291, 320, 321, 322, 323
};

static uint32
gcRms(const int16 *s, uint32 count)
{
	if(count == 0)
		return 0;
	uint64 acc = 0;
	for(uint32 i = 0; i < count; i++){
		int32 v = s[i];
		acc += (uint64)(v*v);
	}
	return (uint32)sqrt((double)(acc/count));
}

static char gTestBuf[8192];
static uint32 gTestLen;

static void
gcTestLog(const char *fmt, ...)
{
	char line[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	// Buffered: one file write at the end. Opening dvd:/ per line put libfat
	// in the middle of the very thing being measured.
	uint32 n = (uint32)strlen(line);
	if(gTestLen + n + 2 < sizeof(gTestBuf)){
		memcpy(gTestBuf + gTestLen, line, n);
		gTestLen += n;
		gTestBuf[gTestLen++] = '\n';
	}
}

static void
gcTestFlush(void)
{
	FILE *f = fopen("mc:/audiotest.log", "w");
	if(f){ fwrite(gTestBuf, 1, gTestLen, f); fclose(f); }
}
