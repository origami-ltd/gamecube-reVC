#pragma warning( push )
#pragma warning( disable : 4005)
#pragma warning( pop )
#define FORCE_PC_SCALING
#ifndef LIBRW
#define WITHD3D
#endif
#include "common.h"

#include <new>
#ifdef ANISOTROPIC_FILTERING
#include "rpanisot.h"
#endif
#include "crossplatform.h"
#include "platform.h"

#include "Timer.h"
#ifdef GTA_PC
#include "FileMgr.h"
#include "Pad.h"
#include "main.h"
#include "Directory.h"
#include "Streaming.h"
#include "TxdStore.h"
#include "CdStream.h"
#include "Font.h"
#include "Sprite2d.h"
#include "Text.h"
#include "RwHelper.h"
#include "Frontend.h"
#endif //GTA_PC

float texLoadTime;
int32 texNumLoaded;

#ifdef LIBRW
#define READNATIVE(stream, tex, size) rwNativeTextureHackRead(stream, tex, size)
#else
#define READNATIVE(stream, tex, size) RWSRCGLOBAL(stdFunc[rwSTANDARDNATIVETEXTUREREAD](stream, tex, size))
#endif

RwTexture*
RwTextureGtaStreamRead(RwStream *stream, uint32 end)
{
	RwUInt32 size, version;
	RwTexture *tex = nil;

	uint32 position = STREAMPOS(stream);
	if(position == UINT32_MAX || position > end || end - position < 12)
		return nil;
	if(!RwStreamFindChunk(stream, rwID_TEXTURENATIVE, &size, &version))
		return nil;
	uint32 nativeStart = STREAMPOS(stream);
	if(size > INT32_MAX || nativeStart == UINT32_MAX ||
	   nativeStart > end || size > end - nativeStart)
		return nil;

#ifdef GTA_PC
	float preloadTime = (float)CTimer::GetCurrentTimeInCycles() / (float)CTimer::GetCyclesPerMillisecond();
#endif

	if(!READNATIVE(stream, &tex, size) || tex == nil)
		return nil;
	if(STREAMPOS(stream) != nativeStart + size){
		RwTextureDestroy(tex);
		return nil;
	}

#ifdef GTA_PC
	if (gGameState == GS_INIT_PLAYING_GAME) {
		texLoadTime = (texNumLoaded * texLoadTime + (float)CTimer::GetCurrentTimeInCycles() / (float)CTimer::GetCyclesPerMillisecond() - preloadTime) / (float)(texNumLoaded+1);
		texNumLoaded++;
	}
#endif

#ifdef ANISOTROPIC_FILTERING
	if(tex && RpAnisotGetMaxSupportedMaxAnisotropy() > 1)	// BUG? this was RpAnisotTextureGetMaxAnisotropy, but that doesn't make much sense
		RpAnisotTextureSetMaxAnisotropy(tex, RpAnisotGetMaxSupportedMaxAnisotropy());
#endif

	return tex;
}

RwTexture*
destroyTexture(RwTexture *texture, void *data)
{
	RwTextureDestroy(texture);
	return texture;
}

RwTexDictionary*
RwTexDictionaryGtaStreamRead(RwStream *stream, uint32 dictionarySize)
{
	RwUInt32 size, version;
	RwInt32 numTextures;
	RwTexDictionary *texDict;
	RwTexture *tex;
	uint32 position = STREAMPOS(stream);
	uint32 end;

	if(position == UINT32_MAX || !GetRwStreamEnd(stream, dictionarySize, end) ||
	   end - position < 12)
		return nil;
	if(!RwStreamFindChunk(stream, rwID_STRUCT, &size, &version))
		return nil;
	uint32 structStart = STREAMPOS(stream);
	if(structStart == UINT32_MAX || structStart > end || size > end - structStart ||
	   size != sizeof(numTextures) || !ReadStreamLE32(stream, numTextures))
		return nil;
	if(numTextures < 0 || numTextures > INT16_MAX)
		return nil;

	texDict = RwTexDictionaryCreate();
	if(texDict == nil)
		return nil;

	while(numTextures--){
		tex = RwTextureGtaStreamRead(stream, end);
		if(tex == nil){
			RwTexDictionaryForAllTextures(texDict, destroyTexture, nil);
			RwTexDictionaryDestroy(texDict);
			return nil;
		}
		RwTexDictionaryAddTexture(texDict, tex);
	}
	position = STREAMPOS(stream);
	if(position == UINT32_MAX || position > end){
		RwTexDictionaryForAllTextures(texDict, destroyTexture, nil);
		RwTexDictionaryDestroy(texDict);
		return nil;
	}

	return texDict;
}

static int32 numberTextures = -1;
static uint32 streamPosition;
static uint32 textureDictionaryEnd;

RwTexDictionary*
RwTexDictionaryGtaStreamRead1(RwStream *stream, uint32 dictionarySize)
{
	RwUInt32 size, version;
	RwInt32 numTextures;
	RwTexDictionary *texDict;
	RwTexture *tex;
	uint32 position = STREAMPOS(stream);

	numberTextures = -1;
	if(position == UINT32_MAX ||
	   !GetRwStreamEnd(stream, dictionarySize, textureDictionaryEnd) ||
	   textureDictionaryEnd - position < 12)
		return nil;
	if(!RwStreamFindChunk(stream, rwID_STRUCT, &size, &version))
		return nil;
	uint32 structStart = STREAMPOS(stream);
	if(structStart == UINT32_MAX || structStart > textureDictionaryEnd ||
	   size > textureDictionaryEnd - structStart || size != sizeof(numTextures) ||
	   !ReadStreamLE32(stream, numTextures))
		return nil;
	if(numTextures < 0 || numTextures > INT16_MAX)
		return nil;

	texDict = RwTexDictionaryCreate();
	if(texDict == nil)
		return nil;

	numberTextures = numTextures/2;

	while(numTextures > numberTextures){
		numTextures--;

		tex = RwTextureGtaStreamRead(stream, textureDictionaryEnd);
		if(tex == nil){
			RwTexDictionaryForAllTextures(texDict, destroyTexture, nil);
			RwTexDictionaryDestroy(texDict);
			return nil;
		}
		RwTexDictionaryAddTexture(texDict, tex);
	}

	numberTextures = numTextures;
	streamPosition = STREAMPOS(stream);
	if(streamPosition == UINT32_MAX){
		RwTexDictionaryForAllTextures(texDict, destroyTexture, nil);
		RwTexDictionaryDestroy(texDict);
		numberTextures = -1;
		return nil;
	}

	return texDict;
}

RwTexDictionary*
RwTexDictionaryGtaStreamRead2(RwStream *stream, RwTexDictionary *texDict)
{
	RwTexture *tex;
	uint32 position = STREAMPOS(stream);

	if(texDict == nil){
		numberTextures = -1;
		return nil;
	}
	if(numberTextures < 0 || position == UINT32_MAX || position > streamPosition)
		goto fail;
	RwStreamSkip(stream, streamPosition - position);
	if(STREAMPOS(stream) != streamPosition)
		goto fail;

	while(numberTextures > 0){
		tex = RwTextureGtaStreamRead(stream, textureDictionaryEnd);
		if(tex == nil)
			goto fail;
		RwTexDictionaryAddTexture(texDict, tex);
		numberTextures--;
	}
	position = STREAMPOS(stream);
	if(position == UINT32_MAX || position > textureDictionaryEnd)
		goto fail;
	numberTextures = -1;

	return texDict;

fail:
	numberTextures = -1;
	RwTexDictionaryForAllTextures(texDict, destroyTexture, nil);
	RwTexDictionaryDestroy(texDict);
	return nil;
}

#ifdef GTA_PC

#ifdef LIBRW

#define CAPSVERSION 0

struct GPUcaps
{
	uint32 version;	// so we can force regeneration easily
	uint32 platform;
	uint32 subplatform;
	uint32 dxtSupport;
};

static void
GetGPUcaps(GPUcaps *caps)
{
	caps->version = CAPSVERSION;
	caps->platform = rw::platform;
	caps->subplatform = 0;
	caps->dxtSupport = 0;
	// TODO: more later
#ifdef RW_GL3
	caps->subplatform = rw::gl3::gl3Caps.gles;
	caps->dxtSupport = rw::gl3::gl3Caps.dxtSupported;
#endif
#ifdef RW_D3D9
	caps->dxtSupport = 1;	// TODO, probably
#endif
}

void
ReadVideoCardCapsFile(GPUcaps *caps)
{
	memset(caps, 0, sizeof(GPUcaps));

	int32 file = CFileMgr::OpenFile("DATA\\CAPS.DAT", "rb");
	if (file != 0) {
		CFileMgr::Read(file, (char*)&caps->version, 4);
		CFileMgr::Read(file, (char*)&caps->platform, 4);
		CFileMgr::Read(file, (char*)&caps->subplatform, 4);
		CFileMgr::Read(file, (char*)&caps->dxtSupport, 4);
		CFileMgr::CloseFile(file);
	}
}

bool
CheckVideoCardCaps(void)
{
	GPUcaps caps, fcaps;
	GetGPUcaps(&caps);
	ReadVideoCardCapsFile(&fcaps);
	return caps.version != fcaps.version ||
		caps.platform != fcaps.platform ||
		caps.subplatform != fcaps.subplatform ||
		caps.dxtSupport != fcaps.dxtSupport;
}

void
WriteVideoCardCapsFile(void)
{
	GPUcaps caps;
	GetGPUcaps(&caps);
	int32 file = CFileMgr::OpenFile("DATA\\CAPS.DAT", "wb");
	if (file != 0) {
		CFileMgr::Write(file, (char*)&caps.version, 4);
		CFileMgr::Write(file, (char*)&caps.platform, 4);
		CFileMgr::Write(file, (char*)&caps.subplatform, 4);
		CFileMgr::Write(file, (char*)&caps.dxtSupport, 4);
		CFileMgr::CloseFile(file);
	}
}


#else
extern "C" RwInt32 _rwD3D8FindCorrectRasterFormat(RwRasterType type, RwInt32 flags);
extern "C" RwBool   _rwD3D8CheckValidTextureFormat(RwInt32 format);
void
ReadVideoCardCapsFile(uint32 &cap32, uint32 &cap24, uint32 &cap16, uint32 &cap8)
{
	cap32 = UINT32_MAX;
	cap24 = UINT32_MAX;
	cap16 = UINT32_MAX;
	cap8 = UINT32_MAX;

	int32 file = CFileMgr::OpenFile("DATA\\CAPS.DAT", "rb");
	if (file != 0) {
		CFileMgr::Read(file, (char*)&cap32, 4);
		CFileMgr::Read(file, (char*)&cap24, 4);
		CFileMgr::Read(file, (char*)&cap16, 4);
		CFileMgr::Read(file, (char*)&cap8, 4);
		CFileMgr::CloseFile(file);
	}
}

bool
CheckVideoCardCaps(void)
{
	uint32 cap32 = _rwD3D8FindCorrectRasterFormat(rwRASTERTYPETEXTURE, rwRASTERFORMAT8888);
	uint32 cap24 = _rwD3D8FindCorrectRasterFormat(rwRASTERTYPETEXTURE, rwRASTERFORMAT888);
	uint32 cap16 = _rwD3D8FindCorrectRasterFormat(rwRASTERTYPETEXTURE, rwRASTERFORMAT1555);
	uint32 cap8 = _rwD3D8FindCorrectRasterFormat(rwRASTERTYPETEXTURE, rwRASTERFORMATPAL8 | rwRASTERFORMAT8888);
	uint32 fcap32, fcap24, fcap16, fcap8;
	ReadVideoCardCapsFile(fcap32, fcap24, fcap16, fcap8);
	return cap32 != fcap32 || cap24 != fcap24 || cap16 != fcap16 || cap8 != fcap8;
}

void
WriteVideoCardCapsFile(void)
{
	uint32 cap32 = _rwD3D8FindCorrectRasterFormat(rwRASTERTYPETEXTURE, rwRASTERFORMAT8888);
	uint32 cap24 = _rwD3D8FindCorrectRasterFormat(rwRASTERTYPETEXTURE, rwRASTERFORMAT888);
	uint32 cap16 = _rwD3D8FindCorrectRasterFormat(rwRASTERTYPETEXTURE, rwRASTERFORMAT1555);
	uint32 cap8 = _rwD3D8FindCorrectRasterFormat(rwRASTERTYPETEXTURE, rwRASTERFORMATPAL8 | rwRASTERFORMAT8888);
	int32 file = CFileMgr::OpenFile("DATA\\CAPS.DAT", "wb");
	if (file != 0) {
		CFileMgr::Write(file, (char*)&cap32, 4);
		CFileMgr::Write(file, (char*)&cap24, 4);
		CFileMgr::Write(file, (char*)&cap16, 4);
		CFileMgr::Write(file, (char*)&cap8, 4);
		CFileMgr::CloseFile(file);
	}
}
#endif

bool
CanVideoCardDoDXT(void)
{
#ifdef LIBRW
	// TODO
#ifdef RW_OPENGL
	return false;
#else
	return true;
#endif
#else
	return _rwD3D8CheckValidTextureFormat(D3DFMT_DXT1) && _rwD3D8CheckValidTextureFormat(D3DFMT_DXT3);
#endif
}

void
ConvertingTexturesScreen(uint32 num, uint32 count, const char *text)
{
	HandleExit();

	CSprite2d *splash = LoadSplash(nil);
	if (!DoRWStuffStartOfFrame(0, 0, 0, 0, 0, 0, 255))
		return;

	CSprite2d::SetRecipNearClip();
	CSprite2d::InitPerFrame();
	CFont::InitPerFrame();
	DefinedState();

	RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*)rwTEXTUREADDRESSCLAMP);
	splash->Draw(CRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT), CRGBA(255, 255, 255, 255));

	CSprite2d::DrawRect(CRect(SCREEN_SCALE_X(200.0f), SCREEN_SCALE_Y(240.0f), SCREEN_SCALE_FROM_RIGHT(200.0f), SCREEN_SCALE_Y(248.0f)), CRGBA(64, 64, 64, 255));
#ifdef FIX_BUGS
	CSprite2d::DrawRect(CRect(SCREEN_SCALE_X(200.0f), SCREEN_SCALE_Y(240.0f), (SCREEN_SCALE_FROM_RIGHT(200.0f) - SCREEN_SCALE_X(200.0f)) * ((float)num / (float)count) + SCREEN_SCALE_X(200.0f), SCREEN_SCALE_Y(248.0f)), CRGBA(255, 150, 225, 255));
#else
	CSprite2d::DrawRect(CRect(SCREEN_SCALE_X(200.0f), SCREEN_SCALE_Y(240.0f), (SCREEN_SCALE_FROM_RIGHT(200.0f) - SCREEN_SCALE_X(200.0f)) * ((float)num / (float)count) + SCREEN_SCALE_X(200.0f), SCREEN_SCALE_Y(248.0f)), CRGBA(255, 217, 106, 255));
#endif
	CSprite2d::DrawRect(CRect(SCREEN_SCALE_X(120.0f), SCREEN_SCALE_Y(150.0f), SCREEN_SCALE_FROM_RIGHT(120.0f), SCREEN_HEIGHT - SCREEN_SCALE_Y(220.0f)), CRGBA(50, 50, 50, 210));

	CFont::SetBackgroundOff();
	CFont::SetPropOn();
	CFont::SetScale(SCREEN_SCALE_X(0.45f), SCREEN_SCALE_Y(0.7f));
	CFont::SetCentreOff();
	CFont::SetWrapx(SCREEN_SCALE_FROM_RIGHT(170.0f));
	CFont::SetJustifyOff();
#ifdef FIX_BUGS
	CFont::SetColor(CRGBA(255, 150, 225, 255));
#else
	CFont::SetColor(CRGBA(255, 217, 106, 255));
#endif
	CFont::SetBackGroundOnlyTextOff();
	CFont::SetFontStyle(FONT_STANDARD);
	CFont::PrintString(SCREEN_SCALE_X(170.0f), SCREEN_SCALE_Y(160.0f), TheText.Get(text));
	CFont::DrawFonts();
	DoRWStuffEndOfFrame();
}

void
DealWithTxdWriteError(uint32 num, uint32 count, const char *text)
{
	while (!RsGlobal.quit) {
		ConvertingTexturesScreen(num, count, text);
		CPad::UpdatePads();
		if (CPad::GetPad(0)->GetEscapeJustDown())
			break;
	}
	RsGlobal.quit = false;
	LoadingScreen(nil, nil, nil);
	RsGlobal.quit = true;
}

#ifdef LIBRW
#define STREAMTELL(str)	str->tell()
#else
#define STREAMTELL(str) filesys->rwftell((str)->Type.file.fpFile)
#endif

bool
CreateTxdImageForVideoCard()
{
	uint8 *buf = new(std::nothrow) uint8[CDSTREAM_SECTOR_SIZE];
	CDirectory *pDir = new(std::nothrow) CDirectory(TXDSTORESIZE);
	CDirectory::DirectoryInfo dirInfo;
	if(buf == nil || pDir == nil || !pDir->IsValid()){
		delete []buf;
		delete pDir;
		return false;
	}

	CStreaming::FlushRequestList();

#ifndef LIBRW
	RwFileFunctions *filesys = RwOsGetFileInterface();
#endif

	RwStream *img = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMWRITE, "models\\txd.img");
	if (img == nil) {
		// original code does otherwise and it leaks
		delete []buf;
		delete pDir;

		if (_dwOperatingSystemVersion == OS_WINNT || _dwOperatingSystemVersion == OS_WIN2000 || _dwOperatingSystemVersion == OS_WINXP)
			DealWithTxdWriteError(0, TXDSTORESIZE, "CVT_CRT");

		return false;
	}

#ifdef RW_GL3
	// so we can read back DXT with GLES
	// only works for textures that are not yet loaded
	// so let's hope that is the case for all
	rw::gl3::needToReadBackTextures = true;
#endif
	
#ifdef DISABLE_VSYNC_ON_TEXTURE_CONVERSION
	// let's disable vsync and frame limiter to speed up texture conversion
	// (actually we probably don't need to disable frame limiter in here, but let's do it just in case =P)
	int8 vsyncState = FrontEndMenuManager.m_PrefsVsync;
	int8 frameLimiterState = FrontEndMenuManager.m_PrefsFrameLimiter;
	FrontEndMenuManager.m_PrefsVsync = 0;
	FrontEndMenuManager.m_PrefsFrameLimiter = 0;
#endif

	int32 i;
	for (i = 0; i < TXDSTORESIZE; i++) {
		ConvertingTexturesScreen(i, TXDSTORESIZE, "CVT_MSG");

		if (CTxdStore::GetSlot(i) != nil && CStreaming::IsObjectInCdImage(i + STREAM_OFFSET_TXD)) {
#ifdef FIX_BUGS
			if(strcmp(CTxdStore::GetTxdName(i), "generic") == 0)
				continue;
#endif

			CStreaming::RequestTxd(i, STREAMFLAGS_KEEP_IN_MEMORY);
			CStreaming::RequestModelStream(0);
			CStreaming::FlushChannels();

			char filename[64];
			sprintf(filename, "%s.txd", CTxdStore::GetTxdName(i));

			if (CTxdStore::GetSlot(i)->texDict) {

				int32 pos = STREAMTELL(img);

				if (RwTexDictionaryStreamWrite(CTxdStore::GetSlot(i)->texDict, img) == nil) {
					DealWithTxdWriteError(i, TXDSTORESIZE, "CVT_ERR");
					RwStreamClose(img, nil);
					delete []buf;
					delete pDir;
					CStreaming::RemoveTxd(i);
#ifdef RW_GL3
					rw::gl3::needToReadBackTextures = false;
#endif
					return false;
				}

				int32 size = STREAMTELL(img) - pos;
				int32 num = size % CDSTREAM_SECTOR_SIZE;

				size /= CDSTREAM_SECTOR_SIZE;
				if (num != 0) {
					size++;
					num = CDSTREAM_SECTOR_SIZE - num;
					RwStreamWrite(img, buf, num);
				}

				dirInfo.offset = pos / CDSTREAM_SECTOR_SIZE;
				dirInfo.size = size;
				strncpy(dirInfo.name, filename, sizeof(dirInfo.name));
				if(!pDir->AddItem(dirInfo)){
					RwStreamClose(img, nil);
					delete []buf;
					delete pDir;
					CStreaming::RemoveTxd(i);
					return false;
				}
				CStreaming::RemoveTxd(i);
			}
			CStreaming::FlushRequestList();
		}
	}

#ifdef DISABLE_VSYNC_ON_TEXTURE_CONVERSION
	// restore vsync and frame limiter states
	FrontEndMenuManager.m_PrefsVsync = vsyncState;
	FrontEndMenuManager.m_PrefsFrameLimiter = frameLimiterState;
#endif

	RwStreamClose(img, nil);
	delete []buf;

#ifdef RW_GL3
	rw::gl3::needToReadBackTextures = false;
#endif

	if (!pDir->WriteDirFile("models\\txd.dir")) {
		DealWithTxdWriteError(i, TXDSTORESIZE, "CVT_ERR");
		delete pDir;
		return false;
	}

	delete pDir;

	WriteVideoCardCapsFile();
	return true;
}
#endif // GTA_PC
