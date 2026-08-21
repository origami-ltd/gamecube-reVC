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
#include "cdstream.h"
#include "CdStream.h"

#include <gccore.h>
#include <aesndlib.h>
#include <ogc/aram.h>
#include <ogc/cache.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <tremor/ivorbisfile.h>
#include <ctype.h>
#include <unistd.h>

void GeckoLog(const char *msg);

// Fail-loud audio (user directive): any sound that cannot be served crashes
// the game on the spot, with the reason on the card first. A silent miss
// hides in a play session; a red screen with "FATAL-AUDIO" does not.
// Single switch — comment out to ship.
#define AUDIO_FAIL_LOUD

#ifdef AUDIO_FAIL_LOUD
static void
gcAudioDie(const char *what, const char *detail)
{
	char line[200];
	struct mallinfo mi = mallinfo();
	snprintf(line, sizeof(line), "FATAL-AUDIO %s %s [libc-free=%uK]",
	    what, detail ? detail : "", (unsigned)mi.fordblks/1024);
	GeckoLog(line);
	// Silence the DSP first: it keeps looping its last buffer through the
	// crash otherwise (the user's "horrible beep").
	AESND_Pause(true);
	// gcFatalPark runs in thread context: writes crash.log, paints the park
	// screen WITH this message (the register-dump red screen of a raw null
	// store carries no text), and stops the world.
	extern void gcFatalPark(const char *tag, const char *msg);
	gcFatalPark("FATAL-AUDIO", line);
}
#else
#define gcAudioDie(what, detail) ((void)0)
#endif

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
};
static GcChannel gChannels[MAXCHANNELS + MAX2DCHANNELS];

// AESND has 32 voices, hard cap. Three are the streams (radio, mission
// dialogue, cutscene); the rest are game channels. GetMaximumSupportedChannels
// reports this, so the game indexes channels 0..28 and never touches a
// voiceless slot — returning 0 there is what turned audio off entirely
// (AudioManager reads <=1 as "no hardware" and terminates itself).
enum { GC_CHANNEL_VOICES = MAX_VOICES - MAX_STREAMS };

// AESND has no "is this voice still going" query, so the voice tells us. The
// callback runs on the audio thread and only ever clears the flag, which is
// why a plain volatile bool is enough — there is no read-modify-write to race.
static void
gcVoiceCallback(AESNDPB *pb, u32 state, void *arg)
{
	(void)pb;
	if(state == VOICE_STATE_STOPPED)
		((GcChannel*)arg)->playing = FALSE;
}

// The sample index, read once from sfx.sdt. tSample is what the game already
// uses: offset, size, frequency, loop start and loop end.
static tSample *gSampleIndex;
static uint32   gNumSamples;

// Bank residency in ARAM. A bank is a contiguous run of sfx.raw, so one ARAM
// allocation and one DMA per bank.
struct GcBank {
	uint32 aramAddr;
	uint32 bytes;
	bool8  loaded;
};

// Where each bank sample sits in audio memory (ARAM on GameCube, the
// 16MB-capped MEM2 shim on the Wii dev target). Native rate, host-endian,
// byte-for-byte the size the game's own sfx.raw carries — no inflation.
static uint32 gBankSampleAddr[SAMPLEBANK_PED_START];
static GcBank gBanks[MAX_SFX_BANKS];

// The DSP's own output rate, taken from libogc rather than assumed: the
// GameCube clocks it at 54MHz/1124 = 48042.7Hz and the Wii at a flat 48000.
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
enum { GC_CH_RESAMPLE_CAP = 512*1024 };
// ...but 29 channels must not each hold one, so conversions also draw on a
// shared MEM1 budget. Past it a sound plays native rather than failing: the
// DSP's stair-step is the graceful degradation, an allocation failure is not.
enum { GC_CONV_BUDGET = 2048*1024 };
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

static uint8 gEffectsVolume = 127, gMusicVolume = 127;
static uint8 gEffectsFade = 127, gMusicFade = 127;

// Ped comments: seven rotating PED_BLOCKSIZE slots filled straight from
// sfx.raw on demand, plus one dedicated player-talk buffer — the OAL layout.
// ponytail: plain MEM1 malloc (~630KB); move to MEM2/ARAM staging if the
// arena ever needs it back.
static uint8 *gPedBuf;
static int32  gPedSlotSfx[MAX_PEDSFX];
static uint8  gCurrentPedSlot;
static uint8 *gPlayerTalkData;
static uint32 gPlayerTalkSfx = 0xFFFFFFFF;

// One read shared by ped comments and player talk: sample bytes from
// sfx.raw into dst, byteswapped to the DSP's big-endian.
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
	DVD_FS_GUARD;
	FILE *f = fopen("dvd:/audio/sfx.raw", "rb");
	if(f == nil){
		gcAudioDie("sfx.raw-open", nil);
		return FALSE;
	}
	uint32 size = gSampleIndex[nSfx].nSize;
	bool8 ok = fseek(f, (long)gSampleIndex[nSfx].nOffset, SEEK_SET) == 0 &&
	    fread(dst, 1, size, f) == size;
	fclose(f);
	if(!ok){
		snprintf(d, sizeof(d), "sfx=%u", (unsigned)nSfx);
		gcAudioDie("sample-read", d);
	}
	if(ok)
		for(uint32 b = 0; b + 1 < size; b += 2){
			uint8 t = dst[b]; dst[b] = dst[b+1]; dst[b+1] = t;
		}
	return ok;
}

static inline uint32
align32(uint32 v)
{
	return (v + 31) & ~31u;
}

// The bank lives in audio memory: ARAM on the GameCube, MEM2 standing in on
// the Wii dev target (the Wii removed ARAM; Dolphin-Wii ignores AR DMA, which
// is why the bank "loaded" into nothing). One-way stack lifetime either way.
#if defined(HW_RVL)
static uint32
gcBankAlloc(uint32 bytes)
{
	// From Arena2 HI, downward: malloc's sbrk fallback grows Arena2Lo upward
	// (see _sbrk_r in gamecube.cpp), so the two stay disjoint by
	// construction — sharing Arena2Lo with sbrk was the heap smash.
	// HARD CAP at 16MB: this shim stands in for the GameCube's ARAM and
	// nothing else. Without the cap the dev build quietly spends the Wii's
	// 64MB and stops representing the ship target — which is exactly how a
	// 46MB sample bank got built for a machine with 16MB of audio memory.
	enum { GC_ARAM_SIZE = 16*1024*1024 };
	static uint32 used;
	if(used + align32(bytes) > GC_ARAM_SIZE)
		return 0;
	uint8 *lo = (uint8*)SYS_GetArena2Lo();
	uint8 *hi = (uint8*)SYS_GetArena2Hi();
	uint8 *nhi = (uint8*)((uint32)(hi - align32(bytes)) & ~31u);
	if(nhi < lo)
		return 0;
	SYS_SetArena2Hi(nhi);
	used += align32(bytes);
	return (uint32)nhi;
}
static void gcBankWrite(uint32 dst, const void *src, uint32 n){ memcpy((void*)dst, src, n); }
static void gcBankRead(void *dst, uint32 src, uint32 n){ memcpy(dst, (const void*)src, n); }
#else
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
	AR_StartDMA(AR_MRAMTOARAM, (u32)src, dst, n);
	while(AR_GetDMAStatus())
		;
}
static void
gcBankRead(void *dst, uint32 src, uint32 n)
{
	DCInvalidateRange(dst, n);
	AR_StartDMA(AR_ARAMTOMRAM, (u32)dst, src, n);
	while(AR_GetDMAStatus())
		;
}
#endif

// ---------------------------------------------------------------- lifecycle

static void gcStreamsShutdown(void);   // defined with the stream machinery
static void gcLoadTrackLengths(void);  // same
static void gcAudioSelfTest(void);     // defined after the stream machinery


bool8
cSampleManager::Initialise(void)
{
	if(_bSampmanInitialised)
		return TRUE;

	AESND_Init();
	AESND_Pause(false);

#if !defined(HW_RVL)
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
#endif

	// Only as many as the budget allows: allocating all 44 slots would eat
	// every voice and leave the streams none.
	for(int32 i = 0; i < GC_CHANNEL_VOICES; i++){
		gChannels[i].voice = AESND_AllocateVoiceWithArg(gcVoiceCallback, &gChannels[i]);
		if(gChannels[i].voice)
			AESND_SetVoiceStop(gChannels[i].voice, true);
	}

	// Not fatal when the bank is absent. The card does not carry audio yet,
	// and the null backend this replaces always reported success — failing
	// init here would turn "no sound" into "no boot", which is a strictly
	// worse way to be missing audio.
	if(!InitialiseSampleBanks()){
		GeckoLog("audio: no sfx bank, sound disabled");
		gcAudioDie("sfx.sdt-open-or-read", "dvd:/audio/sfx.sdt");
	}else if(!LoadSampleBank(SFX_BANK_0)){
		// The OAL and Miles backends load the main bank inside their own
		// Initialise; nothing game-side does it on the PC path. Without this
		// no channel ever passes the loaded check and every effect is silent.
		GeckoLog("audio: bank0 load failed");
		gcAudioDie("bank0-load", "see BANK line above");
	}

	gcLoadTrackLengths();

	_bSampmanInitialised = TRUE;
	gcAudioSelfTest();          // no-op unless dvd:/audiotest.txt is present
	return TRUE;
}

void
cSampleManager::Terminate(void)
{
	if(!_bSampmanInitialised)
		return;
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
	gSampleIndex = (tSample*)malloc(gNumSamples*sizeof(tSample));
	if(gSampleIndex == nil){ fclose(f); return FALSE; }
	if(fread(gSampleIndex, sizeof(tSample), gNumSamples, f) != gNumSamples){
		free(gSampleIndex); gSampleIndex = nil; fclose(f); return FALSE;
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

	BankStartOffset[SFX_BANK_0] = 0;
	return TRUE;
}

bool8
cSampleManager::LoadSampleBank(uint8 nBank)
{
	if(nBank >= MAX_SFX_BANKS || gSampleIndex == nil)
		return FALSE;
	if(gBanks[nBank].loaded)
		return TRUE;

	// A bank is the run of samples from its start offset to the next bank's.
	// The resident bank ends where the ped-comment region begins: everything
	// from SAMPLEBANK_PED_START up is streamed per-sample into the rotating
	// ped slots (see InitialiseChannel's routing), never served from here.
	// Falling back to gNumSamples made "bank 0" span all of sfx.raw — 340MB,
	// which no ARAM or MEM2 pool holds — instead of its real 14MB.
	uint32 first = BankStartOffset[nBank];
	uint32 last = nBank+1 < MAX_SFX_BANKS && BankStartOffset[nBank+1] ?
	    BankStartOffset[nBank+1] : SAMPLEBANK_PED_START;
	if(first >= gNumSamples)
		return FALSE;
	if(last > gNumSamples)
		last = gNumSamples;

	uint32 byteStart = gSampleIndex[first].nOffset;
	uint32 byteEnd = gSampleIndex[last-1].nOffset + gSampleIndex[last-1].nSize;
#ifdef HW_RVL
	// Native-rate bank: sized by the file bytes, per-sample aligned. The
	// 48k conversion happens per channel at play time now.
	uint32 bytes = align32(byteEnd - byteStart);
#else
	uint32 bytes = align32(byteEnd - byteStart);
#endif

	// gcBankAlloc is a stack on both targets (ARAM stack on GameCube, MEM2
	// arena on the Wii dev build): UnloadSampleBank cannot return memory, so
	// an unload/reload cycle (audio Terminate/Initialise around a cutscene
	// skip) must reuse the old allocation or the second alloc of a 14MB bank
	// exhausts the pool and sound never comes back. This was the whole-game
	// SFX mute on the Wii build: raw AR_Alloc here bypassed the HW_RVL MEM2
	// shim, and the Wii has no ARAM to allocate.
	uint32 addr;
	if(gBanks[nBank].aramAddr && gBanks[nBank].bytes >= bytes)
		addr = gBanks[nBank].aramAddr;
	else{
		addr = gcBankAlloc(bytes);
		{
			// To the card, not gecko: the gecko capture truncates lines.
			DVD_FS_GUARD;
			char bl[96];
#ifdef HW_RVL
			snprintf(bl, sizeof(bl), "BANK %s %uK arena2=%uK addr=%08x\n",
			    addr ? "ok" : "FAIL", (unsigned)(bytes/1024),
			    (unsigned)(SYS_GetArena2Size()/1024), (unsigned)addr);
#else
			snprintf(bl, sizeof(bl), "BANK %s %uK aram addr=%08x\n",
			    addr ? "ok" : "FAIL", (unsigned)(bytes/1024), (unsigned)addr);
#endif
			FILE *al = fopen("dvd:/audio.log", "a");
			if(al){ fputs(bl, al); fclose(al); }
		}
		if(addr == 0){
			GeckoLog("audio: bank alloc failed");
			return FALSE;
		}
	}

	DVD_FS_GUARD;

	// ONE PATH, BOTH TARGETS: stream the bank's contiguous run of sfx.raw
	// through a small staging buffer into audio memory, byteswapping as it
	// goes. The staging buffer is one transfer, not the whole bank — the
	// entire point of ARAM is that 14.3MB never has to sit in MEM1.
	enum { STAGE = 64*1024 };
	uint8 *stage = (uint8*)memalign(32, STAGE);
	if(stage == nil){ return FALSE; }
	FILE *raw = fopen("dvd:/audio/sfx.raw", "rb");
	if(raw == nil){ free(stage); return FALSE; }
	fseek(raw, (long)byteStart, SEEK_SET);
	for(uint32 done = 0; done < bytes; ){
		uint32 chunk = bytes - done > STAGE ? STAGE : align32(bytes - done);
		size_t got = fread(stage, 1, chunk, raw);
		if(got == 0)
			break;
		// sfx.raw is the PC file: 16-bit little-endian PCM. The DSP reads
		// big-endian; unswapped it plays as metallic noise (user-confirmed).
		// One swap here at load covers every later per-sample DMA.
		for(uint32 b = 0; b + 1 < (uint32)got; b += 2){
			uint8 t = stage[b]; stage[b] = stage[b+1]; stage[b+1] = t;
		}
		gcBankWrite(addr + done, stage, align32((uint32)got));
		done += align32((uint32)got);
	}
	fclose(raw);
	free(stage);

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
	}
	gFirReady = TRUE;
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
	uint32 bytes = align32(gSampleIndex[nSfx].nSize);
	if(bytes == 0){
		snprintf(d, sizeof(d), "sfx=%u", (unsigned)nSfx);
		gcAudioDie("sample-zero-bytes", d);
		return FALSE;
	}

	// Route by sample index, like the OAL backend: bank samples from the
	// resident bank, player talk and ped comments from their MEM staging
	// buffers (already byteswapped at load).
	if(nSfx < SAMPLEBANK_PED_START){
		nBank = SFX_BANK_0;
		if(!gBanks[nBank].loaded){
			snprintf(d, sizeof(d), "sfx=%u", (unsigned)nSfx);
			gcAudioDie("bank0-not-loaded", d);
			return FALSE;
		}
		// The bank holds the game's own PCM at its own rate (ARAM on the
		// GameCube, the 16MB-capped MEM2 shim on the Wii dev target) - DMA
		// only, never CPU-addressable, and never inflated. What DOES get
		// converted is the copy this channel is about to play.
		//
		// Why convert at all: AESND's ucode resamples by repeating samples,
		// with no interpolation, so an 11kHz effect played at the DSP's
		// 48kHz output gets aliasing images as loud as the real top octave.
		// That is the "metallic" the user keeps hearing, and the GameCube is
		// better than this. Interpolating once, here, into the channel's own
		// MEM1 buffer costs a few dozen KB for the length of one sound and
		// hands the DSP a 1:1 buffer with nothing left to alias. Storing the
		// whole bank at 48kHz instead would be 46MB against 14.3MB of source
		// - it never fit ARAM, which is the mistake this replaces.
		uint32 rawBytes = gSampleIndex[nSfx].nSize;
		uint32 baseFreq = gSampleIndex[nSfx].nFrequency ?
		    gSampleIndex[nSfx].nFrequency : 22050;
		uint32 inS = rawBytes/2;
		uint32 outS = baseFreq < GC_DSP_RATE ?
		    (uint32)((uint64)inS*GC_DSP_RATE/baseFreq) : inS;
		uint32 outBytes = align32(outS*2);

		// ARAM DMA needs a 32-byte aligned source, and sample offsets are
		// arbitrary (the table is packed byte-for-byte: 0, 1400, 3918...).
		// Read from the aligned address below the sample and skip the
		// remainder. On the Wii shim this is a memcpy and the alignment is
		// free, which is exactly why it went unnoticed there.
		uint32 srcAddr = gBankSampleAddr[nSfx];
		uint32 srcSkew = srcAddr & 31;
		uint32 readBytes = align32(srcSkew + rawBytes);

		// One buffer. The native data lands at the TAIL and the conversion
		// runs forward into the front: for every output k the source index
		// i0 satisfies (k - i0) <= (outS - inS), so the read head always
		// stays ahead of the write head and no scratch is needed. 64 bytes
		// of slack covers the alignment skew.
		bool8 resample = baseFreq < GC_DSP_RATE && outBytes <= GC_CH_RESAMPLE_CAP;
		if(resample){
			uint32 need = outBytes + 64;
			if(need > c->allocBytes &&
			   gConvBytes - c->allocBytes + need > GC_CONV_BUDGET)
				resample = FALSE;      // pool is full; take the DSP's version
		}
		uint32 want = resample ? outBytes + 64 : readBytes;
		if(want < readBytes)
			want = readBytes;

		if(!c->pcmOwned) { c->pcm = nil; c->pcmBytes = 0; c->allocBytes = 0; }
		if(c->allocBytes < want){
			gConvBytes -= c->allocBytes;
			free(c->pcm);
			c->pcm = memalign(32, want);
			c->allocBytes = c->pcm ? want : 0;
			gConvBytes += c->allocBytes;
			c->pcmBytes = c->allocBytes;
			c->pcmOwned = c->pcm != nil;
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
		gcBankRead(base + tail, srcAddr - srcSkew, readBytes);
		const int16 *sp = (const int16*)(base + tail + srcSkew);
		if(resample && inS >= 2){
			int16 *dst = (int16*)base;
			uint32 step = (baseFreq << 16)/GC_DSP_RATE;   // 16.16 source cursor
			uint32 pos = 0;
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
			gConvOk++;
			if(gConvBytes > gConvPeak) gConvPeak = gConvBytes;
		}else{
			if(tail || srcSkew)
				memmove(base, sp, rawBytes);
			c->pcmBytes = align32(rawBytes);
			c->pcm48 = FALSE;
			if(baseFreq < GC_DSP_RATE)
				gConvFallback++;   // this one will alias, and we know it
		}
	}else{
		// Ped comments and player talk are copied, not pointed to: their
		// staging slots rotate and a pointed-at slot could be overwritten
		// mid-play. Bounded by PED_BLOCKSIZE, so the cost is fixed.
		//
		// Size check FIRST: the observed channel-pcm-alloc park was a 205088B
		// request — 2.6x the ped cap — failing its alloc before the slot
		// check could name the real problem. An oversize sample can never
		// have a loaded slot, so refuse it by name.
		if(gSampleIndex[nSfx].nSize > PED_BLOCKSIZE){
			snprintf(d, sizeof(d), "sfx=%u %uB", (unsigned)nSfx,
			    (unsigned)gSampleIndex[nSfx].nSize);
			gcAudioDie("ped-sample-oversize", d);
			return FALSE;
		}
		if(!c->pcmOwned) { c->pcm = nil; c->pcmBytes = 0; c->allocBytes = 0; }
		if(c->allocBytes < bytes){
			gConvBytes -= c->allocBytes;
			free(c->pcm);
			c->pcm = memalign(32, bytes);
			c->allocBytes = c->pcm ? bytes : 0;
			gConvBytes += c->allocBytes;
			c->pcmBytes = c->allocBytes;
			c->pcmOwned = c->pcm != nil;
		}
		if(c->pcm == nil){
			snprintf(d, sizeof(d), "ch=%u %uB", (unsigned)nChannel, (unsigned)bytes);
			gcAudioDie("channel-pcm-alloc", d);
			return FALSE;
		}
		if(nSfx == gPlayerTalkSfx && gPlayerTalkData){
			memcpy(c->pcm, gPlayerTalkData, gSampleIndex[nSfx].nSize);
		}else{
			int32 slot = _GetPedCommentSlot(nSfx);
			if(slot < 0 || gPedBuf == nil){
				snprintf(d, sizeof(d), "sfx=%u slot=%d", (unsigned)nSfx, (int)slot);
				gcAudioDie("ped-comment-not-loaded", d);
				return FALSE;
			}
			memcpy(c->pcm, gPedBuf + PED_BLOCKSIZE*slot, gSampleIndex[nSfx].nSize);
		}
		// Copied verbatim, never converted - and the flag has to be cleared
		// for BOTH the ped and the player-talk path, or a channel that last
		// played a converted bank sample keeps scaling this one's pitch.
		c->pcm48 = FALSE;
	}

	c->sample = nSfx;
	c->freq = gSampleIndex[nSfx].nFrequency;
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
	if(nChannel < ARRAY_SIZE(gChannels))
		gChannels[nChannel].freq = nFreq;
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
	if(c->voice == nil || c->pcm == nil || !c->used){
		char d[64];
		snprintf(d, sizeof(d), "ch=%u v=%d p=%d u=%d", (unsigned)nChannel,
		    c->voice != nil, c->pcm != nil, (int)c->used);
		gcAudioDie("start-unprepared-channel", d);
		return;
	}

	// Volume and pan live in one place now (gcApplyChannelVolume), so a
	// fade that arrives mid-sound reaches the voice too.
	if(c->pcmOwned)
		DCFlushRange(c->pcm, c->pcmBytes);
	gcApplyChannelVolume(c);
	AESND_SetVoiceFormat(c->voice, VOICE_MONO16);
	// A converted buffer is 48kHz, so the game's pitch request (engine revs
	// and friends, expressed against the sample's own rate) scales onto it.
	f32 voiceFreq = (f32)c->freq;
	if(c->pcm48 && c->sample < SAMPLEBANK_PED_START &&
	   gSampleIndex && gSampleIndex[c->sample].nFrequency)
		voiceFreq = GC_DSP_RATE_F*(f32)c->freq /
		            (f32)gSampleIndex[c->sample].nFrequency;
	AESND_SetVoiceFrequency(c->voice, voiceFreq);
	bool8 looping = c->loopCount != 1;
	AESND_SetVoiceLoop(c->voice, looping);

	// AESND loops whole buffers, so a sub-buffer loop is expressed by handing
	// it only that part of the buffer. Without this the bike engine looped its
	// attack along with its sustain and restarted from the top every cycle -
	// the user heard it as the engine never looping at all.
	uint8 *bufStart = (uint8*)c->pcm;
	uint32 bufBytes = c->pcmBytes;
	if(looping && (c->loopStart > 0 || c->loopEnd > 0)){
		uint32 s = c->loopStart;
		uint32 e = c->loopEnd > 0 ? (uint32)c->loopEnd : 0;
		// The buffer may have been converted to the DSP's rate, so the loop
		// points - which the game gives against the sample's own rate - move
		// with it.
		if(c->pcm48 && c->sample < SAMPLEBANK_PED_START &&
		   gSampleIndex && gSampleIndex[c->sample].nFrequency){
			uint32 f = gSampleIndex[c->sample].nFrequency;
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
	AESND_SetVoiceBuffer(c->voice, bufStart, bufBytes);
	// TEMP diagnostic (remove at bring-up close): what actually starts, so a
	// doubled sound shows up as the same sfx twice instead of being argued
	// about. Bounded, card only.
	{
		static int32 left = 300;
		if(left > 0){
			left--;
			DVD_FS_GUARD;
			FILE *sl = fopen("dvd:/chan.log", "a");
			if(sl){
				fprintf(sl, "CH %u sfx=%u b=%u f=%u v%u p%u l%u c48=%d t=%u\n",
				    (unsigned)nChannel, (unsigned)c->sample,
				    (unsigned)c->pcmBytes, (unsigned)voiceFreq,
				    (unsigned)c->volume, (unsigned)c->pan,
				    (unsigned)c->loopCount, (int)c->pcm48,
				    (unsigned)ticks_to_millisecs(gettime()));
				fclose(sl);
			}
		}
	}
	c->playing = TRUE;
	AESND_SetVoiceStop(c->voice, false);
}

void
cSampleManager::StopChannel(uint32 nChannel)
{
	if(nChannel >= ARRAY_SIZE(gChannels))
		return;
	GcChannel *c = &gChannels[nChannel];
	if(c->voice)
		AESND_SetVoiceStop(c->voice, true);
	c->playing = FALSE;
	c->used = FALSE;
	// Hand a large buffer back to the pool. Small ones stay put: they are the
	// common case and churning them would just fragment the arena.
	if(c->pcmOwned && c->allocBytes > GC_CONV_RECLAIM){
		gConvBytes -= c->allocBytes;
		free(c->pcm);
		c->pcm = nil;
		c->pcmBytes = 0;
		c->allocBytes = 0;
		c->pcmOwned = FALSE;
	}
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

void
cSampleManager::SetEffectsFadeVolume(uint8 nVolume)
{
	gEffectsFade = nVolume;
}

void
cSampleManager::SetMusicFadeVolume(uint8 nVolume)
{
	gMusicFade = nVolume;
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
		strcpy(dot, (dot[1] == 'w' || dot[1] == 'W') ? ".wav" : ".ogg");
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
	bool8    adpcm;         // native IMA ADPCM .wav (voice) rather than Vorbis
	uint8    adpcmSpill[4224];  // decoded samples that did not fit the last chunk
	uint32   adpcmSpillBytes;
	uint16   blockAlign;    // ADPCM block size in bytes
	uint32   dataBytes;     // ADPCM payload length
	bool8    playing;
	bool8    paused;
	bool8    looping;
	char     path[80];      // for fail-loud reporting
};
static GcStream gStreams[MAX_STREAMS];

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
	if(st->bufReady){
		AESND_SetVoiceBuffer(pb, st->buf[st->play], STREAM_CHUNK_BYTES);
		st->play ^= 1;
		st->bufReady = FALSE;
	}else if(st->eof){
		// Everything decoded has now been handed over and played. This is the
		// real end of the sound.
		st->playing = FALSE;
		AESND_SetVoiceStop(pb, true);
	}else
		st->starved++;
}

// Tremor pulls straight from the file. Tremor is the fixed-point Vorbis
// decoder: the Gekko's FPU is fast, but the reference libvorbis leans on
// doubles, and integer decode is what consoles use.
// ponytail: direct reads are free under Dolphin; a real Mini-DVD wants a bulk
// read-ahead ring here to kill the per-decode seeks.
static size_t
gcVorbisRead(void *ptr, size_t size, size_t nmemb, void *datasource)
{
	GcStream *st = (GcStream*)datasource;
	if(st->file == nil)
		return 0;
	return fread(ptr, size, nmemb, st->file);
}

static int
gcVorbisSeek(void *datasource, ogg_int64_t offset, int whence)
{
	GcStream *st = (GcStream*)datasource;
	if(st->file == nil)
		return -1;
	return fseek(st->file, (long)offset, whence);
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
	return st->file ? ftell(st->file) : -1;
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

// Parse the RIFF header: rate, channels, block size and where data starts.
static bool8
gcWavOpen(GcStream *st)
{
	uint8 h[64];
	if(fseek(st->file, 0, SEEK_SET) != 0 || fread(h, 1, 12, st->file) != 12)
		return FALSE;
	if(memcmp(h, "RIFF", 4) != 0 || memcmp(h+8, "WAVE", 4) != 0)
		return FALSE;
	uint32 fmtTag = 0;
	bool8 haveFmt = FALSE;
	for(;;){
		uint8 ck[8];
		if(fread(ck, 1, 8, st->file) != 8)
			return FALSE;
		uint32 sz = (uint32)ck[4] | ((uint32)ck[5]<<8) | ((uint32)ck[6]<<16) | ((uint32)ck[7]<<24);
		if(memcmp(ck, "fmt ", 4) == 0){
			uint32 n = sz > sizeof(h) ? sizeof(h) : sz;
			if(fread(h, 1, n, st->file) != n)
				return FALSE;
			fmtTag       = (uint32)h[0] | ((uint32)h[1]<<8);
			st->channels = (uint32)h[2] | ((uint32)h[3]<<8);
			st->rate     = (uint32)h[4] | ((uint32)h[5]<<8) |
			               ((uint32)h[6]<<16) | ((uint32)h[7]<<24);
			st->blockAlign = (uint16)((uint32)h[12] | ((uint32)h[13]<<8));
			haveFmt = TRUE;
			if(sz > n && fseek(st->file, (long)(sz - n), SEEK_CUR) != 0)
				return FALSE;
		}else if(memcmp(ck, "data", 4) == 0){
			if(!haveFmt)
				return FALSE;
			st->dataStart = (uint32)ftell(st->file);
			st->dataBytes = sz;
			break;
		}else if(fseek(st->file, (long)((sz + 1) & ~1u), SEEK_CUR) != 0)
			return FALSE;
	}
	if(st->channels != 1 || st->rate == 0)
		return FALSE;           // every voice line in the game is mono
	if(fmtTag == 17 && st->blockAlign > 4){
		st->adpcm = TRUE;
		uint32 bs = gcAdpcmBlockSamples(st->blockAlign);
		st->lenSamples = (st->dataBytes / st->blockAlign) * bs;
	}else if(fmtTag == 1){
		st->adpcm = FALSE;      // plain PCM: 28 of the 1120 files are 16-bit
		st->blockAlign = 0;
		st->lenSamples = st->dataBytes/2;
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
		size_t got = fread(dst, 1, STREAM_CHUNK_BYTES, st->file);
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
			if(fread(blk, 1, ba, st->file) != ba)
				break;
			int32 pred = (int16)((uint16)blk[0] | ((uint16)blk[1] << 8));
			int32 idx = blk[2];
			if(idx > 88) idx = 88;
			uint32 n = 0;
			tmp[n++] = (int16)pred;
			for(uint32 i = 4; i < ba && n + 2 <= ARRAY_SIZE(tmp); i++){
				tmp[n++] = gcImaNibble(blk[i] & 15, &pred, &idx);
				tmp[n++] = gcImaNibble(blk[i] >> 4, &pred, &idx);
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
	st->posSamples += done/2;   // mono 16-bit
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
			GcStream probe;
			memset(&probe, 0, sizeof(probe));
			probe.file = f;
			if(gcWavOpen(&probe) && probe.rate)
				gTrackLengthMs[i] = (uint32)((uint64)probe.lenSamples*1000/probe.rate);
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

// Fill dst with up to STREAM_CHUNK_BYTES of 16-bit stereo PCM at DIGITALRATE.
// Returns bytes produced; 0 means end of track.
//
// Placeholder until the converter settles the disc format. It reads raw
// interleaved PCM, which is what a decoded stream looks like, so the pump can
// be exercised before the codec exists.
static uint32
gcStreamDecode(GcStream *st, uint8 *dst)
{
	if(st->file && !st->vfOpen)
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


// Keep one decoded chunk ahead of the DSP. The callback consumes it with a
// pointer swap; this refills on the game thread, where file reads belong.
static void
gcStreamPump(GcStream *st)
{
	if(!st->playing || st->paused || st->bufReady || st->eof || st->voice == nil)
		return;
	uint8 *dst = st->buf[st->fill];
	if(dst == nil)
		return;
	// Taken after the early-outs, so an idle stream does not contend with the
	// streaming worker sixty times a second for nothing.
	DVD_FS_GUARD;
	uint32 got = gcStreamDecode(st, dst);
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
static void
gcStreamArm(GcStream *st, uint8 nStream)
{
	(void)nStream;
	if(st->voice == nil)
		return;
	AESND_SetVoiceStream(st->voice, true);
	if(!st->bufReady)
		gcStreamPump(st);
	if(st->bufReady){
		AESND_SetVoiceBuffer(st->voice, st->buf[st->play], STREAM_CHUNK_BYTES);
		st->play ^= 1;
		st->bufReady = FALSE;
		gcStreamPump(st);        // the next chunk waits for the first callback
	}
	AESND_SetVoiceStop(st->voice, false);
}

void
cSampleManager::Service(void)
{
	// AESND mixes on the DSP; what the CPU owes it each frame is the next
	// block of stream data. Reading from disc here rather than in the voice
	// callback keeps file I/O off the audio path.
	// gxAudioUs: what this costs the frame, for the a= profile field — the
	// number that decides whether radio decode is the stutter.
	extern unsigned gxAudioUs;
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
		if(!c->used && c->pcmOwned && c->allocBytes > GC_CONV_RECLAIM){
			gConvBytes -= c->allocBytes;
			free(c->pcm);
			c->pcm = nil;
			c->pcmBytes = 0;
			c->allocBytes = 0;
			c->pcmOwned = FALSE;
		}
	}
	for(int32 i = 0; i < MAX_STREAMS; i++)
		gcStreamPump(&gStreams[i]);
	gxAudioUs = (unsigned)ticks_to_microsecs(gettime() - t0);

	// AHB: the audio system's own heartbeat, to the card every ~5s. One line
	// answers the questions the mute reports keep raising: are CHANNELS being
	// started at all (SFX requested), are the streams open/playing/paused,
	// and did anything die since the last beat.
	{
		static u64 lastBeat;
		if(ticks_to_millisecs(gettime() - lastBeat) >= 5000){
			lastBeat = gettime();
			int used = 0, playing = 0;
			for(uint32 i = 0; i < ARRAY_SIZE(gChannels); i++){
				if(gChannels[i].used) used++;
				if(gChannels[i].playing) playing++;
			}
			char sline[64]; int sn = 0;
			for(int32 i = 0; i < MAX_STREAMS; i++)
				sn += snprintf(sline+sn, sizeof(sline)-sn, " s%d=%c%c",
				    i, gStreams[i].file ? 'F' : '-',
				    gStreams[i].playing ? (gStreams[i].paused ? 'p' : 'P') : '-');
			// Unguarded, this raced the streaming worker inside libfat every
			// 5s of gameplay — the binary junk blocks in audio.log were the
			// visible half; the smashed heap (_calloc_r/__sflush_r deaths at
			// 0xC) was the other.
			DVD_FS_GUARD;
			FILE *al = fopen("dvd:/audio.log", "a");
			if(al){
				fprintf(al, "AHB ch=%d/%d%s vol=%u/%u conv=%u/%u pool=%uK/%uK\n",
				    playing, used, sline,
				    (unsigned)gEffectsVolume, (unsigned)gMusicVolume,
				    (unsigned)gConvOk, (unsigned)(gConvOk + gConvFallback),
				    (unsigned)(gConvPeak/1024),
				    (unsigned)(GC_CONV_BUDGET/1024));
				fclose(al);
			}
		}
	}
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
	if(gPedBuf == nil){
#ifdef HW_RVL
		gPedBuf = (uint8*)gcBankAlloc(align32(PED_BLOCKSIZE*MAX_PEDSFX));
#else
		gPedBuf = (uint8*)memalign(32, PED_BLOCKSIZE*MAX_PEDSFX);
#endif
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

// Cutscene speech arrives through this pair: MusicManager preloads the track
// while the scene sets up, then releases it on the first frame. Open-paused is
// exactly that.
// TEMP (remove at bring-up close): the mission-audio slots, 1 and 2, are
// where cutscene speech lives, and the game gives a stream 30 frames to
// report itself playing before it writes the line off as finished and moves
// to the next one. This records every transition on those slots so the
// reason a line is skipped is a log line rather than an argument.
static void
gcStreamTrace(const char *what, uint8 nStream)
{
	if(nStream == 0 || nStream >= MAX_STREAMS)
		return;                       // slot 0 is music, not speech
	static int32 left = 120;
	if(left-- <= 0)
		return;
	GcStream *st = &gStreams[nStream];
	DVD_FS_GUARD;
	FILE *f = fopen("dvd:/audio.log", "a");
	if(f){
		fprintf(f, "MA s%u %-10s play=%d paused=%d file=%d rate=%u pos=%u/%u t=%u\n",
		    (unsigned)nStream, what, (int)st->playing, (int)st->paused,
		    st->file != nil, (unsigned)st->rate,
		    (unsigned)st->posSamples, (unsigned)st->lenSamples,
		    (unsigned)ticks_to_millisecs(gettime()));
		fclose(f);
	}
}

void
cSampleManager::PreloadStreamedFile(tTrack nFile, uint8 nStream)
{
	if(StartStreamedFile(nFile, 0, nStream)){
		// Hold it: opened and primed, voice silent until the scene asks.
		GcStream *st = &gStreams[nStream];
		st->paused = TRUE;
		if(st->voice)
			AESND_SetVoiceStop(st->voice, true);
	}
	gcStreamTrace("preload", nStream);
}

void
cSampleManager::PauseStream(bool8 nPauseFlag, uint8 nStream)
{
	if(nStream >= MAX_STREAMS)
		return;
	GcStream *st = &gStreams[nStream];
	st->paused = nPauseFlag;
	if(st->voice)
		AESND_SetVoiceStop(st->voice, nPauseFlag || !st->playing);
}

void
cSampleManager::StartPreloadedStreamedFile(uint8 nStream)
{
	if(nStream >= MAX_STREAMS)
		return;
	GcStream *st = &gStreams[nStream];
	st->paused = FALSE;
	// Re-arm rather than merely un-stop: see gcStreamArm.
	gcStreamArm(st, nStream);
	gcStreamTrace("start", nStream);
}

bool8
cSampleManager::StartStreamedFile(tTrack nFile, uint32 nPos, uint8 nStream)
{
	if(nStream >= MAX_STREAMS)
		return FALSE;
	GcStream *st = &gStreams[nStream];
	u64 tOpen = gettime();
	StopStreamedFile(nStream);
	// Spans the fopen, ov_open_callbacks (which reads headers) and the priming
	// pump. The lock is recursive, so the nested guards inside are free.
	DVD_FS_GUARD;

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
		GeckoLog(gl);
	}
	strncpy(st->path, path, sizeof(st->path)-1);
	st->path[sizeof(st->path)-1] = '\0';
	st->file = fopen(path, "rb");
	if(st->file == nil){
		// A silent FALSE here is a silent GAME: cutscene speech and radio
		// both die invisibly on a bad path. Card, not Gecko — Gecko drops it.
		FILE *al = fopen("dvd:/audio.log", "a");
		if(al){ fprintf(al, "STRM OPEN-FAIL s%d %s\n", (int)nStream, path); fclose(al); }
		gcAudioDie("stream-open", path);
		return FALSE;
	}
	{
		struct mallinfo smi = mallinfo();
		unsigned openMs = (unsigned)ticks_to_millisecs(gettime() - tOpen);
		FILE *al = fopen("dvd:/audio.log", "a");
		if(al){ fprintf(al, "STRM ok s%d %s pos=%u free=%uK open=%ums\n",
		    (int)nStream, path, (unsigned)nPos,
		    (unsigned)smi.fordblks/1024, openMs); fclose(al); }
	}
	// stdio buffering for this stream must not depend on the allocator: at
	// 441K free the fread inside ov_open failed its buffer malloc and the
	// open died OV_EREAD on a perfectly good file.
	{
		static char stdioBuf[MAX_STREAMS][32*1024] __attribute__((aligned(32)));
		setvbuf(st->file, stdioBuf[nStream], _IOFBF, sizeof(stdioBuf[0]));
	}

	if(st->voice == nil){
		st->voice = AESND_AllocateVoiceWithArg(gcStreamCallback, st);
		if(st->voice == nil){ fclose(st->file); st->file = nil; return FALSE; }
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
		fclose(st->file); st->file = nil; return FALSE;
	}

	// Voice is native: no Vorbis, no decode state, no allocation.
	const char *ext = strrchr(path, '.');
	bool8 isWav = ext && (ext[1] == 'w' || ext[1] == 'W');
	if(isWav){
		if(!gcWavOpen(st)){
			fclose(st->file); st->file = nil;
			gcAudioDie("stream-open-wav", path);
			return FALSE;
		}
		st->posSamples = 0;
		if(nPos && st->rate){
			uint32 want = (uint32)((uint64)nPos*st->rate/1000);
			if(st->lenSamples) want %= st->lenSamples;
			uint32 off = st->adpcm ?
			    (want/gcAdpcmBlockSamples(st->blockAlign))*st->blockAlign :
			    want*2;
			if(fseek(st->file, (long)(st->dataStart + off), SEEK_SET) == 0)
				st->posSamples = want;
		}
	}else{
	int ovrc = ov_open_callbacks(st, &st->vf, nil, 0, gcVorbisCallbacks);
	if(ovrc < 0){
		fclose(st->file); st->file = nil;
		// OV_EFAULT here is usually Tremor failing to malloc its decode
		// state, not a bad file — the rc tells them apart.
		char od[100];
		snprintf(od, sizeof(od), "%s rc=%d", path, ovrc);
		gcAudioDie("stream-open-vorbis", od);
		return FALSE;
	}
	st->vfOpen = TRUE;
	// The voice plays at the file's rate. Feeding the DSP below its 48kHz
	// output makes the ucode resample by sample-repeat — no interpolation —
	// and the aliasing images were measured as loud as the real top octave
	// (the "metallic" radio). 48kHz files sidestep the resampler entirely.
	vorbis_info *vi = ov_info(&st->vf, -1);
	st->rate = vi ? (uint32)vi->rate : DIGITALRATE;
	st->channels = vi && vi->channels == 1 ? 1 : 2;
	st->lenSamples = (uint32)ov_pcm_total(&st->vf, -1);
	// The radio is wall-clock synced: the game hands the station's position in
	// ms and expects playback from there, not from the top of the tape.
	st->posSamples = 0;
	if(nPos){
		ogg_int64_t want = (ogg_int64_t)nPos*(st->rate/1000);
		if(st->lenSamples)
			want %= (ogg_int64_t)st->lenSamples;
		// PAGE seek, not sample-accurate seek. ov_pcm_seek bisects the file
		// and then decodes forward to land on the exact sample; over a 70MB
		// station on SD that was measured at up to 148ms inside one frame -
		// the largest single number in the whole profile and the stutter the
		// user reported. Radio is wall-clock synced to within a page (a few
		// tens of ms), which nobody can hear, and the page seek skips the
		// decode-forward entirely.
		if(ov_pcm_seek_page(&st->vf, want) == 0){
			st->posSamples = (uint32)ov_pcm_tell(&st->vf);
			// THROW THE FIRST BLOCK AWAY. Vorbis reconstructs every block from
			// the overlapped half of the one before it, and a raw page seek
			// lands mid-stream with that half missing - so the first decode
			// after one is a burst of noise, not audio. That burst is the white
			// noise in the menu: the frontend plays the tuned station and starts
			// it at a wall-clock position, so every restart played it, while the
			// radio-select screen sounded clean because it starts from zero.
			// One discarded chunk (84ms) costs nothing and the decoder is fully
			// primed by the next one.
			gcStreamDecode(st, st->buf[0]);
		}
	}
	}
	st->fill = 0;
	st->play = 0;
	st->adpcmSpillBytes = 0;   // no carry from the previous line
	st->bufReady = FALSE;
	st->eof = FALSE;
	st->starved = 0;
	st->paused = FALSE;
	st->playing = TRUE;

	AESND_SetVoiceFormat(st->voice, st->channels == 1 ? VOICE_MONO16 : VOICE_STEREO16);
	AESND_SetVoiceFrequency(st->voice, (f32)st->rate);
	AESND_SetVoiceVolume(st->voice, 255, 255);

	// Arm the voice for streaming BEFORE handing it a buffer - that is the
	// order AESND's stream voices expect, and reversing it wedged the fourth
	// stream of a sweep. The white noise this was chasing came from the chunk
	// allocation being uninitialised, not from the ordering: the buffers are
	// zeroed at allocation now, so the window between arming and the first
	// chunk plays silence instead of whatever MEM1 held.
	gcStreamArm(st, nStream);
	if(!st->bufReady && st->posSamples == 0){
		// Nothing decoded. Say so rather than let silence pass for success.
		DVD_FS_GUARD;
		FILE *al = fopen("dvd:/audio.log", "a");
		if(al){ fprintf(al, "STRM PRIME-EMPTY s%d %s\n", (int)nStream, path); fclose(al); }
	}
	return TRUE;
}

void
cSampleManager::StopStreamedFile(uint8 nStream)
{
	if(nStream >= MAX_STREAMS)
		return;
	GcStream *st = &gStreams[nStream];
	if(st->vfOpen){
		char gl[32];
		snprintf(gl, sizeof(gl), "STRM stop s%d", (int)nStream);
		GeckoLog(gl);
	}
	if(st->voice)
		AESND_SetVoiceStop(st->voice, true);
	DVD_FS_GUARD;
	if(st->vfOpen){ ov_clear(&st->vf); st->vfOpen = FALSE; }
	if(st->file){ fclose(st->file); st->file = nil; }
	st->playing = FALSE;
	st->posSamples = 0;
}

int32
cSampleManager::GetStreamedFilePosition(uint8 nStream)
{
	// In milliseconds, which is what the music manager expects.
	if(nStream >= MAX_STREAMS)
		return 0;
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
	// One trace of the first few answers on a speech slot: this is the reply
	// the mission-audio state machine acts on.
	if(nStream != 0 && nStream < MAX_STREAMS){
		static int32 seen[MAX_STREAMS];
		if(seen[nStream] < 6){
			seen[nStream]++;
			gcStreamTrace("isplaying?", nStream);
		}
	}
	// OAL parity: a paused stream reads as NOT playing (CStream::IsPlaying
	// returns false under m_bPaused). MusicManager's mode-change handshake
	// depends on it — reading TRUE here made Service stop a preloaded
	// (paused) cutscene track instead of completing the switch cleanly.
	return nStream < MAX_STREAMS && gStreams[nStream].playing &&
	       !gStreams[nStream].paused ? TRUE : FALSE;
}

static void
gcStreamsShutdown(void)
{
	for(int32 i = 0; i < MAX_STREAMS; i++){
		SampleManager.StopStreamedFile(i);
		if(gStreams[i].voice){
			AESND_FreeVoice(gStreams[i].voice);
			gStreams[i].voice = nil;
		}
	}
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
	return GC_CHANNEL_VOICES;
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
void
cSampleManager::SetStreamedVolumeAndPan(uint8 nVolume, uint8 nPan, bool8 nEffectFlag, uint8 nStream)
{
	if(nStream >= MAX_STREAMS)
		return;
	GcStream *st = &gStreams[nStream];
	if(st->voice == nil)
		return;
	uint32 vol = nVolume*(nEffectFlag ? gEffectsVolume : gMusicVolume)/127;
	vol = vol*(nEffectFlag ? gEffectsFade : gMusicFade)/127;
	if(vol > 127) vol = 127;
	uint32 base = vol*255/127;
	uint32 pan = nPan > 127 ? 127 : nPan;
	// Same model as the channels: full scale at centre, pan attenuates only.
	uint32 lf = 127 - pan, rf = pan;
	uint32 l32 = lf >= 63 ? base : base*lf/63;
	uint32 r32 = rf >= 63 ? base : base*rf/63;
	AESND_SetVoiceVolume(st->voice,
	    (u16)(l32 > 255 ? 255 : l32), (u16)(r32 > 255 ? 255 : r32));
}

#endif // AUDIO_GAMECUBE

// ---------------------------------------------------------- conformance test
//
// Armed by dvd:/audiotest.txt. Sweeps EVERY category of audio the game has -
// bank effects across the whole rate range, all nine radio stations, mission
// voice, ambience - measures what each one actually produces, and writes the
// numbers to dvd:/audiotest.log before the game ever boots.
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
	GeckoLog(line);
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
	DVD_FS_GUARD;
	FILE *f = fopen("dvd:/audiotest.log", "w");
	if(f){ fwrite(gTestBuf, 1, gTestLen, f); fclose(f); }
}

static void
gcAudioSelfTest(void)
{
	{
		DVD_FS_GUARD;
		FILE *f = fopen("dvd:/audiotest.txt", "r");
		if(f == nil)
			return;
		fclose(f);
	}
	gEffectsVolume = 127;
	gEffectsFade = 127;
	gMusicVolume = 127;
	gMusicFade = 127;
	gcTestLog("AUDIOTEST begin");

	// --- streams first: the nine stations, then ambience and voice. Opening
	// one proves nothing; the RMS of a decoded chunk is the evidence.
	for(uint32 t = 0; t < 12 && t < ARRAY_SIZE(StreamedNameTable); t++){
		char path[80];
		gcTrackPath(t, path, sizeof(path));
		if(!SampleManager.StartStreamedFile(t, 0, 0)){
			gcTestLog("STREAM %u %s OPEN-FAILED", (unsigned)t, path);
			continue;
		}
		GcStream *st = &gStreams[0];
		uint32 rms = 0;
		for(uint32 tries = 0; tries < 6 && rms == 0; tries++){
			st->bufReady = FALSE;
			gcStreamPump(st);
			if(st->bufReady)
				rms = gcRms((const int16*)st->buf[st->fill ^ 1],
				    STREAM_CHUNK_BYTES/2);
		}
		gcTestLog("STREAM %u %s %uHz ch%u len=%u rms=%u%s",
		    (unsigned)t, path, (unsigned)st->rate, (unsigned)st->channels,
		    (unsigned)st->lenSamples, (unsigned)rms,
		    rms == 0 ? "  SILENT" : "");
		gcTestFlush();          // partial sweeps must still leave evidence
		SampleManager.SetStreamedVolumeAndPan(127, 63, 0, 0);
		usleep(2000*1000);
		SampleManager.StopStreamedFile(0);
		usleep(1000*1000);   // gap: the host segments the dump on silence
	}

	// --- bank effects across the rate and size range
	for(uint32 i = 0; i < ARRAY_SIZE(gAudioTestSfx); i++){
		uint32 sfx = gAudioTestSfx[i];
		if(sfx >= gNumSamples){
			gcTestLog("SFX %u OUT-OF-TABLE", (unsigned)sfx);
			continue;
		}
		if(!SampleManager.InitialiseChannel(0, sfx, 0)){
			gcTestLog("SFX %u INIT-FAILED", (unsigned)sfx);
			continue;
		}
		GcChannel *c = &gChannels[0];
		uint32 rms = gcRms((const int16*)c->pcm, c->pcmBytes/2);
		gcTestLog("SFX %u src=%uB %uHz -> %uB conv=%d rms=%u",
		    (unsigned)sfx, (unsigned)gSampleIndex[sfx].nSize,
		    (unsigned)gSampleIndex[sfx].nFrequency,
		    (unsigned)c->pcmBytes, (int)c->pcm48, (unsigned)rms);
		SampleManager.SetChannelVolume(0, 127);
		SampleManager.SetChannelPan(0, 63);
		SampleManager.SetChannelLoopCount(0, 1);
		gcTestFlush();
		SampleManager.StartChannel(0);
		usleep(2000*1000);
		SampleManager.StopChannel(0);
		usleep(1000*1000);
	}

	gcTestLog("AUDIOTEST end");
	gcTestFlush();
	extern void gcFatalPark(const char *tag, const char *msg);
	gcFatalPark("AUDIOTEST", "sweep complete; see dvd:/audiotest.log");
}

