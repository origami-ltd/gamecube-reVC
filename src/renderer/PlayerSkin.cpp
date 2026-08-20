#include "common.h"

#include "main.h"
#include "PlayerSkin.h"
#include "TxdStore.h"
#include "rtbmp.h"
#include "ClumpModelInfo.h"
#include "VisibilityPlugins.h"
#include "World.h"
#include "PlayerInfo.h"
#include "CdStream.h"
#include "FileMgr.h"
#include "Directory.h"
#include "RwHelper.h"
#include "Timer.h"
#include "Lights.h"
#include "MemoryMgr.h"

RpClump *gpPlayerClump;
float gOldFov;

int CPlayerSkin::m_txdSlot;

bool
FindPlayerDff(uint32 &offset, uint32 &size)
{
	int file;
	int32 status;
	CDirectory::DirectoryInfo info;

	file = CFileMgr::OpenFile("models\\gta3.dir", "rb");
	if(file <= 0)
		return false;

	do {
		status = CDirectory::ReadEntry(file, info);
		if(status <= 0){
			CFileMgr::CloseFile(file);
			return false;
		}
	} while (strcasecmp("player.dff", info.name) != 0);

	CFileMgr::CloseFile(file);
	offset = info.offset;
	size = info.size;
	return true;
}

bool
LoadPlayerDff(void)
{
	RwStream *stream = nil;
	RwMemory mem;
	uint32 offset, size;
	uint8 *buffer = nil;
	uint64 byteCount;
	bool streamWasAdded = false;
	bool success = false;

	if (CdStreamGetNumImages() == 0) {
		if(!CdStreamAddImage("models\\gta3.img"))
			return false;
		streamWasAdded = true;
	}

	if(!FindPlayerDff(offset, size) || offset >= 0x1000000 || size == 0)
		goto cleanup;
#ifdef GTA_OGC
	if(offset > CdStreamGetImageSectorCount(0) ||
	   size > CdStreamGetImageSectorCount(0) - offset)
		goto cleanup;
#endif
	byteCount = (uint64)size * CDSTREAM_SECTOR_SIZE;
	if(byteCount > SIZE_MAX || byteCount > UINT32_MAX)
		goto cleanup;
	buffer = (uint8*)RwMallocAlign((size_t)byteCount, CDSTREAM_SECTOR_SIZE);
	if(buffer == nil || CdStreamRead(0, buffer, offset, size) != STREAM_SUCCESS ||
	   CdStreamSync(0) != STREAM_NONE)
		goto cleanup;

	mem.start = buffer;
	mem.length = (uint32)byteCount;
	stream = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &mem);
	if(stream == nil)
		goto cleanup;

	if (RwStreamFindChunk(stream, rwID_CLUMP, nil, nil))
		gpPlayerClump = RpClumpStreamRead(stream);
	success = gpPlayerClump != nil;

cleanup:
	if(stream)
		RwStreamClose(stream, &mem);
	if(buffer)
		RwFreeAlign(buffer);
	if (streamWasAdded)
		CdStreamRemoveImages();
	return success;
}

void
CPlayerSkin::Initialise(void)
{
	// empty on PS2
	m_txdSlot = CTxdStore::AddTxdSlot("skin");
	CTxdStore::Create(m_txdSlot);
	CTxdStore::AddRef(m_txdSlot);
}

void
CPlayerSkin::Shutdown(void)
{
	EndFrontendSkinEdit();
	CTxdStore::RemoveTxdSlot(m_txdSlot);
}

RwTexture *
CPlayerSkin::GetSkinTexture(const char *texName)
{
	RwTexture *tex;
	RwRaster *raster;
	int32 width, height, depth, format;

	CTxdStore::PushCurrentTxd();
	CTxdStore::SetCurrentTxd(m_txdSlot);
	tex = RwTextureRead(texName, NULL);
	CTxdStore::PopCurrentTxd();
	if (tex != nil) return tex;

	if (strcmp(DEFAULT_SKIN_NAME, texName) == 0 || texName[0] == '\0')
		sprintf(gString, "models\\generic\\player.bmp");
	else
		sprintf(gString, "skins\\%s.bmp", texName);

	RwImage *image = RtBMPImageRead(gString);
#ifdef GTA_OGC
	// Which of the four steps fails decides the fix, and each guess costs a
	// boot: a nil image is the file or the BMP reader, a nil raster is the GX
	// allocator, a nil setFromImage is the format conversion.
	{
		extern void GeckoLog(const char*);
		char line[96];
		snprintf(line, sizeof(line), "SKIN %s img%d", gString, image != nil);
		GeckoLog(line);
		DVD_FS_GUARD;
		FILE *f = fopen("dvd:/automenu.log", "a");
		if(f){ fprintf(f, "%s\n", line); fclose(f); }
	}
#endif
	if (image) {
		RwImageFindRasterFormat(image, rwRASTERTYPETEXTURE, &width, &height, &depth, &format);
		raster = RwRasterCreate(width, height, depth, format);
#ifdef GTA_OGC
		{
			extern void GeckoLog(const char*);
			char line[96];
			snprintf(line, sizeof(line), "SKIN %dx%d d%d f%x ras%d set%d",
			    width, height, depth, (unsigned)format, raster != nil,
			    raster != nil && RwRasterSetFromImage(raster, image) != nil);
			GeckoLog(line);
			DVD_FS_GUARD;
			FILE *f = fopen("dvd:/automenu.log", "a");
			if(f){ fprintf(f, "%s\n", line); fclose(f); }
		}
#else
		RwRasterSetFromImage(raster, image);
#endif

		tex = RwTextureCreate(raster);
		RwTextureSetName(tex, texName);
		RwTextureSetFilterMode(tex, rwFILTERLINEAR);
		RwTexDictionaryAddTexture(CTxdStore::GetSlot(m_txdSlot)->texDict, tex);

		RwImageDestroy(image);
	}
	return tex;
}

bool
CPlayerSkin::BeginFrontendSkinEdit(void)
{
	if(gpPlayerClump != nil || !LoadPlayerDff())
		return false;
	RpClumpForAllAtomics(gpPlayerClump, CClumpModelInfo::SetAtomicRendererCB, (void*)CVisibilityPlugins::RenderPlayerCB);
	CWorld::Players[0].LoadPlayerSkin();
	gOldFov = CDraw::GetFOV();
	CDraw::SetFOV(30.0f);
	return true;
}

void
CPlayerSkin::EndFrontendSkinEdit(void)
{
	if(gpPlayerClump == nil)
		return;
	RpClumpDestroy(gpPlayerClump);
	gpPlayerClump = NULL;
	CDraw::SetFOV(gOldFov);
}

void
CPlayerSkin::RenderFrontendSkinEdit(void)
{
	if(gpPlayerClump == nil)
		return;
	static float rotation = 0.0f;
	RwRGBAReal AmbientColor = { 0.65f, 0.65f, 0.65f, 1.0f };
	const RwV3d pos = { 1.35f, 0.35f, 7.725f };
	const RwV3d axis = { 0.0f, 1.0f, 0.0f };
	static uint32 LastFlash = 0;

	RwFrame *frame = RpClumpGetFrame(gpPlayerClump);

	if (CTimer::GetTimeInMillisecondsPauseMode() - LastFlash > 7) {
		rotation += 2.0f;
		if (rotation > 360.0f)
			rotation -= 360.0f;
		LastFlash = CTimer::GetTimeInMillisecondsPauseMode();
	}
	RwFrameTransform(frame, RwFrameGetMatrix(RwCameraGetFrame(Scene.camera)), rwCOMBINEREPLACE);
	RwFrameTranslate(frame, &pos, rwCOMBINEPRECONCAT);
	RwFrameRotate(frame, &axis, rotation, rwCOMBINEPRECONCAT);
	RwFrameUpdateObjects(frame);
	SetAmbientColours(&AmbientColor);
	RpClumpRender(gpPlayerClump);
}
