#ifdef GTA_OGC
#include <unistd.h>
extern "C" int gcStreamPrimed(int n);   // sampman_gamecube.cpp (B83)
extern bool gcAutoSkipCutscenes;   // gamecube.cpp: set by autocar.txt
#endif
#include "common.h"

#include "General.h"
#include "CutsceneMgr.h"

#include <new>
#include "Directory.h"
#include "Camera.h"
#include "Draw.h"
#include "Streaming.h"
#include "CdStream.h"
#include "FileMgr.h"
#include "main.h"
#include "AnimManager.h"
#include "AnimBlendAssociation.h"
#include "AnimBlendAssocGroup.h"
#include "AnimBlendClumpData.h"
#include "Pad.h"
#include "DMAudio.h"
#include "World.h"
#include "PlayerPed.h"
#include "Wanted.h"
#include "RpAnimBlend.h"
#include "ModelIndices.h"
#include "TempColModels.h"
#include "ColStore.h"
#include "Radar.h"
#include "Pools.h"

const struct {
	const char *szTrackName;
	int iTrackId;
} musicNameIdAssoc[] = {
	{ "ASS_1", STREAMED_SOUND_CUTSCENE_ASS_1 },
	{ "ASS_2", STREAMED_SOUND_CUTSCENE_ASS_2 },
	{ "BANK_1", STREAMED_SOUND_CUTSCENE_BANK_1 },
	{ "BANK_2A", STREAMED_SOUND_CUTSCENE_BANK_2A },
	{ "BANK_2B", STREAMED_SOUND_CUTSCENE_BANK_2B },
	{ "BANK_3A", STREAMED_SOUND_CUTSCENE_BANK_3A },
	{ "BANK_3B", STREAMED_SOUND_CUTSCENE_BANK_3B },
	{ "BANK_4", STREAMED_SOUND_CUTSCENE_BANK_4 },
	{ "BIKE_1", STREAMED_SOUND_CUTSCENE_BIKE_1 },
	{ "BIKE_2", STREAMED_SOUND_CUTSCENE_BIKE_2 },
	{ "BIKE_3", STREAMED_SOUND_CUTSCENE_BIKE_3 },
	{ "BUD_1", STREAMED_SOUND_CUTSCENE_BUD_1 },
	{ "BUD_2", STREAMED_SOUND_CUTSCENE_BUD_2 },
	{ "BUD_3", STREAMED_SOUND_CUTSCENE_BUD_3 },
	{ "CAP_1", STREAMED_SOUND_CUTSCENE_CAP_1 },
	{ "CAR_1", STREAMED_SOUND_CUTSCENE_CAR_1 },
	{ "CNT_1A", STREAMED_SOUND_CUTSCENE_CNT_1A },
	{ "CNT_1B", STREAMED_SOUND_CUTSCENE_CNT_1B },
	{ "CNT_2", STREAMED_SOUND_CUTSCENE_CNT_2 },
	{ "COK_1", STREAMED_SOUND_CUTSCENE_COK_1 },
	{ "COK_2A", STREAMED_SOUND_CUTSCENE_COK_2A },
	{ "COK_2B", STREAMED_SOUND_CUTSCENE_COK_2B },
	{ "COK_3", STREAMED_SOUND_CUTSCENE_COK_3 },
	{ "COK_4A", STREAMED_SOUND_CUTSCENE_COK_4A },
	{ "COK_4A2", STREAMED_SOUND_CUTSCENE_COK_4A2 },
	{ "COK_4B", STREAMED_SOUND_CUTSCENE_COK_4B },
	{ "COL_1", STREAMED_SOUND_CUTSCENE_COL_1 },
	{ "COL_2", STREAMED_SOUND_CUTSCENE_COL_2 },
	{ "COL_3A", STREAMED_SOUND_CUTSCENE_COL_3A },
	{ "COL_4A", STREAMED_SOUND_CUTSCENE_COL_4A },
	{ "COL_5A", STREAMED_SOUND_CUTSCENE_COL_5A },
	{ "COL_5B", STREAMED_SOUND_CUTSCENE_COL_5B },
	{ "CUB_1", STREAMED_SOUND_CUTSCENE_CUB_1 },
	{ "CUB_2", STREAMED_SOUND_CUTSCENE_CUB_2 },
	{ "CUB_3", STREAMED_SOUND_CUTSCENE_CUB_3 },
	{ "CUB_4", STREAMED_SOUND_CUTSCENE_CUB_4 },
	{ "DRUG_1", STREAMED_SOUND_CUTSCENE_DRUG_1 },
	{ "FIN", STREAMED_SOUND_CUTSCENE_FIN },
	{ "FIN_2", STREAMED_SOUND_CUTSCENE_FIN2 },
	{ "FINALE", STREAMED_SOUND_CUTSCENE_FINALE },
	{ "HAT_1", STREAMED_SOUND_CUTSCENE_HAT_1 },
	{ "HAT_2", STREAMED_SOUND_CUTSCENE_HAT_2 },
	{ "HAT_3", STREAMED_SOUND_CUTSCENE_HAT_3 },
	{ "ICE_1", STREAMED_SOUND_CUTSCENE_ICE_1 },
	{ "INT_A", STREAMED_SOUND_CUTSCENE_INT_A },
	{ "INT_B", STREAMED_SOUND_CUTSCENE_INT_B },
	{ "INT_D", STREAMED_SOUND_CUTSCENE_INT_D },
	{ "INT_M", STREAMED_SOUND_CUTSCENE_INT_M },
	{ "LAW_1A", STREAMED_SOUND_CUTSCENE_LAW_1A },
	{ "LAW_1B", STREAMED_SOUND_CUTSCENE_LAW_1B },
	{ "LAW_2A", STREAMED_SOUND_CUTSCENE_LAW_2A },
	{ "LAW_2B", STREAMED_SOUND_CUTSCENE_LAW_2B },
	{ "LAW_2C", STREAMED_SOUND_CUTSCENE_LAW_2C },
	{ "LAW_3", STREAMED_SOUND_CUTSCENE_LAW_3 },
	{ "LAW_4", STREAMED_SOUND_CUTSCENE_LAW_4 },
	{ "PHIL_1", STREAMED_SOUND_CUTSCENE_PHIL_1 },
	{ "PHIL_2", STREAMED_SOUND_CUTSCENE_PHIL_2 },
	{ "PORN_1", STREAMED_SOUND_CUTSCENE_PORN_1 },
	{ "PORN_2", STREAMED_SOUND_CUTSCENE_PORN_2 },
	{ "PORN_3", STREAMED_SOUND_CUTSCENE_PORN_3 },
	{ "PORN_4", STREAMED_SOUND_CUTSCENE_PORN_4 },
	{ "RESC_1A", STREAMED_SOUND_CUTSCENE_RESC_1A },
	{ "ROK_1", STREAMED_SOUND_CUTSCENE_ROK_1 },
	{ "ROK_2", STREAMED_SOUND_CUTSCENE_ROK_2 },
	{ "ROK_3A", STREAMED_SOUND_CUTSCENE_ROK_3A },
	{ "STRIPA", STREAMED_SOUND_CUTSCENE_STRIPA },
	{ "TAX_1", STREAMED_SOUND_CUTSCENE_TAX_1 },
	{ "TEX_1", STREAMED_SOUND_CUTSCENE_TEX_1 },
	{ "TEX_2", STREAMED_SOUND_CUTSCENE_TEX_2 },
	{ "TEX_3", STREAMED_SOUND_CUTSCENE_TEX_3 },
	{ "GSPOT", STREAMED_SOUND_CUTSCENE_GLIGHT },
	{ "FIST", STREAMED_SOUND_CUTSCENE_FIST },
	{ "EL_PH1", STREAMED_SOUND_CUTSCENE_ELBURRO1_PH1 },
	{ "EL_PH2", STREAMED_SOUND_CUTSCENE_ELBURRO2_PH2 },
	{ NULL, 0 }
};

int
FindCutsceneAudioTrackId(const char *szCutsceneName)
{
	for (int i = 0; musicNameIdAssoc[i].szTrackName; i++) {
		if (!CGeneral::faststricmp(musicNameIdAssoc[i].szTrackName, szCutsceneName))
			return musicNameIdAssoc[i].iTrackId;
	}
	return -1;
}

bool CCutsceneMgr::ms_running;
bool CCutsceneMgr::ms_cutsceneProcessing;
CDirectory *CCutsceneMgr::ms_pCutsceneDir;
CCutsceneObject *CCutsceneMgr::ms_pCutsceneObjects[NUMCUTSCENEOBJECTS];
int32 CCutsceneMgr::ms_numCutsceneObjs;
bool CCutsceneMgr::ms_loaded;
bool CCutsceneMgr::ms_animLoaded;
bool CCutsceneMgr::ms_useLodMultiplier;
char CCutsceneMgr::ms_cutsceneName[CUTSCENENAMESIZE];
CAnimBlendAssocGroup CCutsceneMgr::ms_cutsceneAssociations;
CVector CCutsceneMgr::ms_cutsceneOffset;
float CCutsceneMgr::ms_cutsceneTimer;
bool CCutsceneMgr::ms_wasCutsceneSkipped;
uint32 CCutsceneMgr::ms_cutsceneLoadStatus;
#ifdef GTA_OGC
// Off: the shadow's render-to-texture blur passes (ShadowCamera's multiply
// and invert quads, 64/128px) land on the MAIN screen at the origin on GX —
// the permanent top-left square + dark ball. The regular projected shadows
// cover peds until the shadow camera's raster switch is honoured.
bool CCutsceneMgr::ms_useCutsceneShadows = false;
#else
bool CCutsceneMgr::ms_useCutsceneShadows = true;
#endif

bool bCamLoaded;
bool bIsEverythingRemovedFromTheWorldForTheBiggestFuckoffCutsceneEver; // pls don't shrink the name :P
int32 NumberOfSavedWeapons;
eWeaponType SavedWeaponIDs[TOTAL_WEAPON_SLOTS];
int32 SavedWeaponAmmo[TOTAL_WEAPON_SLOTS];
char uncompressedAnims[8][32];
uint32 numUncompressedAnims;


RpAtomic *
CalculateBoundingSphereRadiusCB(RpAtomic *atomic, void *data)
{
	float radius = RpAtomicGetBoundingSphere(atomic)->radius;
	RwV3d center = RpAtomicGetBoundingSphere(atomic)->center;

	for (RwFrame *frame = RpAtomicGetFrame(atomic); RwFrameGetParent(frame); frame = RwFrameGetParent(frame))
		RwV3dTransformPoints(&center, &center, 1, RwFrameGetMatrix(frame));

	float size = RwV3dLength(&center) + radius;
	if (size > *(float *)data)
		*(float *)data = size;
	return atomic;
}

bool
CCutsceneMgr::Initialise(void)
{
	ms_numCutsceneObjs = 0;
	ms_loaded = false;
	ms_wasCutsceneSkipped = false;
	ms_running = false;
	ms_useLodMultiplier = false;
	ms_animLoaded = false;
	ms_cutsceneProcessing = false;

	ms_pCutsceneDir = new(std::nothrow) CDirectory(CUTSCENEDIRSIZE);
	if(ms_pCutsceneDir == nil || !ms_pCutsceneDir->IsValid() ||
	   !ms_pCutsceneDir->ReadDirFile("ANIM\\CUTS.DIR")){
		delete ms_pCutsceneDir;
		ms_pCutsceneDir = nil;
		return false;
	}

	numUncompressedAnims = 0;
	uncompressedAnims[0][0] = '\0';
	return true;
}

void
CCutsceneMgr::Shutdown(void)
{
	delete ms_pCutsceneDir;
}

void
CCutsceneMgr::LoadCutsceneData(const char *szCutsceneName)
{
	int file;
	uint32 size;
	uint32 offset;
	uint32 byteSize;
	CPlayerPed *pPlayerPed;

	DMAudio.SetEffectsFadeVol(0);
	DMAudio.SetMusicFadeVol(0);
	ms_pCutsceneDir->numEntries = 0;
	if(!ms_pCutsceneDir->ReadDirFile("ANIM\\CUTS.DIR")){
		ms_animLoaded = false;
		return;
	}
#ifdef GTA_OGC
	uint32 gcLoadT0 = CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond();
	printf("CUT %s load begin t=%u f=%u fade=%u\n", szCutsceneName, (unsigned)CTimer::GetTimeInMilliseconds(),
	    (unsigned)CTimer::GetFrameCounter(), (unsigned)CDraw::FadeValue);
#endif

	ms_cutsceneProcessing = true;
	ms_wasCutsceneSkipped = false;
	CTimer::Suspend();
	if (!bIsEverythingRemovedFromTheWorldForTheBiggestFuckoffCutsceneEver)
		CStreaming::RemoveCurrentZonesModels();

	CStreaming::RemoveUnusedModelsInLoadedList();
	CGame::DrasticTidyUpMemory(true);
	strcpy(ms_cutsceneName, szCutsceneName);

	// Load animations
	ms_cutsceneAssociations.DestroyAssociations();
	sprintf(gString, "%s.IFP", szCutsceneName);
	RwStream *stream = nil;
	if (ms_pCutsceneDir->FindItem(gString, offset, size) && size != 0 &&
	    offset <= INT32_MAX/CDSTREAM_SECTOR_SIZE &&
	    size <= INT32_MAX/CDSTREAM_SECTOR_SIZE &&
	    offset <= INT32_MAX/CDSTREAM_SECTOR_SIZE - size) {
		byteSize = size * CDSTREAM_SECTOR_SIZE;
		uint32 byteOffset = offset * CDSTREAM_SECTOR_SIZE;
		CStreaming::MakeSpaceFor(byteSize);
		CGame::DrasticTidyUpMemory(true);
		CStreaming::ImGonnaUseStreamingMemory();
		stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, "ANIM\\CUTS.IMG");
		bool success = stream != nil;
		if(success){
			RwStreamSkip(stream, byteOffset);
			success = STREAMPOS(stream) == byteOffset &&
			          CAnimManager::LoadAnimFile(stream, true, uncompressedAnims, byteSize);
		}
		if(success)
			ms_cutsceneAssociations.CreateAssociations(szCutsceneName);
		if(stream)
			RwStreamClose(stream, nil);
		CStreaming::IHaveUsedStreamingMemory();
		ms_animLoaded = success;
	} else {
		ms_animLoaded = false;
	}

	// Load camera data
	file = CFileMgr::OpenFile("ANIM\\CUTS.IMG", "rb");
	sprintf(gString, "%s.DAT", szCutsceneName);
	if (file != 0 && ms_pCutsceneDir->FindItem(gString, offset, size) && size != 0 &&
	    offset <= INT32_MAX/CDSTREAM_SECTOR_SIZE &&
	    size <= INT32_MAX/CDSTREAM_SECTOR_SIZE &&
	    offset <= INT32_MAX/CDSTREAM_SECTOR_SIZE - size) {
		byteSize = size * CDSTREAM_SECTOR_SIZE;
		CStreaming::ImGonnaUseStreamingMemory();
		bCamLoaded = CFileMgr::Seek(file, offset * CDSTREAM_SECTOR_SIZE, SEEK_SET) &&
		             TheCamera.LoadPathSplines(file, byteSize);
		CStreaming::IHaveUsedStreamingMemory();
	} else {
		bCamLoaded = false;
	}

	if(file)
		CFileMgr::CloseFile(file);

	if (CGeneral::faststricmp(ms_cutsceneName, "finale")) {
		DMAudio.ChangeMusicMode(MUSICMODE_CUTSCENE);
		int trackId = FindCutsceneAudioTrackId(szCutsceneName);
		if (trackId != -1) {
			printf("Start preload audio %s\n", szCutsceneName);
			DMAudio.PreloadCutSceneMusic(trackId);
			printf("End preload audio %s\n", szCutsceneName);
		}
	}

	ms_cutsceneTimer = 0.0f;
	ms_loaded = true;
	ms_cutsceneOffset = CVector(0.0f, 0.0f, 0.0f);

	pPlayerPed = FindPlayerPed();
	pPlayerPed->m_pWanted->ClearQdCrimes();
	pPlayerPed->bIsVisible = false;
	pPlayerPed->m_fCurrentStamina = pPlayerPed->m_fMaxStamina;
	CPad::GetPad(0)->SetDisablePlayerControls(PLAYERCONTROL_CUTSCENE);
	CWorld::Players[CWorld::PlayerInFocus].MakePlayerSafe(true);

	CTimer::Resume();
#ifdef GTA_OGC
	printf("CUT %s load end %ums anim=%d cam=%d\n", szCutsceneName,
	    (unsigned)(CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond() - gcLoadT0), ms_animLoaded, bCamLoaded);
#endif
}

void
CCutsceneMgr::FinishCutscene()
{
	ms_wasCutsceneSkipped = true;
	if (bCamLoaded) {
		CCutsceneMgr::ms_cutsceneTimer = TheCamera.GetCutSceneFinishTime() * 0.001f;
		TheCamera.FinishCutscene();
	}

	FindPlayerPed()->bIsVisible = true;
	CWorld::Players[CWorld::PlayerInFocus].MakePlayerSafe(false);
}

void
CCutsceneMgr::SetupCutsceneToStart(void)
{
	if (bCamLoaded) {
		TheCamera.SetCamCutSceneOffSet(ms_cutsceneOffset);
		TheCamera.TakeControlWithSpline(JUMP_CUT);
		TheCamera.SetWideScreenOn();
	}

	ms_cutsceneOffset.z++;

	for (int i = ms_numCutsceneObjs - 1; i >= 0; i--) {
		assert(RwObjectGetType(ms_pCutsceneObjects[i]->m_rwObject) == rpCLUMP);
		if (CAnimBlendAssociation *pAnimBlendAssoc = RpAnimBlendClumpGetFirstAssociation((RpClump*)ms_pCutsceneObjects[i]->m_rwObject)) {
			assert(pAnimBlendAssoc->hierarchy->sequences[0].HasTranslation());
			if (ms_pCutsceneObjects[i]->m_pAttachTo != nil) {
				pAnimBlendAssoc->flags &= (~ASSOC_HAS_TRANSLATION);
			} else {
				if (pAnimBlendAssoc->hierarchy->IsCompressed()){
					KeyFrameTransCompressed *keyFrames = ((KeyFrameTransCompressed*)pAnimBlendAssoc->hierarchy->sequences[0].GetKeyFrameCompressed(0));
					CVector trans;
					keyFrames->GetTranslation(&trans);
					ms_pCutsceneObjects[i]->SetPosition(ms_cutsceneOffset + trans);
				}else{
					KeyFrameTrans *keyFrames = ((KeyFrameTrans*)pAnimBlendAssoc->hierarchy->sequences[0].GetKeyFrame(0));
					ms_pCutsceneObjects[i]->SetPosition(ms_cutsceneOffset + keyFrames->translation);
				}
			}
			pAnimBlendAssoc->SetRun();
		} else {
			ms_pCutsceneObjects[i]->SetPosition(ms_cutsceneOffset);
		}
		CWorld::Add(ms_pCutsceneObjects[i]);
		if (RwObjectGetType(ms_pCutsceneObjects[i]->m_rwObject) == rpCLUMP) {
			ms_pCutsceneObjects[i]->UpdateRpHAnim();
		}
	}

	CTimer::Update();
	CTimer::Update();
	ms_running = true;
	ms_cutsceneTimer = 0.0f;
}

void
CCutsceneMgr::SetCutsceneAnim(const char *animName, CObject *pObject)
{
	CAnimBlendAssociation *pNewAnim;
	CAnimBlendClumpData *pAnimBlendClumpData;

	if(!ms_animLoaded)
		return;
	assert(RwObjectGetType(pObject->m_rwObject) == rpCLUMP);
	debug("Give cutscene anim %s\n", animName);
	RpAnimBlendClumpRemoveAllAssociations((RpClump*)pObject->m_rwObject);

	pNewAnim = ms_cutsceneAssociations.GetAnimation(animName);
	if (!pNewAnim) {
		debug("\n\nHaven't I told you I can't find the fucking animation %s\n\n\n", animName);
		return;
	}

	if (pNewAnim->hierarchy->IsCompressed())
		pNewAnim->hierarchy->keepCompressed = true;

	CStreaming::ImGonnaUseStreamingMemory();
	pNewAnim = ms_cutsceneAssociations.CopyAnimation(animName);
	CStreaming::IHaveUsedStreamingMemory();

	pNewAnim->SetCurrentTime(0.0f);
	pNewAnim->flags |= ASSOC_HAS_TRANSLATION;
	pNewAnim->flags &= ~ASSOC_RUNNING;

	pAnimBlendClumpData = *RPANIMBLENDCLUMPDATA(pObject->m_rwObject);
	pAnimBlendClumpData->link.Prepend(&pNewAnim->link);

	if (pNewAnim->hierarchy->keepCompressed)
		pAnimBlendClumpData->frames->flag |= AnimBlendFrameData::COMPRESSED;
}

void
CCutsceneMgr::SetCutsceneAnimToLoop(const char* animName)
{
	if(!ms_animLoaded)
		return;
	CAnimBlendAssociation *association = ms_cutsceneAssociations.GetAnimation(animName);
	if(association)
		association->flags |= ASSOC_REPEAT;
}

CCutsceneHead *
CCutsceneMgr::AddCutsceneHead(CObject *pObject, int modelId)
{
	return nil;
}

void UpdateCutsceneObjectBoundingBox(RpClump* clump, int modelId)
{
	if (modelId >= MI_CUTOBJ01 && modelId <= MI_CUTOBJ05) {
		CColModel* pColModel = &CTempColModels::ms_colModelCutObj[modelId - MI_CUTOBJ01];
		float radius = 0.0f;
		RpClumpForAllAtomics(clump, CalculateBoundingSphereRadiusCB, &radius);
		pColModel->boundingSphere.radius = radius;
		pColModel->boundingBox.min = CVector(-radius, -radius, -radius);
		pColModel->boundingBox.max = CVector(radius, radius, radius);
	}
}

CCutsceneObject *
CCutsceneMgr::CreateCutsceneObject(int modelId)
{
	CBaseModelInfo *pModelInfo;
	CColModel *pColModel;
	CCutsceneObject *pCutsceneObject;

	CStreaming::ImGonnaUseStreamingMemory();
	debug("Created cutscene object %s\n", CModelInfo::GetModelInfo(modelId)->GetModelName());
	if (modelId >= MI_CUTOBJ01 && modelId <= MI_CUTOBJ05) {
		pModelInfo = CModelInfo::GetModelInfo(modelId);
		pColModel = &CTempColModels::ms_colModelCutObj[modelId - MI_CUTOBJ01];
		pModelInfo->SetColModel(pColModel);
		UpdateCutsceneObjectBoundingBox((RpClump*)pModelInfo->GetRwObject(), modelId);
	} else if (modelId >= MI_SPECIAL01 && modelId <= MI_SPECIAL21) {
		pModelInfo = CModelInfo::GetModelInfo(modelId);
		if (pModelInfo->GetColModel() == &CTempColModels::ms_colModelPed1) {
			CColModel *colModel = new CColModel();
			colModel->boundingSphere.radius = 2.0f;
			colModel->boundingSphere.center = CVector(0.0f, 0.0f, 0.0f);
			pModelInfo->SetColModel(colModel, true);
		}
		pColModel = pModelInfo->GetColModel();
		float radius = 2.0f;
		pColModel->boundingSphere.radius = radius;
		pColModel->boundingBox.min = CVector(-radius, -radius, -radius);
		pColModel->boundingBox.max = CVector(radius, radius, radius);
	}

	pCutsceneObject = new CCutsceneObject();
	pCutsceneObject->SetModelIndex(modelId);
	if (ms_useCutsceneShadows)
		pCutsceneObject->CreateShadow();
	ms_pCutsceneObjects[ms_numCutsceneObjs++] = pCutsceneObject;
	CStreaming::IHaveUsedStreamingMemory();
	return pCutsceneObject;
}

void
CCutsceneMgr::DeleteCutsceneData(void)
{
	if (!ms_loaded) return;
	CTimer::Suspend();
#ifdef GTA_OGC
	DMAudio.SetEffectsFadeVol(0);
	DMAudio.SetMusicFadeVol(0);
#endif

	ms_cutsceneProcessing = false;
	ms_useLodMultiplier = false;
#ifndef GTA_OGC
	ms_useCutsceneShadows = true;
#endif

	for (--ms_numCutsceneObjs; ms_numCutsceneObjs >= 0; ms_numCutsceneObjs--) {
		CWorld::Remove(ms_pCutsceneObjects[ms_numCutsceneObjs]);
		ms_pCutsceneObjects[ms_numCutsceneObjs]->DeleteRwObject();
		delete ms_pCutsceneObjects[ms_numCutsceneObjs];
		ms_pCutsceneObjects[ms_numCutsceneObjs] = nil;
	}
	ms_numCutsceneObjs = 0;

	for (int i = MI_SPECIAL01; i < MI_SPECIAL21; i++) {
		CBaseModelInfo *minfo = CModelInfo::GetModelInfo(i);
		CColModel *colModel = minfo->GetColModel();
		if (colModel != &CTempColModels::ms_colModelPed1) {
			delete colModel;
			minfo->SetColModel(&CTempColModels::ms_colModelPed1);
		}
	}

	ms_cutsceneAssociations.DestroyAssociations();
	if (ms_animLoaded)
		CAnimManager::RemoveLastAnimFile();

	ms_animLoaded = false;
	numUncompressedAnims = 0;
	uncompressedAnims[0][0] = '\0';

	if (bCamLoaded) {
		TheCamera.RestoreWithJumpCut();
		TheCamera.SetWideScreenOff();
		TheCamera.DeleteCutSceneCamDataMemory();
	}
	ms_running = false;
	ms_loaded = false;
#ifndef GTA_OGC
	DMAudio.SetEffectsFadeVol(127);   // B80: a skipped cutscene never reached the un-mute above
	DMAudio.SetMusicFadeVol(127);
#endif
	CStreaming::RemoveUnusedModelsInLoadedList();   // B80: cutscene props/peds go now, not at the LRU's leisure

	FindPlayerPed()->bIsVisible = true;
	CPad::GetPad(0)->SetEnablePlayerControls(PLAYERCONTROL_CUTSCENE);
	CWorld::Players[CWorld::PlayerInFocus].MakePlayerSafe(false);

	if (CGeneral::faststricmp(ms_cutsceneName, "finale")) {
		DMAudio.StopCutSceneMusic();
		DMAudio.ChangeMusicMode(MUSICMODE_GAME);
	}

	CStreaming::ms_disableStreaming = false;
	CWorld::bProcessCutsceneOnly = false;

	if(bCamLoaded)
		CGame::DrasticTidyUpMemory(TheCamera.GetScreenFadeStatus() == FADE_2);
	
	CPad::GetPad(0)->Clear(false);
	if (bIsEverythingRemovedFromTheWorldForTheBiggestFuckoffCutsceneEver) {
		CStreaming::LoadInitialPeds();
		CStreaming::LoadInitialWeapons();
		CStreaming::LoadInitialVehicles();
		bIsEverythingRemovedFromTheWorldForTheBiggestFuckoffCutsceneEver = false;
		
		CPlayerPed *pPlayerPed = FindPlayerPed();
		for (int i = 0; i < NumberOfSavedWeapons; i++) {
			int32 weaponModelId = CWeaponInfo::GetWeaponInfo(SavedWeaponIDs[i])->m_nModelId;
			uint8 flags = CStreaming::ms_aInfoForModel[weaponModelId].m_flags;
			CStreaming::RequestModel(weaponModelId, STREAMFLAGS_DONT_REMOVE);
			CStreaming::LoadAllRequestedModels(false);
			if (CWeaponInfo::GetWeaponInfo(SavedWeaponIDs[i])->m_nModel2Id != -1) {
				CStreaming::RequestModel(CWeaponInfo::GetWeaponInfo(SavedWeaponIDs[i])->m_nModel2Id, 0);
				CStreaming::LoadAllRequestedModels(false);
			}
			if (!(flags & STREAMFLAGS_DONT_REMOVE))
				CStreaming::SetModelIsDeletable(weaponModelId);
			pPlayerPed->GiveWeapon(SavedWeaponIDs[i], SavedWeaponAmmo[i], true);
		}
		NumberOfSavedWeapons = 0;
	}

#ifdef GTA_OGC
	if(TheCamera.GetScreenFadeStatus() != FADE_2){
		DMAudio.SetEffectsFadeVol(127);
		DMAudio.SetMusicFadeVol(127);
	}
#endif
	CTimer::Resume();
}

void
CCutsceneMgr::Update(void)
{
	enum {
		CUTSCENE_LOADING_0 = 0,
		CUTSCENE_LOADING_AUDIO,
		CUTSCENE_LOADING_2,
		CUTSCENE_LOADING_3,
		CUTSCENE_LOADING_4
	};

	switch (ms_cutsceneLoadStatus) {
	case CUTSCENE_LOADING_AUDIO:
#ifdef GTA_OGC
		// B95: hold the main loop (black screen) until the dialogue stream is primed,
		// 3s at most. B83 returned to the game loop instead, and the world showed
		// through before the cutscene camera took over.
		{
			int wait;
			for(wait = 0; wait < 300 && !gcStreamPrimed(0); wait++) usleep(10000);
			printf("CUT %s audio-wait %dms primed=%d t=%u f=%u\n", ms_cutsceneName, wait*10, gcStreamPrimed(0),
			    (unsigned)CTimer::GetTimeInMilliseconds(), (unsigned)CTimer::GetFrameCounter());
		}
#endif
		SetupCutsceneToStart();
#ifdef GTA_OGC
		printf("CUT %s started camLoaded=%d cam=%d fade=%u\n", ms_cutsceneName, bCamLoaded,
		    TheCamera.Cams[TheCamera.ActiveCam].Mode, (unsigned)CDraw::FadeValue);
		{ extern int gcCutAudioTrace; gcCutAudioTrace = 180; }   // B102: name the "door slam" at the scene start
#endif
		DMAudio.SetEffectsFadeVol(127);   // B80: loaded, sound back
		DMAudio.SetMusicFadeVol(127);
		if (CGeneral::faststricmp(ms_cutsceneName, "finale"))
			DMAudio.PlayPreloadedCutSceneMusic();
		ms_cutsceneLoadStatus++;
		break;
	case CUTSCENE_LOADING_2:
	case CUTSCENE_LOADING_3:
		ms_cutsceneLoadStatus++;
		break;
	case CUTSCENE_LOADING_4:
		ms_cutsceneLoadStatus = CUTSCENE_LOADING_0;
		break;
	default:
		break;
	}

	if (!ms_running) return;

#ifdef GTA_OGC
	// Clipped, not NonClipped. NonClipped hands the scene clock the WHOLE of
	// every load stall: cut.log caught it jumping +68s across 60 streaming
	// frames, which runs the scene to its end time mid-load — the "cutscene
	// at 2x" and the "speech cut halfway" reports are both this teardown.
	// Clipped (60ms/frame cap) is what the sim and the anims already use, so
	// the scene stays in step with them; on PC the two differ only in stall
	// frames PCs do not have.
	ms_cutsceneTimer += CTimer::GetTimeStepInSeconds();
#else
	ms_cutsceneTimer += CTimer::GetTimeStepNonClippedInSeconds();
#endif


	for (int i = 0; i < ms_numCutsceneObjs; i++) {
		int modelId = ms_pCutsceneObjects[i]->GetModelIndex();
		if (modelId >= MI_CUTOBJ01 && modelId <= MI_CUTOBJ05)
			UpdateCutsceneObjectBoundingBox(ms_pCutsceneObjects[i]->GetClump(), modelId);

		if (ms_pCutsceneObjects[i]->m_pAttachTo != nil && modelId >= MI_SPECIAL01 && modelId <= MI_SPECIAL21)
			UpdateCutsceneObjectBoundingBox(ms_pCutsceneObjects[i]->GetClump(), modelId);
	}

	if (bCamLoaded)
		if (CGeneral::faststricmp(ms_cutsceneName, "finale") && TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_FLYBY && ms_cutsceneLoadStatus == CUTSCENE_LOADING_0) {
			if (CPad::GetPad(0)->GetCrossJustDown()
#ifdef GTA_OGC
				|| (gcAutoSkipCutscenes && ms_cutsceneTimer > 1.0f && !ms_wasCutsceneSkipped)
#endif
#ifndef GTA_OGC
				|| (CGame::playingIntro && CPad::GetPad(0)->GetStartJustDown())
#else
				// Start opens the pause menu, including over a cutscene, so it
				// cannot also tear the cutscene down — that ran FinishCutscene
				// on the same frame the frontend was coming up. Cross still
				// skips, which is the button the game prompts for anyway.
#endif
#ifdef GTA_PC_CONTROLS
				|| CPad::GetPad(0)->GetLeftMouseJustDown()
				|| CPad::GetPad(0)->GetEnterJustDown()
				|| CPad::GetPad(0)->GetCharJustDown(' ')
#endif
			){
#ifdef GTA_OGC
					// B98: a scene that ends early ends here or nowhere; say so.
					printf("CUT %s skipped by cross at %.1fs t=%u f=%u\n", ms_cutsceneName, ms_cutsceneTimer,
					    (unsigned)CTimer::GetTimeInMilliseconds(), (unsigned)CTimer::GetFrameCounter());
#endif
					FinishCutscene();
			}
		}
#ifdef GTA_OGC
	if(bCamLoaded && (CTimer::GetFrameCounter() % 120) == 0)
		printf("CUT %s running %.1fs spline %.3f fade=%u f=%u\n", ms_cutsceneName, ms_cutsceneTimer,
		    TheCamera.GetPositionAlongSpline(), (unsigned)CDraw::FadeValue, (unsigned)CTimer::GetFrameCounter());
#endif
}

bool CCutsceneMgr::HasCutsceneFinished(void) { return !bCamLoaded || TheCamera.GetPositionAlongSpline() == 1.0f; }

void
CCutsceneMgr::LoadAnimationUncompressed(char const* name)
{
	strcpy(uncompressedAnims[numUncompressedAnims], name);
	
	// Because that's how CAnimManager knows the end of array
	++numUncompressedAnims;
	assert(numUncompressedAnims < ARRAY_SIZE(uncompressedAnims));
	uncompressedAnims[numUncompressedAnims][0] = '\0';
}

void
CCutsceneMgr::AttachObjectToParent(CObject *pObject, CEntity *pAttachTo)
{
	((CCutsceneObject*)pObject)->m_pAttachmentObject = nil;
	((CCutsceneObject*)pObject)->m_pAttachTo = RpClumpGetFrame(pAttachTo->GetClump());

	debug("Attach %s to %s\n", CModelInfo::GetModelInfo(pObject->GetModelIndex())->GetModelName(), CModelInfo::GetModelInfo(pAttachTo->GetModelIndex())->GetModelName());
}

void
CCutsceneMgr::AttachObjectToFrame(CObject *pObject, CEntity *pAttachTo, const char *frame)
{
	((CCutsceneObject*)pObject)->m_pAttachmentObject = nil;
	((CCutsceneObject*)pObject)->m_pAttachTo = RpAnimBlendClumpFindFrame(pAttachTo->GetClump(), frame)->frame;
	debug("Attach %s to component %s of %s\n",
		CModelInfo::GetModelInfo(pObject->GetModelIndex())->GetModelName(),
		frame,
		CModelInfo::GetModelInfo(pAttachTo->GetModelIndex())->GetModelName());
	if (RwObjectGetType(pObject->m_rwObject) == rpCLUMP) {
		RpClump *clump = (RpClump*)pObject->m_rwObject;
		if (IsClumpSkinned(clump))
			RpAtomicGetBoundingSphere(GetFirstAtomic(clump))->radius *= 1.1f;
	}
}

void
CCutsceneMgr::AttachObjectToBone(CObject *pObject, CObject *pAttachTo, int bone)
{
	RpHAnimHierarchy *hanim = GetAnimHierarchyFromSkinClump(pAttachTo->GetClump());
	RwInt32 id = RpHAnimIDGetIndex(hanim, bone);
	RwMatrix *matrixArray = RpHAnimHierarchyGetMatrixArray(hanim);
	((CCutsceneObject*)pObject)->m_pAttachmentObject = pAttachTo;
	((CCutsceneObject*)pObject)->m_pAttachTo = &matrixArray[id];
	debug("Attach %s to %s\n",
		CModelInfo::GetModelInfo(pObject->GetModelIndex())->GetModelName(),
		CModelInfo::GetModelInfo(pAttachTo->GetModelIndex())->GetModelName());
}

void
CCutsceneMgr::RemoveEverythingFromTheWorldForTheBiggestFuckoffCutsceneEver()
{
	CStreaming::ms_disableStreaming = true;
	CColStore::RemoveAllCollision();
	CWorld::bProcessCutsceneOnly = true;
	ms_cutsceneProcessing = true;

	for (int i = CPools::GetPedPool()->GetSize() - 1; i >= 0; i--) {
		CPed *pPed = CPools::GetPedPool()->GetSlot(i);
		if (pPed) {
			if (!pPed->IsPlayer() && pPed->CanBeDeleted()) {
				CWorld::Remove(pPed);
				delete pPed;
			}
		}
	}

	for (int i = CPools::GetVehiclePool()->GetSize() - 1; i >= 0; i--) {
		CVehicle *pVehicle = CPools::GetVehiclePool()->GetSlot(i);
		if (pVehicle) {
			if (pVehicle->CanBeDeleted()) {
				CWorld::Remove(pVehicle);
				delete pVehicle;
			}
		}
	}

	bIsEverythingRemovedFromTheWorldForTheBiggestFuckoffCutsceneEver = true;
	CStreaming::RemoveCurrentZonesModels();
	CStreaming::SetModelIsDeletable(MI_MALE01);
	CStreaming::SetModelTxdIsDeletable(MI_MALE01);
	CStreaming::SetModelIsDeletable(MI_TAXI_D);
	CStreaming::SetModelTxdIsDeletable(MI_TAXI_D);
	CStreaming::SetModelIsDeletable(MI_NIGHTSTICK);
	CStreaming::SetModelTxdIsDeletable(MI_NIGHTSTICK);
	CStreaming::SetModelIsDeletable(MI_MISSILE);
	CStreaming::SetModelTxdIsDeletable(MI_MISSILE);
	CStreaming::SetModelIsDeletable(MI_POLICE);
	CStreaming::SetModelTxdIsDeletable(MI_POLICE);

	while (CStreaming::RemoveLoadedVehicle()) ;

	CRadar::RemoveRadarSections();

	for (int i = CPools::GetDummyPool()->GetSize() - 1; i >= 0; i--) {
		CDummy* pDummy = CPools::GetDummyPool()->GetSlot(i);
		if (pDummy)
			pDummy->DeleteRwObject();
	}

	for (int i = CPools::GetObjectPool()->GetSize() - 1; i >= 0; i--) {
		CObject* pObject = CPools::GetObjectPool()->GetSlot(i);
		if (pObject)
			pObject->DeleteRwObject();
	}

	for (int i = CPools::GetBuildingPool()->GetSize() - 1; i >= 0; i--) {
		CBuilding* pBuilding = CPools::GetBuildingPool()->GetSlot(i);
		if (pBuilding && pBuilding->m_rwObject != nil && pBuilding->bIsBIGBuilding && pBuilding->bStreamBIGBuilding) {
			if (pBuilding->bIsBIGBuilding)
				CStreaming::RequestModel(pBuilding->GetModelIndex(), 0);
			if (!pBuilding->bImBeingRendered)
				pBuilding->DeleteRwObject();
		}
	}

	CPlayerPed *pPlayerPed = FindPlayerPed();
	pPlayerPed->RemoveWeaponAnims(0, -1000.0f);
	NumberOfSavedWeapons = 0;

	for (int i = 0; i < TOTAL_WEAPON_SLOTS; i++) {
		if (pPlayerPed->m_weapons[i].m_eWeaponType != WEAPONTYPE_UNARMED) {
			SavedWeaponIDs[NumberOfSavedWeapons] = pPlayerPed->m_weapons[i].m_eWeaponType;
			SavedWeaponAmmo[NumberOfSavedWeapons] = pPlayerPed->m_weapons[i].m_nAmmoTotal;
			NumberOfSavedWeapons++;
		}
	}

	pPlayerPed->ClearWeapons();
	CGame::DrasticTidyUpMemory(true);
}
