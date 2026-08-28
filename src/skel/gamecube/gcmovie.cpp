#include "common.h"

#ifdef GTA_OGC

#include "gcmovie.h"

#include <aesndlib.h>
#include <gccore.h>
#include <malloc.h>
#include <ogg/ogg.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <string.h>
#include <theora/theoradec.h>
#include <tremor/ivorbiscodec.h>

extern void GeckoLog(const char *msg);
extern "C" void CdStreamFsLock(void);
extern "C" void CdStreamFsUnlock(void);
extern void *gxMovieXfb;

namespace {

enum {
	MOVIE_IO_BYTES = 32 * 1024,
	// AESND consumes 1152-byte DSP staging blocks and zero-pads a partial last
	// block. 65536 bytes therefore inserted 32 silent stereo frames at every
	// handoff — the periodic crackle. Keep the same ~336ms margin, but make the
	// whole buffer an exact number of native blocks.
	MOVIE_AUDIO_BYTES = DSP_STREAMBUFFER_SIZE * 56,
	MOVIE_AUDIO_FRAMES = MOVIE_AUDIO_BYTES / (2 * sizeof(int16)),
	// Decode one quarter-buffer per service point. Filling all 336ms in one
	// synchronous burst stalls the video; four small slices still finish long
	// before the DSP consumes the other full buffer.
	MOVIE_AUDIO_SLICE_FRAMES = MOVIE_AUDIO_FRAMES / 4,
	// libtheoradec owns several padded reference frames. Fail before decoder
	// setup when boot memory is too fragmented instead of dying inside malloc.
	MOVIE_DECODER_PREFLIGHT_BYTES = 2300 * 1024
};

enum PacketResult {
	PACKET_ERROR = -1,
	PACKET_END = 0,
	PACKET_OK = 1
};

static void
movieTrace(const char *line)
{
	// Normal boot stdout is redirected to OSReport after RenderWare starts, so
	// this reaches Dolphin/hardware diagnostics without touching the XFB.
	printf("%s\n", line);
	GeckoLog(line);
}

struct FsGuard {
	FsGuard(void) { CdStreamFsLock(); }
	~FsGuard(void) { CdStreamFsUnlock(); }
};

struct MovieDecoder {
	FILE *file;
	ogg_sync_state sync;
	ogg_stream_state videoStream;
	ogg_stream_state audioStream;
	bool syncReady;
	bool videoStreamReady;
	bool audioStreamReady;
	bool inputEof;
	bool failed;
	uint32 inputReads;

	th_info videoInfo;
	th_comment videoComment;
	th_setup_info *videoSetup;
	th_dec_ctx *videoDecoder;
	bool videoInfoReady;
	bool videoCommentReady;
	int videoHeaders;

	vorbis_info audioInfo;
	vorbis_comment audioComment;
	vorbis_dsp_state audioDsp;
	vorbis_block audioBlock;
	bool audioInfoReady;
	bool audioCommentReady;
	bool audioDspReady;
	bool audioBlockReady;
	int audioHeaders;

	th_ycbcr_buffer frame;
	bool frameReady;
	bool frameChanged;
	uint32 frameNumber;
	double frameTime;
	double frameStep;
	uint32 decodeMaxUs;
	u64 decodeTotalUs;
};

static bool
movieFail(MovieDecoder *movie, const char *reason)
{
	if(!movie->failed){
		char line[192];
		snprintf(line, sizeof(line), "FMV invalid: %s", reason);
		movieTrace(line);
	}
	movie->failed = true;
	return false;
}

static void
movieDecoderInit(MovieDecoder *movie)
{
	memset(movie, 0, sizeof(*movie));
	th_info_init(&movie->videoInfo);
	movie->videoInfoReady = true;
	th_comment_init(&movie->videoComment);
	movie->videoCommentReady = true;
	vorbis_info_init(&movie->audioInfo);
	movie->audioInfoReady = true;
	vorbis_comment_init(&movie->audioComment);
	movie->audioCommentReady = true;
}

static void
movieDecoderClose(MovieDecoder *movie)
{
	if(movie->audioBlockReady)
		vorbis_block_clear(&movie->audioBlock);
	if(movie->audioDspReady)
		vorbis_dsp_clear(&movie->audioDsp);
	if(movie->audioCommentReady)
		vorbis_comment_clear(&movie->audioComment);
	if(movie->audioInfoReady)
		vorbis_info_clear(&movie->audioInfo);
	if(movie->videoDecoder)
		th_decode_free(movie->videoDecoder);
	if(movie->videoSetup)
		th_setup_free(movie->videoSetup);
	if(movie->videoCommentReady)
		th_comment_clear(&movie->videoComment);
	if(movie->videoInfoReady)
		th_info_clear(&movie->videoInfo);
	if(movie->videoStreamReady)
		ogg_stream_clear(&movie->videoStream);
	if(movie->audioStreamReady)
		ogg_stream_clear(&movie->audioStream);
	if(movie->syncReady)
		ogg_sync_clear(&movie->sync);
	if(movie->file)
		fclose(movie->file);
	memset(movie, 0, sizeof(*movie));
}

// Returns one physical Ogg page. All input stays bounded by libogg's sync
// buffer; no movie-sized allocation and no second file/seek for audio.
static int
movieNextPage(MovieDecoder *movie, ogg_page *page)
{
	for(;;){
		int result = ogg_sync_pageout(&movie->sync, page);
		if(result == 1)
			return 1;
		if(result < 0){
			char line[128];
			snprintf(line, sizeof(line),
			    "Ogg sync lost after read=%u file=%ld", movie->inputReads,
			    ftell(movie->file));
			movieTrace(line);
			movieFail(movie, "corrupt Ogg page");
			return -1;
		}
		if(movie->inputEof)
			return 0;

		char *buffer = ogg_sync_buffer(&movie->sync, MOVIE_IO_BYTES);
		if(buffer == nil){
			movieFail(movie, "Ogg input allocation failed");
			return -1;
		}
		size_t bytes = fread(buffer, 1, MOVIE_IO_BYTES, movie->file);
		if(bytes == 0){
			if(ferror(movie->file)){
				movieFail(movie, "disc read failed");
				return -1;
			}
			movie->inputEof = true;
			continue;
		}
		if(ogg_sync_wrote(&movie->sync, (long)bytes) != 0){
			movieFail(movie, "Ogg input overflow");
			return -1;
		}
		movie->inputReads++;
		if(movie->inputReads <= 4){
			uint32 hash = 2166136261u;
			for(size_t i = 0; i < bytes; i++){
				hash ^= (uint8)buffer[i];
				hash *= 16777619u;
			}
			char line[144];
			snprintf(line, sizeof(line),
			    "FMV IO read=%u bytes=%u end=%ld fnv=%08X",
			    movie->inputReads, (unsigned)bytes, ftell(movie->file),
			    (unsigned)hash);
			movieTrace(line);
		}
	}
}

static bool
movieRoutePage(MovieDecoder *movie, ogg_page *page)
{
	int serial = ogg_page_serialno(page);
	if(movie->videoStreamReady && serial == movie->videoStream.serialno){
		if(ogg_stream_pagein(&movie->videoStream, page) != 0)
			return movieFail(movie, "Theora page rejected");
		return true;
	}
	if(movie->audioStreamReady && serial == movie->audioStream.serialno){
		if(ogg_stream_pagein(&movie->audioStream, page) != 0)
			return movieFail(movie, "Vorbis page rejected");
		return true;
	}
	return movieFail(movie, "unexpected Ogg logical stream");
}

static int
movieNextPacket(MovieDecoder *movie, ogg_stream_state *stream,
    ogg_packet *packet)
{
	for(;;){
		int result = ogg_stream_packetout(stream, packet);
		if(result == 1)
			return PACKET_OK;
		if(result < 0){
			movieFail(movie, "hole in Ogg packet stream");
			return PACKET_ERROR;
		}
		if(ogg_stream_eos(stream))
			return PACKET_END;

		ogg_page page;
		result = movieNextPage(movie, &page);
		if(result < 0)
			return PACKET_ERROR;
		if(result == 0)
			return PACKET_END;
		if(ogg_page_bos(&page)){
			movieFail(movie, "chained Ogg stream is unsupported");
			return PACKET_ERROR;
		}
		if(!movieRoutePage(movie, &page))
			return PACKET_ERROR;
	}
}

static bool
movieIdentifyBos(MovieDecoder *movie, ogg_page *page)
{
	ogg_stream_state candidate;
	memset(&candidate, 0, sizeof(candidate));
	if(ogg_stream_init(&candidate, ogg_page_serialno(page)) != 0)
		return movieFail(movie, "Ogg stream allocation failed");
	if(ogg_stream_pagein(&candidate, page) != 0){
		ogg_stream_clear(&candidate);
		return movieFail(movie, "Ogg BOS page rejected");
	}

	ogg_packet packet;
	if(ogg_stream_packetout(&candidate, &packet) != 1){
		ogg_stream_clear(&candidate);
		return movieFail(movie, "Ogg BOS packet missing");
	}

	if(!movie->videoStreamReady){
		int header = th_decode_headerin(&movie->videoInfo,
		    &movie->videoComment, &movie->videoSetup, &packet);
		if(header > 0){
			movie->videoStream = candidate;
			movie->videoStreamReady = true;
			movie->videoHeaders = 1;
			return true;
		}
		if(header != TH_ENOTFORMAT){
			ogg_stream_clear(&candidate);
			return movieFail(movie, "bad Theora identification header");
		}
	}

	if(!movie->audioStreamReady && vorbis_synthesis_idheader(&packet) == 1){
		if(vorbis_synthesis_headerin(&movie->audioInfo,
		    &movie->audioComment, &packet) != 0){
			ogg_stream_clear(&candidate);
			return movieFail(movie, "bad Vorbis identification header");
		}
		movie->audioStream = candidate;
		movie->audioStreamReady = true;
		movie->audioHeaders = 1;
		return true;
	}

	ogg_stream_clear(&candidate);
	return movieFail(movie, "Ogg BOS is neither Theora nor Vorbis");
}

static bool
movieReadHeaders(MovieDecoder *movie)
{
	while(!movie->videoStreamReady || !movie->audioStreamReady){
		ogg_page page;
		int result = movieNextPage(movie, &page);
		if(result <= 0)
			return movieFail(movie, "missing Theora/Vorbis BOS pages");
		if(!ogg_page_bos(&page))
			return movieFail(movie, "data before both Ogg streams");
		if(!movieIdentifyBos(movie, &page))
			return false;
	}

	while(movie->videoHeaders < 3 || movie->audioHeaders < 3){
		ogg_page page;
		int result = movieNextPage(movie, &page);
		if(result <= 0)
			return movieFail(movie, "truncated codec headers");
		if(ogg_page_bos(&page))
			return movieFail(movie, "too many Ogg streams");
		if(!movieRoutePage(movie, &page))
			return false;

		while(movie->videoHeaders < 3){
			ogg_packet packet;
			result = ogg_stream_packetout(&movie->videoStream, &packet);
			if(result == 0)
				break;
			if(result < 0)
				return movieFail(movie, "hole in Theora headers");
			if(th_decode_headerin(&movie->videoInfo, &movie->videoComment,
			    &movie->videoSetup, &packet) <= 0)
				return movieFail(movie, "invalid Theora headers");
			movie->videoHeaders++;
		}

		while(movie->audioHeaders < 3){
			ogg_packet packet;
			result = ogg_stream_packetout(&movie->audioStream, &packet);
			if(result == 0)
				break;
			if(result < 0)
				return movieFail(movie, "hole in Vorbis headers");
			if(vorbis_synthesis_headerin(&movie->audioInfo,
			    &movie->audioComment, &packet) != 0)
				return movieFail(movie, "invalid Vorbis headers");
			movie->audioHeaders++;
		}
	}
	return true;
}

static bool
movieDecoderOpen(MovieDecoder *movie, const char *path)
{
	movieDecoderInit(movie);
	movie->file = fopen(path, "rb");
	if(movie->file == nil)
		return movieFail(movie, "movie file absent");
	if(ogg_sync_init(&movie->sync) != 0)
		return movieFail(movie, "Ogg sync allocation failed");
	movie->syncReady = true;
	if(!movieReadHeaders(movie))
		return false;

	const th_info &info = movie->videoInfo;
	if(info.frame_width != 640 || info.frame_height != 480 ||
	   info.pic_width != 640 || info.pic_height != 480 ||
	   info.pic_x != 0 || info.pic_y != 0 || info.pixel_fmt != TH_PF_420 ||
	   info.fps_numerator != 25 || info.fps_denominator != 1)
		return movieFail(movie, "need uncropped 640x480 4:2:0 Theora at 25fps");
	if(movie->audioInfo.rate != 44100 || movie->audioInfo.channels != 2)
		return movieFail(movie, "need stereo 44.1kHz Vorbis");

	movie->videoDecoder = th_decode_alloc(&movie->videoInfo, movie->videoSetup);
	if(movie->videoDecoder == nil)
		return movieFail(movie, "Theora decoder allocation failed");
	th_setup_free(movie->videoSetup);
	movie->videoSetup = nil;
	if(vorbis_synthesis_init(&movie->audioDsp, &movie->audioInfo) != 0)
		return movieFail(movie, "Vorbis decoder allocation failed");
	movie->audioDspReady = true;
	if(vorbis_block_init(&movie->audioDsp, &movie->audioBlock) != 0)
		return movieFail(movie, "Vorbis block allocation failed");
	movie->audioBlockReady = true;
	movie->frameStep = 1.0 / 25.0;

	char line[176];
	snprintf(line, sizeof(line),
	    "FMV codecs Theora=%s bitstream=%u.%u.%u Vorbis=44.1k/stereo",
	    th_version_string(), info.version_major, info.version_minor,
	    info.version_subminor);
	movieTrace(line);
	return true;
}

static int
movieDecodeVideo(MovieDecoder *movie)
{
	ogg_packet packet;
	u64 started = gettime();
	int packetResult = movieNextPacket(movie, &movie->videoStream, &packet);
	if(packetResult != PACKET_OK){
		movie->frameReady = false;
		return packetResult;
	}

	ogg_int64_t granule = -1;
	int result = th_decode_packetin(movie->videoDecoder, &packet, &granule);
	if(result != 0 && result != TH_DUPFRAME){
		movieFail(movie, "Theora video packet rejected");
		movie->frameReady = false;
		return PACKET_ERROR;
	}
	if(result == 0){
		if(th_decode_ycbcr_out(movie->videoDecoder, movie->frame) != 0){
			movieFail(movie, "Theora frame unavailable");
			movie->frameReady = false;
			return PACKET_ERROR;
		}
		movie->frameChanged = true;
	}else{
		if(movie->frameNumber == 0){
			movieFail(movie, "first Theora frame is duplicate");
			movie->frameReady = false;
			return PACKET_ERROR;
		}
		movie->frameChanged = false;
	}
	movie->frameTime = movie->frameNumber * movie->frameStep;
	movie->frameNumber++;
	movie->frameReady = true;
	uint32 elapsed = (uint32)ticks_to_microsecs(gettime() - started);
	movie->decodeTotalUs += elapsed;
	if(elapsed > movie->decodeMaxUs)
		movie->decodeMaxUs = elapsed;
	return PACKET_OK;
}

struct MovieAudio {
	AESNDPB *voice;
	uint8 *buffer[2];
	int fill;
	int play;
	uint32 fillFrames;
	volatile bool ready;
	volatile bool eof;
	volatile bool started;
	volatile bool stopped;
	volatile uint32 callbacks;
	volatile uint32 buffersSubmitted;
	volatile uint32 starved;
};

static void
movieAudioCallback(AESNDPB *voice, u32 state, void *arg)
{
	MovieAudio *audio = (MovieAudio*)arg;
	if(state == VOICE_STATE_STOPPED){
		audio->stopped = true;
		return;
	}
	if(state != VOICE_STATE_STREAM)
		return;
	audio->callbacks++;
	if(audio->ready){
		AESND_SetVoiceBuffer(voice, audio->buffer[audio->play], MOVIE_AUDIO_BYTES);
		audio->play ^= 1;
		audio->ready = false;
		audio->buffersSubmitted++;
	}else{
		// Never replay the previous DSP block. EOF is normal; missing a buffer
		// before EOF is fatal and the main loop will return false.
		if(!audio->eof)
			audio->starved++;
		AESND_SetVoiceStop(voice, true);
	}
}

static bool
movieAudioCreate(MovieAudio *audio)
{
	memset(audio, 0, sizeof(*audio));
	for(int i = 0; i < 2; i++){
		audio->buffer[i] = (uint8*)memalign(32, MOVIE_AUDIO_BYTES);
		if(audio->buffer[i] == nil)
			return false;
		memset(audio->buffer[i], 0, MOVIE_AUDIO_BYTES);
	}
	audio->voice = AESND_AllocateVoiceWithArg(movieAudioCallback, audio);
	return audio->voice != nil;
}

static void
movieAudioDestroy(MovieAudio *audio)
{
	if(audio->voice){
		if(audio->started){
			audio->stopped = false;
			AESND_SetVoiceStop(audio->voice, true);
			for(int frame = 0; frame < 4 && !audio->stopped; frame++)
				VIDEO_WaitVSync();
		}
		AESND_RegisterVoiceCallbackWithArg(audio->voice, nil, nil);
		AESND_FreeVoice(audio->voice);
	}
	free(audio->buffer[0]);
	free(audio->buffer[1]);
	memset(audio, 0, sizeof(*audio));
}

static int16
moviePcm16(ogg_int32_t value)
{
	// Tremor exposes signed 24-bit fixed-point PCM through pcmout().
	value >>= 9;
	if(value > 32767)
		value = 32767;
	else if(value < -32768)
		value = -32768;
	return (int16)value;
}

static void
movieAudioCommit(MovieAudio *audio)
{
	DCFlushRange(audio->buffer[audio->fill], MOVIE_AUDIO_BYTES);
	audio->fill ^= 1;
	audio->fillFrames = 0;
	audio->ready = true;
}

static bool
movieAudioFill(MovieDecoder *movie, MovieAudio *audio, bool prime)
{
	uint32 produced = 0;
	while(!audio->ready && !audio->eof){
		ogg_int32_t **pcm = nil;
		int available = vorbis_synthesis_pcmout(&movie->audioDsp, &pcm);
		if(available < 0)
			return movieFail(movie, "Vorbis PCM state corrupt");
		if(available > 0){
			uint32 room = MOVIE_AUDIO_FRAMES - audio->fillFrames;
			uint32 take = (uint32)available < room ? (uint32)available : room;
			if(!prime){
				uint32 sliceRoom = MOVIE_AUDIO_SLICE_FRAMES - produced;
				if(take > sliceRoom)
					take = sliceRoom;
			}
			int16 *base = (int16*)audio->buffer[audio->fill] +
			    audio->fillFrames * 2;
			// Tremor keeps channels planar; channel-first copies walk both source
			// planes linearly and match ov_read's cache-friendly conversion.
			for(int channel = 0; channel < 2; channel++){
				int16 *dst = base + channel;
				for(uint32 i = 0; i < take; i++, dst += 2)
					*dst = moviePcm16(pcm[channel][i]);
			}
			if(vorbis_synthesis_read(&movie->audioDsp, (int)take) != 0)
				return movieFail(movie, "Vorbis PCM consume failed");
			audio->fillFrames += take;
			produced += take;
			if(audio->fillFrames == MOVIE_AUDIO_FRAMES)
				movieAudioCommit(audio);
			if(!prime && produced == MOVIE_AUDIO_SLICE_FRAMES)
				break;
			continue;
		}

		ogg_packet packet;
		int result = movieNextPacket(movie, &movie->audioStream, &packet);
		if(result == PACKET_ERROR)
			return false;
		if(result == PACKET_END){
			if(audio->fillFrames){
				uint32 used = audio->fillFrames * 2 * sizeof(int16);
				memset(audio->buffer[audio->fill] + used, 0,
				    MOVIE_AUDIO_BYTES - used);
				movieAudioCommit(audio);
			}
			audio->eof = true;
			break;
		}
		if(vorbis_synthesis(&movie->audioBlock, &packet) != 0)
			return movieFail(movie, "Vorbis audio packet rejected");
		if(vorbis_synthesis_blockin(&movie->audioDsp,
		    &movie->audioBlock) != 0)
			return movieFail(movie, "Vorbis audio block rejected");
	}
	return true;
}

// Same fill, timed. Video decode is accounted for; without the audio side
// accounted too, a loop that is busy cannot be told apart from one that is idle.
static bool
movieAudioService(MovieDecoder *movie, MovieAudio *audio, u64 *totalUs)
{
	u64 started = gettime();
	bool ok = movieAudioFill(movie, audio, false);
	*totalUs += ticks_to_microsecs(gettime() - started);
	return ok;
}

static bool
movieAudioStart(MovieAudio *audio)
{
	if(!audio->ready)
		return false;
	AESND_SetVoiceFormat(audio->voice, VOICE_STEREO16);
	AESND_SetVoiceFrequency(audio->voice, 44100.0f);
	AESND_SetVoiceVolume(audio->voice, 255, 255);
	AESND_SetVoiceStream(audio->voice, true);
	audio->started = true;
	audio->buffersSubmitted = 1;
	AESND_SetVoiceBuffer(audio->voice, audio->buffer[audio->play],
	    MOVIE_AUDIO_BYTES);
	audio->play ^= 1;
	audio->ready = false;
	AESND_SetVoiceStop(audio->voice, false);
	return true;
}

static uint32
movieHashBytes(const uint8 *bytes, uint32 size)
{
	uint32 hash = 2166136261u;
	for(uint32 i = 0; i < size; i++){
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

static void
movieFrameToXfb(const MovieDecoder *movie, void *xfb,
    uint32 outputWidth, uint32 outputHeight)
{
	const th_img_plane &yPlane = movie->frame[0];
	const th_img_plane &cbPlane = movie->frame[1];
	const th_img_plane &crPlane = movie->frame[2];
	uint32 pictureWidth = movie->videoInfo.pic_width;
	uint32 pictureHeight = movie->videoInfo.pic_height;
	uint32 top = (outputHeight - pictureHeight) / 2;
	uint32 left = (outputWidth - pictureWidth) / 2;
	uint32 pictureX = movie->videoInfo.pic_x;
	uint32 pictureY = movie->videoInfo.pic_y;
	for(uint32 y = 0; y < pictureHeight; y++){
		uint32 *dst = (uint32*)xfb + (top + y) * (outputWidth / 2) + left / 2;
		const uint8 *luma = yPlane.data + (pictureY + y) * yPlane.stride + pictureX;
		const uint8 *cb = cbPlane.data + ((pictureY + y) >> 1) * cbPlane.stride +
		    (pictureX >> 1);
		const uint8 *cr = crPlane.data + ((pictureY + y) >> 1) * crPlane.stride +
		    (pictureX >> 1);
		for(uint32 x = 0; x < pictureWidth; x += 2)
			*dst++ = (uint32)luma[x] << 24 | (uint32)cb[x >> 1] << 16 |
			         (uint32)luma[x + 1] << 8 | cr[x >> 1];
	}
}

// The two XFBs the boot console already owns, used as a present queue instead
// of a plain flip pair. With exactly one frame in flight, every Theora spike (a
// keyframe costs ~4x an inter frame) landed straight on a present deadline
// while the loop idled ~20ms after each frame doing nothing, and the lateness
// accumulated until a frame had to be dropped. Converting into the free buffer
// as soon as the frame exists gives a whole 40ms slot to absorb the spike: one
// buffer is live, the other already holds the next picture, and libtheora holds
// one more decoded frame behind that. More slots would give more slack, but the
// boot heap has no 600KiB to spare — FONTS.TXD fails to allocate right after
// the movie, so this queue allocates nothing at all.
enum { MOVIE_RING_FRAMES = 2 };

struct MovieRing {
	void *frame[MOVIE_RING_FRAMES];
	double time[MOVIE_RING_FRAMES];
	void *submitted;
	uint32 size;
	uint32 head;
	uint32 count;
};

static void
movieRingCreate(MovieRing *ring, void *xfbA, void *xfbB)
{
	memset(ring, 0, sizeof(*ring));
	// First conversion must target the XFB not currently scanned out. This is
	// normally xfbB after GX adopts the boot console buffer as xfbA.
	if(xfbA == VIDEO_GetCurrentFramebuffer()){
		ring->frame[0] = xfbB;
		ring->frame[1] = xfbA;
	}else{
		ring->frame[0] = xfbA;
		ring->frame[1] = xfbB;
	}
	ring->size = MOVIE_RING_FRAMES;
}

// Returns false when there is nowhere to decode ahead into, which is the normal
// way the loop learns it is far enough ahead and can go back to waiting.
static bool
movieRingPush(MovieRing *ring, const MovieDecoder *movie, uint32 width,
    uint32 height)
{
	if(ring->count >= ring->size)
		return false;
	uint32 index = (ring->head + ring->count) % ring->size;
	void *slot = ring->frame[index];
	// Never draw into the buffer VI is scanning out, nor into the one it is
	// about to latch at the next retrace.
	if(slot == ring->submitted || slot == VIDEO_GetCurrentFramebuffer())
		return false;
	movieFrameToXfb(movie, slot, width, height);
	ring->time[index] = movie->frameTime;
	ring->count++;
	return true;
}

static void *
movieRingPop(MovieRing *ring)
{
	void *frame = ring->frame[ring->head];
	ring->head = (ring->head + 1) % ring->size;
	ring->count--;
	return frame;
}

static void
movieTraceFirstFrame(const MovieDecoder *movie, void *xfb,
    uint32 framebufferBytes, void *otherXfb)
{
	char line[208];
	const th_img_plane &y = movie->frame[0];
	const th_img_plane &cb = movie->frame[1];
	const th_img_plane &cr = movie->frame[2];
	snprintf(line, sizeof(line),
	    "FMV frame0 Y=%dx%d/%d Cb=%dx%d/%d Cr=%dx%d/%d",
	    y.width, y.height, y.stride, cb.width, cb.height, cb.stride,
	    cr.width, cr.height, cr.stride);
	movieTrace(line);
	const uint8 *head = (const uint8*)xfb;
	snprintf(line, sizeof(line),
	    "FMV XFB boot=%p other=%p bytes=%u head=%02X%02X%02X%02X crc=%08X",
	    xfb, otherXfb, (unsigned)framebufferBytes, head[0], head[1], head[2],
	    head[3], (unsigned)movieHashBytes(head, framebufferBytes));
	movieTrace(line);
}

static bool
movieSkipPressed(void)
{
	PAD_ScanPads();
	for(int pad = 0; pad < PAD_CHANMAX; pad++)
		if(PAD_ButtonsDown(pad))
			return true;
	return false;
}

static void
movieDrainSkipButton(void)
{
	int cleanFrames = 0;
	for(int frame = 0; frame < 120 && cleanFrames < 2; frame++){
		PAD_ScanPads();
		bool held = false;
		for(int pad = 0; pad < PAD_CHANMAX; pad++)
			held |= PAD_ButtonsHeld(pad) != 0;
		cleanFrames = held ? 0 : cleanFrames + 1;
		VIDEO_WaitVSync();
	}
}

} // namespace

bool
PlayGameCubeMovie(const char *path, void *bootFramebuffer,
    unsigned width, unsigned height, unsigned framebufferBytes)
{
	if(path == nil || bootFramebuffer == nil || width != 640 || height < 480 ||
	   framebufferBytes < width * height * VI_DISPLAY_PIX_SZ)
		return false;

	void *secondFramebuffer = gxMovieXfb;
	if(secondFramebuffer == nil)
		secondFramebuffer = VIDEO_GetCurrentFramebuffer();
	if(secondFramebuffer == nil || secondFramebuffer == bootFramebuffer)
		return false;
	VIDEO_SetBlack(TRUE);
	VIDEO_Flush();
	VIDEO_WaitVSync();

	FsGuard fs;
	struct mallinfo before = mallinfo();
	char line[320];
	snprintf(line, sizeof(line),
	    "FMV preflight heap used=%uK free=%uK contiguous=%uK",
	    (unsigned)before.uordblks / 1024, (unsigned)before.fordblks / 1024,
	    (unsigned)MOVIE_DECODER_PREFLIGHT_BYTES / 1024);
	movieTrace(line);
	void *preflight = memalign(32, MOVIE_DECODER_PREFLIGHT_BYTES);
	if(preflight == nil){
		movieTrace("FMV preflight contiguous allocation failed");
		return false;
	}
	free(preflight);

	GXRModeObj *outputMode = VIDEO_GetPreferredMode(NULL);
	if(outputMode == nil || outputMode->fbWidth != width ||
	   outputMode->xfbHeight != height)
		return false;
	VIDEO_ClearFrameBuffer(outputMode, bootFramebuffer, COLOR_BLACK);

	MovieDecoder movie;
	if(!movieDecoderOpen(&movie, path)){
		movieDecoderClose(&movie);
		return false;
	}
	MovieAudio audio;
	memset(&audio, 0, sizeof(audio));
	if(!movieAudioCreate(&audio)){
		movieAudioDestroy(&audio);
		movieDecoderClose(&movie);
		movieTrace("FMV audio allocation failed");
		return false;
	}
	MovieRing ring;
	movieRingCreate(&ring, bootFramebuffer, secondFramebuffer);
	// Both XFBs are queue slots from here on, so clear the console off them
	// before the first frame is converted into one.
	VIDEO_ClearFrameBuffer(outputMode, bootFramebuffer, COLOR_BLACK);
	VIDEO_ClearFrameBuffer(outputMode, secondFramebuffer, COLOR_BLACK);
	if(!movieAudioFill(&movie, &audio, true) || !audio.ready ||
	   movieDecodeVideo(&movie) != PACKET_OK ||
	   !movieRingPush(&ring, &movie, width, height)){
		movieAudioDestroy(&audio);
		movieDecoderClose(&movie);
		movieTrace("FMV empty video/audio");
		return false;
	}

	snprintf(line, sizeof(line), "FMV start %s", path);
	movieTrace(line);
	void *firstFrame = movieRingPop(&ring);
	ring.submitted = firstFrame;
	movieTraceFirstFrame(&movie, firstFrame, framebufferBytes,
	    ring.frame[1]);
	VIDEO_SetNextFramebuffer(firstFrame);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();

	int nextResult = movieDecodeVideo(&movie);
	if(nextResult == PACKET_ERROR || !movieAudioStart(&audio)){
		movieAudioDestroy(&audio);
		movieDecoderClose(&movie);
		movieTrace("FMV startup failed");
		return false;
	}
	// Audio starts the master clock. Refilling the second 341ms buffer can do
	// disc I/O, so timestamp before that work or video begins artificially late.
	u64 started = gettime();
	if(!movieAudioFill(&movie, &audio, true)){
		movieAudioDestroy(&audio);
		movieDecoderClose(&movie);
		return false;
	}

	uint32 shown = 1;
	uint32 dropped = 0;
	uint32 yuvMaxUs = 0;
	uint32 ringMin = ring.size;
	uint32 passes = 0;
	u64 audioTotalUs = 0;
	// SetNextFramebuffer takes effect on the following retrace. Queue one 60Hz
	// field ahead instead of waiting until the 25fps timestamp has already
	// passed and then missing another 16.7ms window.
	const double presentLead = 1.0 / 60.0;
	bool skipped = false;
	bool audioFailed = false;
	bool videoFailed = false;

	for(;;){
		passes++;
		if(audio.starved){
			audioFailed = true;
			movieTrace("FMV audio underrun");
			break;
		}
		if(movieSkipPressed()){
			skipped = true;
			break;
		}
		// Audio owns the hard deadline and has no queue of its own; video now
		// has the ring, so fill audio first and again between decodes.
		if(!movieAudioService(&movie, &audio, &audioTotalUs)){
			audioFailed = true;
			break;
		}

		// Decode ahead into every free slot instead of idling until the next
		// retrace. A duplicate frame gets no slot at all: leaving the previous
		// buffer on screen is exactly what TH_DUPFRAME asks for, and it skips
		// a 600KiB conversion.
		while(nextResult == PACKET_OK && movie.frameReady){
			if(movie.frameChanged){
				u64 convertStarted = gettime();
				bool pushed = movieRingPush(&ring, &movie, width, height);
				uint32 convertUs = (uint32)ticks_to_microsecs(
				    gettime() - convertStarted);
				if(!pushed)
					break;
				if(convertUs > yuvMaxUs)
					yuvMaxUs = convertUs;
			}
			nextResult = movieDecodeVideo(&movie);
			if(nextResult == PACKET_ERROR){
				videoFailed = true;
				break;
			}
			if(!movieAudioService(&movie, &audio, &audioTotalUs)){
				audioFailed = true;
				break;
			}
		}
		if(audioFailed || videoFailed)
			break;

		double elapsed = (double)ticks_to_microsecs(gettime() - started) /
		    1000000.0;
		// Depth left after decoding ahead as far as it could. Staying above
		// zero while packets remain is what proves the decoder keeps its lead.
		if(nextResult == PACKET_OK && ring.count < ringMin)
			ringMin = ring.count;
		while(ring.count){
			if(ring.time[ring.head] > elapsed + presentLead)
				break;
			void *ready = movieRingPop(&ring);
			// A newer frame is due as well, so this one's slot on screen has
			// already passed. Its length is the gap to the next entry, not one
			// frame step: duplicates hold the previous picture for longer.
			if(ring.count && ring.time[ring.head] <= elapsed + presentLead){
				dropped++;
				continue;
			}
			shown++;
			ring.submitted = ready;
			VIDEO_SetNextFramebuffer(ready);
			VIDEO_Flush();
			break;
		}

		if(nextResult == PACKET_END && ring.count == 0 && audio.eof &&
		   elapsed >= movie.frameNumber * movie.frameStep)
			break;
		VIDEO_WaitVSync();
	}

	uint32 decoded = movie.frameNumber;
	uint32 decodeMaxUs = movie.decodeMaxUs;
	u64 decodeTotalUs = movie.decodeTotalUs;
	uint32 starved = audio.starved;
	uint32 audioCallbacks = audio.callbacks;
	uint32 audioBuffers = audio.buffersSubmitted;
	bool audioStarted = audio.started;
	bool audioEof = audio.eof;
	movieAudioDestroy(&audio);
	movieDecoderClose(&movie);

	// Leave VI on a cleared non-console XFB. The following splash owns normal
	// rendering; no Gecko/stdio diagnostics become visible between them.
	VIDEO_SetNextFramebuffer(bootFramebuffer);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	for(int i = 0; i < 3 && VIDEO_GetCurrentFramebuffer() != bootFramebuffer; i++)
		VIDEO_WaitVSync();
	if(VIDEO_GetCurrentFramebuffer() != bootFramebuffer){
		movieTrace("FMV XFB handoff failed");
		return false;
	}
	VIDEO_ClearFrameBuffer(outputMode, secondFramebuffer, COLOR_BLACK);
	VIDEO_SetNextFramebuffer(secondFramebuffer);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	for(int i = 0; i < 3 && VIDEO_GetCurrentFramebuffer() != secondFramebuffer; i++)
		VIDEO_WaitVSync();
	if(VIDEO_GetCurrentFramebuffer() != secondFramebuffer){
		movieTrace("FMV XFB hide-console failed");
		return false;
	}
	if(skipped)
		movieDrainSkipButton();

	struct mallinfo after = mallinfo();
	uint32 decodeAverageUs = decoded ? (uint32)(decodeTotalUs / decoded) : 0;
	snprintf(line, sizeof(line),
	    "FMV end skip=%d frames=%u shown=%u drop=%u ringMin=%u pass=%u audioTotal=%lluus audio=%d cb=%u buf=%u starve=%u decMax=%uus decAvg=%uus decTotal=%lluus yuvMax=%uus heap=%dK",
	    skipped, (unsigned)decoded, (unsigned)shown, (unsigned)dropped,
	    (unsigned)ringMin, (unsigned)passes,
	    (unsigned long long)audioTotalUs,
	    (int)audioStarted, (unsigned)audioCallbacks, (unsigned)audioBuffers,
	    (unsigned)starved, (unsigned)decodeMaxUs, (unsigned)decodeAverageUs,
	    (unsigned long long)decodeTotalUs, (unsigned)yuvMaxUs,
	    (int)((int32)after.fordblks - (int32)before.fordblks) / 1024);
	movieTrace(line);
	bool audioHealthy = audioStarted && audioBuffers != 0 && !audioFailed &&
	                    starved == 0 &&
	                    (skipped || (audioCallbacks != 0 && audioEof));
	return audioHealthy && !videoFailed;
}

#endif // GTA_OGC
