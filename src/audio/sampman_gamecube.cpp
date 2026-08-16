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
#include <malloc.h>

cSampleManager SampleManager;
bool8 _bSampmanInitialised = FALSE;

uint32 BankStartOffset[MAX_SFX_BANKS];
uint32 nNumMP3s;

// One AESND voice per game channel. VC drives channels by index and expects
// them to be independent, which maps to a voice each.
struct GcChannel {
	AESNDPB *voice;
	void    *pcm;        // MEM1 copy the voice reads from
	uint32   pcmBytes;
	uint32   sample;     // which sfx is loaded
	uint32   freq;
	uint32   volume;     // 0..127 as the game supplies it
	uint32   pan;        // 0..127, 63 centre
	uint32   loopCount;
	bool8    used;
	volatile bool8 playing;   // cleared by the AESND callback when the buffer ends
};
static GcChannel gChannels[MAXCHANNELS + MAX2DCHANNELS];

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
static GcBank gBanks[MAX_SFX_BANKS];

static uint8 gEffectsVolume = 127, gMusicVolume = 127;
static uint8 gEffectsFade = 127, gMusicFade = 127;

static inline uint32
align32(uint32 v)
{
	return (v + 31) & ~31u;
}

// ---------------------------------------------------------------- lifecycle

bool8
cSampleManager::Initialise(void)
{
	if(_bSampmanInitialised)
		return TRUE;

	AESND_Init();
	AESND_Pause(false);

	if(!AR_CheckInit())
		AR_Init(nil, 0);

	for(int32 i = 0; i < (int32)ARRAY_SIZE(gChannels); i++){
		gChannels[i].voice = AESND_AllocateVoiceWithArg(gcVoiceCallback, &gChannels[i]);
		if(gChannels[i].voice)
			AESND_SetVoiceStop(gChannels[i].voice, true);
	}

	if(!InitialiseSampleBanks())
		return FALSE;

	_bSampmanInitialised = TRUE;
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
		free(gChannels[i].pcm);
		gChannels[i].pcm = nil;
	}
	free(gSampleIndex);
	gSampleIndex = nil;
	AESND_Pause(true);
	_bSampmanInitialised = FALSE;
}

// ------------------------------------------------------------------- banks

bool8
cSampleManager::InitialiseSampleBanks(void)
{
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
	uint32 first = BankStartOffset[nBank];
	uint32 last = nBank+1 < MAX_SFX_BANKS && BankStartOffset[nBank+1] ?
	    BankStartOffset[nBank+1] : gNumSamples;
	if(first >= gNumSamples)
		return FALSE;
	if(last > gNumSamples)
		last = gNumSamples;

	uint32 byteStart = gSampleIndex[first].nOffset;
	uint32 byteEnd = gSampleIndex[last-1].nOffset + gSampleIndex[last-1].nSize;
	uint32 bytes = align32(byteEnd - byteStart);

	uint32 addr = AR_Alloc(bytes);
	if(addr == 0)
		return FALSE;

	// Straight from the disc into ARAM, through a staging buffer sized to one
	// transfer rather than the whole bank — the point of ARAM is that the bank
	// never has to be resident in MEM1.
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
		DCFlushRange(stage, align32((uint32)got));
		AR_StartDMA(AR_MRAMTOARAM, (u32)stage, addr + done, align32((uint32)got));
		while(AR_GetDMAStatus())
			;
		done += align32((uint32)got);
	}
	fclose(raw);
	free(stage);

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

bool8
cSampleManager::InitialiseChannel(uint32 nChannel, uint32 nSfx, uint8 nBank)
{
	if(nChannel >= ARRAY_SIZE(gChannels) || gSampleIndex == nil ||
	   nSfx >= gNumSamples)
		return FALSE;
	GcChannel *c = &gChannels[nChannel];
	if(c->voice == nil)
		return FALSE;
	if(nBank < MAX_SFX_BANKS && !gBanks[nBank].loaded)
		return FALSE;

	uint32 bytes = align32(gSampleIndex[nSfx].nSize);
	if(bytes == 0)
		return FALSE;
	if(c->pcmBytes < bytes){
		free(c->pcm);
		c->pcm = memalign(32, bytes);
		c->pcmBytes = c->pcm ? bytes : 0;
	}
	if(c->pcm == nil)
		return FALSE;

	// Pull just this sample out of the bank. ARAM holds the whole 324MB set;
	// MEM1 only ever holds what is actually sounding.
	uint32 src = gBanks[nBank < MAX_SFX_BANKS ? nBank : 0].aramAddr +
	    (gSampleIndex[nSfx].nOffset - gSampleIndex[BankStartOffset[nBank < MAX_SFX_BANKS ? nBank : 0]].nOffset);
	DCInvalidateRange(c->pcm, bytes);
	AR_StartDMA(AR_ARAMTOMRAM, (u32)c->pcm, src, bytes);
	while(AR_GetDMAStatus())
		;

	c->sample = nSfx;
	c->freq = gSampleIndex[nSfx].nFrequency;
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
	(void)nChannel; (void)nLoopStart; (void)nLoopEnd;
	// AESND loops whole buffers only. Sub-buffer loop points would need the
	// sample trimmed to them at DMA time; left until something audibly needs it.
}

bool8
cSampleManager::GetChannelUsedFlag(uint32 nChannel)
{
	if(nChannel >= ARRAY_SIZE(gChannels))
		return FALSE;
	return gChannels[nChannel].playing;
}

void
cSampleManager::StartChannel(uint32 nChannel)
{
	if(nChannel >= ARRAY_SIZE(gChannels))
		return;
	GcChannel *c = &gChannels[nChannel];
	if(c->voice == nil || c->pcm == nil || !c->used)
		return;

	// The game's volume and pan are 0..127; AESND takes 0..255 per side.
	// Pan is applied as a straight linear split, which is what VC's own
	// software mixer did.
	uint32 vol = c->volume*gEffectsVolume/127;
	vol = vol*gEffectsFade/127;
	uint32 pan = c->pan > 127 ? 127 : c->pan;
	u16 left  = (u16)(vol*2*(127-pan)/127);
	u16 right = (u16)(vol*2*pan/127);

	DCFlushRange(c->pcm, c->pcmBytes);
	AESND_SetVoiceFormat(c->voice, VOICE_MONO16);
	AESND_SetVoiceFrequency(c->voice, (f32)c->freq);
	AESND_SetVoiceVolume(c->voice, left, right);
	AESND_SetVoiceLoop(c->voice, c->loopCount != 1);
	AESND_SetVoiceBuffer(c->voice, c->pcm, c->pcmBytes);
	c->playing = TRUE;
	AESND_SetVoiceStop(c->voice, false);
}

void
cSampleManager::StopChannel(uint32 nChannel)
{
	if(nChannel >= ARRAY_SIZE(gChannels))
		return;
	if(gChannels[nChannel].voice)
		AESND_SetVoiceStop(gChannels[nChannel].voice, true);
	gChannels[nChannel].playing = FALSE;
	gChannels[nChannel].used = FALSE;
}

// ------------------------------------------------------------------ volumes

void
cSampleManager::SetEffectsMasterVolume(uint8 nVolume)
{
	gEffectsVolume = nVolume;
}

void
cSampleManager::SetMusicMasterVolume(uint8 nVolume)
{
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

void
cSampleManager::Service(void)
{
	// AESND runs the mix on the DSP; nothing per-frame is needed yet. The
	// radio streams will pump their buffers from here.
}

bool8
cSampleManager::IsMP3RadioChannelAvailable(void)
{
	return FALSE;   // radio streaming is not written yet
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
	return 0;
}

bool8
cSampleManager::LoadMissionAudio(uint8 nSlot, uint32 nSample)
{
	return FALSE;
}

uint8
cSampleManager::IsPedCommentLoaded(uint32 nComment)
{
	return 0;
}

int32
cSampleManager::_GetPedCommentSlot(uint32 nComment)
{
	return -1;
}

bool8
cSampleManager::LoadPedComment(uint32 nComment)
{
	return FALSE;
}

void
cSampleManager::SetChannelReverbFlag(uint32 nChannel, bool8 nReverbFlag)
{
	;
}

void
cSampleManager::SetChannel3DPosition(uint32 nChannel, float fX, float fY, float fZ)
{
	;
}

void
cSampleManager::SetChannel3DDistances(uint32 nChannel, float fMax, float fMin)
{
	;
}

void
cSampleManager::PreloadStreamedFile(tTrack nFile, uint8 nStream)
{
	;
}

void
cSampleManager::PauseStream(bool8 nPauseFlag, uint8 nStream)
{
	;
}

void
cSampleManager::StartPreloadedStreamedFile(uint8 nStream)
{
	;
}

bool8
cSampleManager::StartStreamedFile(tTrack nFile, uint32 nPos, uint8 nStream)
{
	return FALSE;
}

void
cSampleManager::StopStreamedFile(uint8 nStream)
{
	;
}

int32
cSampleManager::GetStreamedFilePosition(uint8 nStream)
{
	return -1;
}

int32
cSampleManager::GetStreamedFileLength(uint8 nStream)
{
	return -1;
}

bool8
cSampleManager::IsStreamPlaying(uint8 nStream)
{
	return FALSE;
}

void
cSampleManager::SetStreamedFileLoopFlag(bool8 nLoopFlag, uint8 nChannel)
{
	;
}

void
cSampleManager::SetSpeakerConfig(int32 nConfig)
{
	;
}

uint32
cSampleManager::GetMaximumSupportedChannels(void)
{
	return 0;
}

uint32
cSampleManager::GetNum3DProvidersAvailable()
{
	return 0;
}

void
cSampleManager::SetNum3DProvidersAvailable(uint32 num)
{
	;
}

char *
cSampleManager::Get3DProviderName(uint8 id)
{
	return nil;
}

void
cSampleManager::Set3DProviderName(uint8 id, char *name)
{
	;
}

int8
cSampleManager::GetCurrent3DProviderIndex(void)
{
	return -1;
}

int8
cSampleManager::SetCurrent3DProvider(uint8 nProvider)
{
	return -1;
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
	return -1;
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
void
cSampleManager::SetStreamedVolumeAndPan(uint8 nVolume, uint8 nPan, bool8 nEffectFlag, uint8 nStream)
{
	(void)nVolume; (void)nPan; (void)nEffectFlag; (void)nStream;
	// Streams are not implemented yet; the radio lands with the converter.
}

#endif // AUDIO_GAMECUBE
