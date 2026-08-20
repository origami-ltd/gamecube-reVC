#include "common.h"
#include <ctype.h>
#include <new>
#include "main.h"

#include "General.h"
#include "Quaternion.h"
#include "ModelInfo.h"
#include "ModelIndices.h"
#include "TempColModels.h"
#include "VisibilityPlugins.h"
#include "FileMgr.h"
#include "HandlingMgr.h"
#include "CarCtrl.h"
#include "PedType.h"
#include "AnimManager.h"
#include "Game.h"
#include "RwHelper.h"
#include "NodeName.h"
#include "TxdStore.h"
#include "PathFind.h"
#include "ObjectData.h"
#include "DummyObject.h"
#include "World.h"
#include "Zones.h"
#include "ZoneCull.h"
#include "CdStream.h"
#include "FileLoader.h"
#include "MemoryHeap.h"
#include "Streaming.h"
#include "ColStore.h"
#include "Occlusion.h"

char CFileLoader::ms_line[256];

#ifdef GTA_OGC
void gcHeapGuardCheck(const char *marker);   // gamecube.cpp, boot-smash bracketing
#endif

const char*
GetFilename(const char *filename)
{
	char *s = strrchr((char*)filename, '\\');
	return s ? s+1 : filename;
}

void
LoadingScreenLoadingFile(const char *filename)
{
	sprintf(gString, "Loading %s", GetFilename(filename));
	LoadingScreen("Loading the Game", gString, nil);
}

bool
CFileLoader::LoadLevel(const char *filename)
{
	int fd;
	RwTexDictionary *savedTxd;
	bool objectsLoaded;
	char *line;
	char txdname[64];

	savedTxd = RwTexDictionaryGetCurrent();
	objectsLoaded = false;
	if(savedTxd == nil){
		savedTxd = RwTexDictionaryCreate();
		RwTexDictionarySetCurrent(savedTxd);
	}
	fd = CFileMgr::OpenFile(filename, "r");
	if(fd <= 0)
		return false;

	for(line = LoadLine(fd); line; line = LoadLine(fd)){
		if(*line == '#')
			continue;

		if(strncmp(line, "EXIT", 4) == 0)
			break;

		if(strncmp(line, "IMAGEPATH", 9) == 0){
			RwImageSetPath(line + 10);
		}else if(strncmp(line, "TEXDICTION", 10) == 0){
			PUSH_MEMID(MEMID_TEXTURES);
			strcpy(txdname, line+11);
			LoadingScreenLoadingFile(txdname);
			RwTexDictionary *txd = LoadTexDictionary(txdname);
			AddTexDictionaries(savedTxd, txd);
			RwTexDictionaryDestroy(txd);
			POP_MEMID();
		}else if(strncmp(line, "COLFILE", 7) == 0){
			LoadingScreenLoadingFile(line+10);
			if(!LoadCollisionFile(line+10, 0)){
				CFileMgr::CloseFile(fd);
				RwTexDictionarySetCurrent(savedTxd);
				return false;
			}
		}else if(strncmp(line, "MODELFILE", 9) == 0){
			LoadingScreenLoadingFile(line + 10);
			LoadModelFile(line + 10);
		}else if(strncmp(line, "HIERFILE", 8) == 0){
			LoadingScreenLoadingFile(line + 9);
			LoadClumpFile(line + 9);
		}else if(strncmp(line, "IDE", 3) == 0){
			LoadingScreenLoadingFile(line + 4);
			LoadObjectTypes(line + 4);
		}else if(strncmp(line, "IPL", 3) == 0){
			if(!objectsLoaded){
				LoadingScreenLoadingFile("Collision");
				PUSH_MEMID(MEMID_WORLD);
				CObjectData::Initialise("DATA\\OBJECT.DAT");
#ifdef GTA_OGC
				gcHeapGuardCheck("objectdat");
#endif
				if(!CStreaming::Init()){
					POP_MEMID();
					CFileMgr::CloseFile(fd);
					RwTexDictionarySetCurrent(savedTxd);
					return false;
				}
#ifdef GTA_OGC
				gcHeapGuardCheck("streaming-init");
#endif
				POP_MEMID();
				PUSH_MEMID(MEMID_COLLISION);
				CColStore::LoadAllCollision();
#ifdef GTA_OGC
				gcHeapGuardCheck("colstore-all");
#endif
				POP_MEMID();
				for(int i = 0; i < MODELINFOSIZE; i++)
					if(CModelInfo::GetModelInfo(i))
						CModelInfo::GetModelInfo(i)->ConvertAnimFileIndex();
#ifdef GTA_OGC
				gcHeapGuardCheck("anim-convert");
#endif
				objectsLoaded = true;
			}
			PUSH_MEMID(MEMID_WORLD);
			LoadingScreenLoadingFile(line + 4);
			LoadScene(line + 4);
			POP_MEMID();
		}else if(strncmp(line, "SPLASH", 6) == 0){
#ifndef DISABLE_LOADING_SCREEN
			LoadSplash(GetRandomSplashScreen());
#endif
#ifndef GTA_PS2
		}else if(strncmp(line, "CDIMAGE", 7) == 0){
			if(!CdStreamAddImage(line + 8)){
				CFileMgr::CloseFile(fd);
				RwTexDictionarySetCurrent(savedTxd);
				return false;
			}
#endif
		}
	}

	CFileMgr::CloseFile(fd);
	RwTexDictionarySetCurrent(savedTxd);

	int i;
	for(i = 1; i < COLSTORESIZE; i++)
		if(CColStore::GetSlot(i))
			CColStore::GetBoundingBox(i).Grow(120.0f);
	CWorld::RepositionCertainDynamicObjects();
	CColStore::RemoveAllCollision();
	return true;
}

char*
CFileLoader::LoadLine(int fd)
{
	int i;
	char *line;

	if(CFileMgr::ReadLine(fd, ms_line, 256) == false)
		return nil;
	for(i = 0; ms_line[i] != '\0'; i++)
		if(ms_line[i] < ' ' || ms_line[i] == ',')
			ms_line[i] = ' ';
	for(line = ms_line; *line <= ' ' && *line != '\0'; line++);
	return line;
}

RwTexDictionary*
CFileLoader::LoadTexDictionary(const char *filename)
{
	RwTexDictionary *txd;
	RwStream *stream;
	RwUInt32 size;

	txd = nil;
	stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, filename);
	debug("Loading texture dictionary file %s\n", filename);
	if(stream){
		if(RwStreamFindChunk(stream, rwID_TEXDICTIONARY, &size, nil))
			txd = RwTexDictionaryGtaStreamRead(stream, size);
		RwStreamClose(stream, nil);
	}
	if(txd == nil)
		txd = RwTexDictionaryCreate();
	return txd;
}

namespace {

struct CollisionCursor
{
	const uint8 *data;
	uint32 remaining;

	CollisionCursor(const uint8 *data, uint32 size) : data(data), remaining(size) {}

	bool Skip(uint32 size)
	{
		if(size > remaining)
			return false;
		data += size;
		remaining -= size;
		return true;
	}

	bool ReadU16(uint16 &value)
	{
		if(remaining < sizeof(uint16))
			return false;
		value = ReadLE16(data);
		return Skip(sizeof(uint16));
	}

	bool ReadU32(uint32 &value)
	{
		if(remaining < sizeof(uint32))
			return false;
		value = ReadLE32(data);
		return Skip(sizeof(uint32));
	}

	bool ReadFloat(float &value)
	{
		if(remaining < sizeof(float))
			return false;
		value = ReadLEFloat32(data);
		return Skip(sizeof(float));
	}

	bool ArraySize(uint32 count, uint32 stride, uint32 &size) const
	{
		if(stride != 0 && count > UINT32_MAX / stride)
			return false;
		size = count * stride;
		return size <= remaining;
	}
};

struct CollisionModelLayout
{
	CSphere boundingSphere;
	CBox boundingBox;
	uint16 numSpheres;
	uint16 numBoxes;
	uint16 numVertices;
	uint16 numTriangles;
	const uint8 *spheres;
	const uint8 *boxes;
	const uint8 *vertices;
	const uint8 *triangles;
};

struct CollisionRecord
{
	char name[23];
	int16 modelId;
	const uint8 *data;
	uint32 size;
};

struct LoadedCollisionModel
{
	CColSphere *spheres;
	CColBox *boxes;
	CompressedVector *vertices;
	CColTriangle *triangles;

	LoadedCollisionModel(void) : spheres(nil), boxes(nil), vertices(nil), triangles(nil) {}
	~LoadedCollisionModel(void)
	{
		if(spheres)
			RwFree(spheres);
		if(boxes)
			RwFree(boxes);
		if(vertices)
			RwFree(vertices);
		if(triangles)
			RwFree(triangles);
	}
};

struct StagedCollisionModel
{
	CollisionModelLayout layout;
	LoadedCollisionModel loaded;
	CBaseModelInfo *modelInfo;
	CColModel *target;
	CColModel *replacement;
	int modelIndex;
	bool includeModelIndex;

	StagedCollisionModel(void) : modelInfo(nil), target(nil), replacement(nil),
		modelIndex(-1), includeModelIndex(false) {}
	~StagedCollisionModel(void)
	{
		if(replacement)
			delete replacement;
	}
};

static bool
ReadCollisionVector(CollisionCursor &cursor, CVector &vector)
{
	return cursor.ReadFloat(vector.x) && cursor.ReadFloat(vector.y) && cursor.ReadFloat(vector.z);
}

static bool
IsFiniteCollisionVector(const CVector &vector)
{
	return isfinite(vector.x) && isfinite(vector.y) && isfinite(vector.z);
}

static bool
IsValidCollisionBox(const CBox &box)
{
	return IsFiniteCollisionVector(box.min) && IsFiniteCollisionVector(box.max) &&
	       box.min.x <= box.max.x && box.min.y <= box.max.y && box.min.z <= box.max.z;
}

static bool
IsValidCollisionVertex(const CVector &vertex)
{
	if(!IsFiniteCollisionVector(vertex))
		return false;
#ifdef COMPRESSED_COL_VECTORS
	float x = vertex.x * 128.0f;
	float y = vertex.y * 128.0f;
	float z = vertex.z * 128.0f;
	return isfinite(x) && isfinite(y) && isfinite(z) &&
	       x >= INT16_MIN && x <= INT16_MAX &&
	       y >= INT16_MIN && y <= INT16_MAX &&
	       z >= INT16_MIN && z <= INT16_MAX;
#else
	return true;
#endif
}

static bool
ReadCollisionSectionHeader(CollisionCursor &cursor, uint16 &count)
{
	return cursor.ReadU16(count) && count <= INT16_MAX && cursor.Skip(2);
}

static bool
ValidateCollisionModel(const uint8 *data, uint32 size, CollisionModelLayout &layout)
{
	if(data == nil)
		return false;

	CollisionCursor cursor(data, size);
	if(!cursor.ReadFloat(layout.boundingSphere.radius) ||
	   !ReadCollisionVector(cursor, layout.boundingSphere.center) ||
	   !ReadCollisionVector(cursor, layout.boundingBox.min) ||
	   !ReadCollisionVector(cursor, layout.boundingBox.max) ||
	   !isfinite(layout.boundingSphere.radius) || layout.boundingSphere.radius < 0.0f ||
	   !IsFiniteCollisionVector(layout.boundingSphere.center) ||
	   !IsValidCollisionBox(layout.boundingBox))
		return false;

	uint32 sectionSize;
	if(!ReadCollisionSectionHeader(cursor, layout.numSpheres) ||
	   !cursor.ArraySize(layout.numSpheres, 20, sectionSize))
		return false;
	layout.spheres = cursor.data;
	CollisionCursor spheres(cursor.data, sectionSize);
	for(uint32 i = 0; i < layout.numSpheres; i++){
		float radius;
		CVector center;
		if(!spheres.ReadFloat(radius) || !ReadCollisionVector(spheres, center) || !spheres.Skip(4) ||
		   !isfinite(radius) || radius < 0.0f || !IsFiniteCollisionVector(center))
			return false;
	}
	if(!cursor.Skip(sectionSize))
		return false;

	uint16 numLines;
	if(!ReadCollisionSectionHeader(cursor, numLines) || !cursor.ArraySize(numLines, 24, sectionSize))
		return false;
	CollisionCursor lines(cursor.data, sectionSize);
	for(uint32 i = 0; i < numLines; i++){
		CVector start, end;
		if(!ReadCollisionVector(lines, start) || !ReadCollisionVector(lines, end) ||
		   !IsFiniteCollisionVector(start) || !IsFiniteCollisionVector(end))
			return false;
	}
	if(!cursor.Skip(sectionSize))
		return false;

	if(!ReadCollisionSectionHeader(cursor, layout.numBoxes) ||
	   !cursor.ArraySize(layout.numBoxes, 28, sectionSize))
		return false;
	layout.boxes = cursor.data;
	CollisionCursor boxes(cursor.data, sectionSize);
	for(uint32 i = 0; i < layout.numBoxes; i++){
		CBox box;
		if(!ReadCollisionVector(boxes, box.min) || !ReadCollisionVector(boxes, box.max) ||
		   !boxes.Skip(4) || !IsValidCollisionBox(box))
			return false;
	}
	if(!cursor.Skip(sectionSize))
		return false;

	if(!ReadCollisionSectionHeader(cursor, layout.numVertices) ||
	   !cursor.ArraySize(layout.numVertices, 12, sectionSize))
		return false;
	layout.vertices = cursor.data;
	CollisionCursor vertices(cursor.data, sectionSize);
	for(uint32 i = 0; i < layout.numVertices; i++){
		CVector vertex;
		if(!ReadCollisionVector(vertices, vertex) || !IsValidCollisionVertex(vertex))
			return false;
	}
	if(!cursor.Skip(sectionSize))
		return false;

	if(!ReadCollisionSectionHeader(cursor, layout.numTriangles) ||
	   !cursor.ArraySize(layout.numTriangles, 16, sectionSize))
		return false;
	layout.triangles = cursor.data;
	CollisionCursor triangles(cursor.data, sectionSize);
	for(uint32 i = 0; i < layout.numTriangles; i++){
		uint32 a, b, c;
		if(!triangles.ReadU32(a) || !triangles.ReadU32(b) || !triangles.ReadU32(c) ||
		   !triangles.Skip(4) || a >= layout.numVertices || b >= layout.numVertices || c >= layout.numVertices)
			return false;
	}
	return cursor.Skip(sectionSize) && cursor.remaining == 0;
}

template<typename T>
static bool
AllocateCollisionArray(uint16 count, T *&array)
{
	array = nil;
	if(count == 0)
		return true;
	if((size_t)count > SIZE_MAX / sizeof(T))
		return false;
	array = (T*)RwMalloc((size_t)count * sizeof(T));
	return array != nil;
}

static bool
AllocateCollisionModel(const CollisionModelLayout &layout, LoadedCollisionModel &loaded)
{
	return AllocateCollisionArray(layout.numSpheres, loaded.spheres) &&
	       AllocateCollisionArray(layout.numBoxes, loaded.boxes) &&
	       AllocateCollisionArray(layout.numVertices, loaded.vertices) &&
	       AllocateCollisionArray(layout.numTriangles, loaded.triangles);
}

static void
PopulateCollisionModel(const CollisionModelLayout &layout, LoadedCollisionModel &loaded)
{
	for(uint32 i = 0; i < layout.numSpheres; i++){
		const uint8 *data = layout.spheres + i * 20;
		loaded.spheres[i].radius = ReadLEFloat32(data);
		loaded.spheres[i].center.x = ReadLEFloat32(data + 4);
		loaded.spheres[i].center.y = ReadLEFloat32(data + 8);
		loaded.spheres[i].center.z = ReadLEFloat32(data + 12);
		loaded.spheres[i].surface = data[16];
		loaded.spheres[i].piece = data[17];
	}
	for(uint32 i = 0; i < layout.numBoxes; i++){
		const uint8 *data = layout.boxes + i * 28;
		loaded.boxes[i].min.x = ReadLEFloat32(data);
		loaded.boxes[i].min.y = ReadLEFloat32(data + 4);
		loaded.boxes[i].min.z = ReadLEFloat32(data + 8);
		loaded.boxes[i].max.x = ReadLEFloat32(data + 12);
		loaded.boxes[i].max.y = ReadLEFloat32(data + 16);
		loaded.boxes[i].max.z = ReadLEFloat32(data + 20);
		loaded.boxes[i].surface = data[24];
		loaded.boxes[i].piece = data[25];
	}
	for(uint32 i = 0; i < layout.numVertices; i++){
		const uint8 *data = layout.vertices + i * 12;
		loaded.vertices[i].Set(ReadLEFloat32(data), ReadLEFloat32(data + 4), ReadLEFloat32(data + 8));
	}
	for(uint32 i = 0; i < layout.numTriangles; i++){
		const uint8 *data = layout.triangles + i * 16;
		loaded.triangles[i].Set(ReadLE32(data), ReadLE32(data + 4), ReadLE32(data + 8), data[12]);
	}
}

static bool
DecodeCollisionRecord(const uint8 *data, uint32 size, CollisionRecord &record)
{
	if(data == nil || size < 84)
		return false;
	memcpy(record.name, data, 22);
	record.name[22] = '\0';
	record.modelId = (int16)ReadLE16(data + 22);
	record.data = data + 24;
	record.size = size - 24;
	CollisionModelLayout layout;
	return ValidateCollisionModel(record.data, record.size, layout);
}

static void
CommitLoadedCollisionModel(const CollisionModelLayout &layout, LoadedCollisionModel &loaded,
                           CColModel &model)
{
	model.RemoveCollisionVolumes();
	model.boundingSphere = layout.boundingSphere;
	model.boundingBox = layout.boundingBox;
	model.numSpheres = (int16)layout.numSpheres;
	model.numLines = 0;
	model.numBoxes = (int16)layout.numBoxes;
	model.numTriangles = (int16)layout.numTriangles;
	model.spheres = loaded.spheres;
	model.lines = nil;
	model.boxes = loaded.boxes;
	model.vertices = loaded.vertices;
	model.triangles = loaded.triangles;
	model.ownsCollisionVolumes = true;
	REGISTER_MEMPTR(&model.spheres);
	REGISTER_MEMPTR(&model.boxes);
	REGISTER_MEMPTR(&model.vertices);
	REGISTER_MEMPTR(&model.triangles);
	loaded.spheres = nil;
	loaded.boxes = nil;
	loaded.vertices = nil;
	loaded.triangles = nil;
}

static StagedCollisionModel*
AllocateCollisionStages(uint32 count)
{
	if(count == 0 || (size_t)count > SIZE_MAX / sizeof(StagedCollisionModel))
		return nil;
	StagedCollisionModel *stages =
		(StagedCollisionModel*)RwMalloc((size_t)count * sizeof(StagedCollisionModel));
	if(stages == nil)
		return nil;
#ifdef GTA_OGC
	::gcHeapGuardCheck("stages-malloc");
#endif
	for(uint32 i = 0; i < count; i++)
		new (&stages[i]) StagedCollisionModel;
#ifdef GTA_OGC
	::gcHeapGuardCheck("stages-ctor");
#endif
	return stages;
}

static void
DestroyCollisionStages(StagedCollisionModel *stages, uint32 count)
{
	if(stages == nil)
		return;
	for(uint32 i = 0; i < count; i++)
		stages[i].~StagedCollisionModel();
	RwFree(stages);
}

static bool
StageCollisionRecord(const CollisionRecord &record, CBaseModelInfo *modelInfo, int modelIndex,
                     bool includeModelIndex, StagedCollisionModel *stages, uint32 stageIndex)
{
	StagedCollisionModel &stage = stages[stageIndex];
	stage.modelInfo = modelInfo;
	stage.modelIndex = modelIndex;
	stage.includeModelIndex = includeModelIndex;
	if(modelInfo == nil)
		return true;
	for(uint32 i = 0; i < stageIndex; i++)
		if(stages[i].modelInfo == modelInfo)
			return false;

	CColModel *current = modelInfo->GetColModel();
	if(current && modelInfo->DoesOwnColModel() && current->ownsCollisionVolumes){
		stage.target = current;
	}else{
		stage.replacement = new CColModel;
		if(stage.replacement == nil)
			return false;
		stage.target = stage.replacement;
	}
#ifdef GTA_OGC
	::gcHeapGuardCheck("stg-target");
#endif
	if(!ValidateCollisionModel(record.data, record.size, stage.layout))
		return false;
#ifdef GTA_OGC
	::gcHeapGuardCheck("stg-validated");
#endif
	if(!AllocateCollisionModel(stage.layout, stage.loaded))
		return false;
#ifdef GTA_OGC
	::gcHeapGuardCheck("stg-allocated");
#endif
	PopulateCollisionModel(stage.layout, stage.loaded);
#ifdef GTA_OGC
	::gcHeapGuardCheck("stg-populated");
#endif
	return true;
}

static void
CommitCollisionStages(StagedCollisionModel *stages, uint32 count, uint8 colSlot)
{
	for(uint32 i = 0; i < count; i++){
		StagedCollisionModel &stage = stages[i];
		if(stage.modelInfo == nil)
			continue;
		CommitLoadedCollisionModel(stage.layout, stage.loaded, *stage.target);
		stage.target->level = colSlot;
		if(stage.replacement){
			CColModel *current = stage.modelInfo->GetColModel();
			if(current && stage.modelInfo->DoesOwnColModel())
				stage.modelInfo->DeleteCollisionModel();
			stage.modelInfo->SetColModel(stage.replacement, true);
			stage.replacement = nil;
		}
		if(stage.includeModelIndex)
			CColStore::IncludeModelIndex(colSlot, stage.modelIndex);
	}
}

static bool
IsZeroCollisionPadding(const uint8 *data, uint32 size)
{
	if(size == 0 || size >= CDSTREAM_SECTOR_SIZE)
		return false;
	for(uint32 i = 0; i < size; i++)
		if(data[i] != 0)
			return false;
	return true;
}

static bool
ReadCollisionRecord(CollisionCursor &cursor, CollisionRecord &record, bool &hasRecord)
{
	hasRecord = false;
	if(cursor.remaining == 0)
		return true;
	if(cursor.remaining < 8 || memcmp(cursor.data, "COLL", 4) != 0){
		if(!IsZeroCollisionPadding(cursor.data, cursor.remaining))
			return false;
		return cursor.Skip(cursor.remaining);
	}

	uint32 modelSize = ReadLE32(cursor.data + 4);
	if(modelSize < 84 || modelSize > cursor.remaining - 8)
		return false;
#ifdef GTA_OGC
	::gcHeapGuardCheck("rec-header");
#endif
	const uint8 *data = cursor.data + 8;
	if(!DecodeCollisionRecord(data, modelSize, record) || !cursor.Skip(8 + modelSize))
		return false;
#ifdef GTA_OGC
	::gcHeapGuardCheck("rec-decoded");
#endif
	hasRecord = true;
	return true;
}

static bool
CountCollisionBufferRecords(const uint8 *buffer, uint32 size, uint32 &recordCount)
{
	if(buffer == nil || size == 0)
		return false;
	recordCount = 0;
	CollisionCursor cursor(buffer, size);
	while(cursor.remaining != 0){
		CollisionRecord record;
		bool hasRecord;
		if(!ReadCollisionRecord(cursor, record, hasRecord))
			return false;
		if(!hasRecord)
			break;
		if(recordCount == UINT32_MAX)
			return false;
		recordCount++;
	}
	return recordCount != 0;
}

static bool
StageCollisionBuffer(const uint8 *buffer, uint32 size, uint8 colSlot, bool firstTime,
                     StagedCollisionModel *stages, uint32 recordCount)
{
	ColDef *slot = nil;
	if(!firstTime){
		slot = CColStore::GetSlot(colSlot);
		if(slot == nil)
			return false;
	}

	CollisionCursor cursor(buffer, size);
	uint32 stageIndex = 0;
	while(cursor.remaining != 0){
		CollisionRecord record;
		bool hasRecord;
		if(!ReadCollisionRecord(cursor, record, hasRecord))
			return false;
		if(!hasRecord)
			break;
		if(stageIndex >= recordCount)
			return false;

		if(record.size + 24 > 15 * 1024)
			debug("colmodel %s is huge, size %u\n", record.name, record.size + 24);
#ifdef GTA_OGC
		{
			char m[40];
			snprintf(m, sizeof(m), "read %s", record.name);
			::gcHeapGuardCheck(m);
		}
#endif
		int modelIndex = -1;
		CBaseModelInfo *modelInfo = firstTime ?
			CModelInfo::GetModelInfo(record.name, &modelIndex) :
			CModelInfo::GetModelInfo(record.name, slot->minIndex, slot->maxIndex);
#ifdef GTA_OGC
		{
			char m[40];
			snprintf(m, sizeof(m), "mi %s", record.name);
			::gcHeapGuardCheck(m);
		}
#endif
		if(!modelInfo)
			debug("colmodel %s can't find a modelinfo\n", record.name);
		if(!StageCollisionRecord(record, modelInfo, modelIndex, firstTime,
		                         stages, stageIndex))
			return false;
#ifdef GTA_OGC
		// downtows.col is where the boot smash lands; name the exact record.
		::gcHeapGuardCheck(record.name);
#endif
		stageIndex++;
	}
	return stageIndex == recordCount;
}

static bool
LoadCollisionBuffer(const uint8 *buffer, uint32 size, uint8 colSlot, bool firstTime)
{
	uint32 recordCount = 0;
	if(!CountCollisionBufferRecords(buffer, size, recordCount))
		return false;
#ifdef GTA_OGC
	::gcHeapGuardCheck("colbuf-counted");
#endif
	StagedCollisionModel *stages = AllocateCollisionStages(recordCount);
	if(stages == nil)
		return false;
	bool success = StageCollisionBuffer(buffer, size, colSlot, firstTime, stages, recordCount);
#ifdef GTA_OGC
	::gcHeapGuardCheck("colbuf-staged");
#endif
	if(success)
		CommitCollisionStages(stages, recordCount, colSlot);
#ifdef GTA_OGC
	::gcHeapGuardCheck("colbuf-committed");
#endif
	DestroyCollisionStages(stages, recordCount);
	return success;
}

static bool
ReadCollisionFileRecord(int fd, CollisionRecord &record, bool &hasRecord)
{
	hasRecord = false;
	uint8 header[8];
	size_t bytesRead = CFileMgr::Read(fd, (char*)header, sizeof(header));
	if(bytesRead == 0)
		return CFileMgr::GetErrorReadWrite(fd) != 0;
	if(bytesRead != sizeof(header) || memcmp(header, "COLL", 4) != 0)
		return false;

	uint32 modelSize = ReadLE32(header + 4);
	if(modelSize < 84 || modelSize > sizeof(work_buff) ||
	   CFileMgr::Read(fd, (char*)work_buff, modelSize) != modelSize ||
	   !DecodeCollisionRecord(work_buff, modelSize, record))
		return false;
	hasRecord = true;
	return true;
}

static bool
CountCollisionFileRecords(int fd, uint32 &recordCount)
{
	recordCount = 0;
	for(;;){
		CollisionRecord record;
		bool hasRecord;
		if(!ReadCollisionFileRecord(fd, record, hasRecord))
			return false;
		if(!hasRecord)
			return recordCount != 0;
		if(recordCount == UINT32_MAX)
			return false;
		recordCount++;
	}
}

static bool
StageCollisionFile(int fd, StagedCollisionModel *stages, uint32 recordCount)
{
	uint32 stageIndex = 0;
	for(;;){
		CollisionRecord record;
		bool hasRecord;
		if(!ReadCollisionFileRecord(fd, record, hasRecord))
			return false;
		if(!hasRecord)
			return stageIndex == recordCount;
		if(stageIndex >= recordCount)
			return false;

		if(record.size + 24 > 15 * 1024)
			debug("colmodel %s is huge, size %u\n", record.name, record.size + 24);
		CBaseModelInfo *modelInfo = CModelInfo::GetModelInfo(record.name, nil);
		if(!modelInfo)
			debug("colmodel %s can't find a modelinfo\n", record.name);
		if(!StageCollisionRecord(record, modelInfo, -1, false, stages, stageIndex))
			return false;
#ifdef GTA_OGC
		// The GENERIC.COL step is where the boot heap-smash lands; per-record
		// checks name the exact record that does it.
		::gcHeapGuardCheck(record.name);
#endif
		stageIndex++;
	}
}

}

bool
CFileLoader::LoadCollisionFile(const char *filename, uint8 colSlot)
{
	if(filename == nil)
		return false;

	PUSH_MEMID(MEMID_COLLISION);
	debug("Loading collision file %s\n", filename);
#ifdef GTA_OGC
	gcHeapGuardCheck("col-open");
#endif
	int fd = CFileMgr::OpenFile(filename, "rb");
	uint32 recordCount = 0;
	bool success = fd > 0 && CountCollisionFileRecords(fd, recordCount);
#ifdef GTA_OGC
	gcHeapGuardCheck("col-counted");
#endif
	StagedCollisionModel *stages = success ? AllocateCollisionStages(recordCount) : nil;
	if(success)
		success = stages != nil && CFileMgr::Seek(fd, 0, SEEK_SET) &&
		          StageCollisionFile(fd, stages, recordCount);
#ifdef GTA_OGC
	gcHeapGuardCheck("col-staged");
#endif
	if(success)
		CommitCollisionStages(stages, recordCount, colSlot);
#ifdef GTA_OGC
	gcHeapGuardCheck("col-committed");
#endif
	DestroyCollisionStages(stages, recordCount);
	if(fd > 0)
		CFileMgr::CloseFile(fd);
	POP_MEMID();
	return success;
}

bool
CFileLoader::LoadCollisionFileFirstTime(const uint8 *buffer, uint32 size, uint8 colSlot)
{
	return LoadCollisionBuffer(buffer, size, colSlot, true);
}

bool
CFileLoader::LoadCollisionFile(const uint8 *buffer, uint32 size, uint8 colSlot)
{
	return LoadCollisionBuffer(buffer, size, colSlot, false);
}

bool
CFileLoader::LoadCollisionModel(const uint8 *data, uint32 size, CColModel &model)
{
	if(!model.ownsCollisionVolumes)
		return false;
	CollisionModelLayout layout;
	if(!ValidateCollisionModel(data, size, layout))
		return false;

	LoadedCollisionModel loaded;
	if(!AllocateCollisionModel(layout, loaded))
		return false;
	PopulateCollisionModel(layout, loaded);
	CommitLoadedCollisionModel(layout, loaded, model);
	return true;
}

static void
GetNameAndLOD(char *nodename, char *name, int *n)
{
	char *underscore = nil;
	for(char *s = nodename; *s != '\0'; s++){
		if(s[0] == '_' && (s[1] == 'l' || s[1] == 'L') && isdigit(s[2]))
			underscore = s;
	}
	if(underscore){
		strncpy(name, nodename, underscore - nodename);
		name[underscore - nodename] = '\0';
		*n = atoi(underscore + 2);
	}else{
		strncpy(name, nodename, 24);
		*n = 0;
	}
}

RpAtomic*
CFileLoader::FindRelatedModelInfoCB(RpAtomic *atomic, void *data)
{
	CSimpleModelInfo *mi;
	char *nodename, name[24];
	int n;
	RpClump *clump = (RpClump*)data;

	nodename = GetFrameNodeName(RpAtomicGetFrame(atomic));
	GetNameAndLOD(nodename, name, &n);
	mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(name, nil);
	if(mi){
		assert(mi->IsSimple());
		CVisibilityPlugins::SetAtomicRenderCallback(atomic, nil);
		mi->SetAtomic(n, atomic);
		RpClumpRemoveAtomic(clump, atomic);
		RpAtomicSetFrame(atomic, RwFrameCreate());
		CVisibilityPlugins::SetAtomicModelInfo(atomic, mi);
	}else{
		debug("Can't find Atomic %s\n", name);
	}

	return atomic;
}

#ifdef LIBRW
void
InitClump(RpClump *clump)
{
	RpClumpForAllAtomics(clump, ConvertPlatformAtomic, nil);
}
#else
#define InitClump(clump)
#endif

void
CFileLoader::LoadModelFile(const char *filename)
{
	RwStream *stream;
	RpClump *clump;

	debug("Loading model file %s\n", filename);
	stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, filename);
	if(RwStreamFindChunk(stream, rwID_CLUMP, nil, nil)){
		clump = RpClumpStreamRead(stream);
		if(clump){
			InitClump(clump);
			RpClumpForAllAtomics(clump, FindRelatedModelInfoCB, clump);
			RpClumpDestroy(clump);
		}
	}
	RwStreamClose(stream, nil);
}

void
CFileLoader::LoadClumpFile(const char *filename)
{
	RwStream *stream;
	RpClump *clump;
	char *nodename, name[24];
	int n;
	CClumpModelInfo *mi;

	debug("Loading model file %s\n", filename);
	stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, filename);
	while(RwStreamFindChunk(stream, rwID_CLUMP, nil, nil)){
		clump = RpClumpStreamRead(stream);
		if(clump){
			nodename = GetFrameNodeName(RpClumpGetFrame(clump));
			GetNameAndLOD(nodename, name, &n);
			mi = (CClumpModelInfo*)CModelInfo::GetModelInfo(name, nil);
			if(mi){
				InitClump(clump);
				assert(mi->IsClump());
				mi->SetClump(clump);
			}else
				RpClumpDestroy(clump);
		}
	}
	RwStreamClose(stream, nil);
}

bool
CFileLoader::LoadClumpFile(RwStream *stream, uint32 id)
{
	RpClump *clump;
	CClumpModelInfo *mi;

	if(!RwStreamFindChunk(stream, rwID_CLUMP, nil, nil))
		return false;
	clump = RpClumpStreamRead(stream);
	if(clump == nil)
		return false;
	InitClump(clump);
	mi = (CClumpModelInfo*)CModelInfo::GetModelInfo(id);
	mi->SetClump(clump);
	return true;
}

bool
CFileLoader::StartLoadClumpFile(RwStream *stream, uint32 id)
{
	RwUInt32 size;
	if(RwStreamFindChunk(stream, rwID_CLUMP, &size, nil)){
		printf("Start loading %s\n", CModelInfo::GetModelInfo(id)->GetModelName());
		return RpClumpGtaStreamRead1(stream, size);
	}else{
		printf("FAILED\n");
		return false;
	}
}

bool
CFileLoader::FinishLoadClumpFile(RwStream *stream, uint32 id)
{
	RpClump *clump;
	CClumpModelInfo *mi;

	printf("Finish loading %s\n", CModelInfo::GetModelInfo(id)->GetModelName());
	clump = RpClumpGtaStreamRead2(stream);

	if(clump){
		InitClump(clump);
		mi = (CClumpModelInfo*)CModelInfo::GetModelInfo(id);
		mi->SetClump(clump);
		return true;
	}else{
		printf("FAILED\n");
		return false;
	}
}

CSimpleModelInfo *gpRelatedModelInfo;

bool
CFileLoader::LoadAtomicFile(RwStream *stream, uint32 id)
{
	RpClump *clump;

	if(RwStreamFindChunk(stream, rwID_CLUMP, nil, nil)){
		clump = RpClumpStreamRead(stream);
		if(clump == nil)
			return false;
		InitClump(clump);
		gpRelatedModelInfo = (CSimpleModelInfo*)CModelInfo::GetModelInfo(id);
		RpClumpForAllAtomics(clump, SetRelatedModelInfoCB, clump);
		RpClumpDestroy(clump);
	}
	return true;
}

RpAtomic*
CFileLoader::SetRelatedModelInfoCB(RpAtomic *atomic, void *data)
{
	char *nodename, name[24];
	int n;
	RpClump *clump = (RpClump*)data;

	nodename = GetFrameNodeName(RpAtomicGetFrame(atomic));
	GetNameAndLOD(nodename, name, &n);
	CVisibilityPlugins::SetAtomicRenderCallback(atomic, nil);
	gpRelatedModelInfo->SetAtomic(n, atomic);
	RpClumpRemoveAtomic(clump, atomic);
	RpAtomicSetFrame(atomic, RwFrameCreate());
	CVisibilityPlugins::SetAtomicModelInfo(atomic, gpRelatedModelInfo);
	return atomic;
}

RpClump*
CFileLoader::LoadAtomicFile2Return(const char *filename)
{
	RwStream *stream;
	RpClump *clump;

	clump = nil;
	debug("Loading model file %s\n", filename);
	stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, filename);
	if(RwStreamFindChunk(stream, rwID_CLUMP, nil, nil))
		clump = RpClumpStreamRead(stream);
	if(clump)
		InitClump(clump);
	RwStreamClose(stream, nil);
	return clump;
}

static RwTexture*
MoveTexturesCB(RwTexture *texture, void *pData)
{
	RwTexDictionaryAddTexture((RwTexDictionary*)pData, texture);
	return texture;
}

void
CFileLoader::AddTexDictionaries(RwTexDictionary *dst, RwTexDictionary *src)
{
	RwTexDictionaryForAllTextures(src, MoveTexturesCB, dst);
}

#define isLine3(l, a, b, c) ((l[0] == a) && (l[1] == b) && (l[2] == c))
#define isLine4(l, a, b, c, d) ((l[0] == a) && (l[1] == b) && (l[2] == c) && (l[3] == d))

void
CFileLoader::LoadObjectTypes(const char *filename)
{
	enum {
		NONE,
		OBJS,
		MLO,	// unused but enum still has it
		TOBJ,
		WEAP,
		HIER,
		CARS,
		PEDS,
		PATH,
		TWODFX
	};
	char *line;
	int fd;
	int section;
	int pathIndex;
	int id, pathType;
	int minID, maxID;

	section = NONE;
	minID = INT32_MAX;
	maxID = -1;
	pathIndex = -1;
	debug("Loading object types from %s...\n", filename);

	fd = CFileMgr::OpenFile(filename, "rb");
	assert(fd > 0);
	for(line = CFileLoader::LoadLine(fd); line; line = CFileLoader::LoadLine(fd)){
		if(*line == '\0' || *line == '#')
			continue;

		if(section == NONE){
			if(isLine4(line, 'o','b','j','s')) section = OBJS;
			else if(isLine4(line, 't','o','b','j')) section = TOBJ;
			else if(isLine4(line, 'w','e','a','p')) section = WEAP;
			else if(isLine4(line, 'h','i','e','r')) section = HIER;
			else if(isLine4(line, 'c','a','r','s')) section = CARS;
			else if(isLine4(line, 'p','e','d','s')) section = PEDS;
			else if(isLine4(line, 'p','a','t','h')) section = PATH;
			else if(isLine4(line, '2','d','f','x')) section = TWODFX;
		}else if(isLine3(line, 'e','n','d')){
			section = NONE;
		}else switch(section){
		case OBJS:
			id = LoadObject(line);
			if(id > maxID) maxID = id;
			if(id < minID) minID = id;
			break;
		case TOBJ:
			id = LoadTimeObject(line);
			if(id > maxID) maxID = id;
			if(id < minID) minID = id;
			break;
		case WEAP:
			LoadWeaponObject(line);
			break;
		case HIER:
			LoadClumpObject(line);
			break;
		case CARS:
			LoadVehicleObject(line);
			break;
		case PEDS:
			LoadPedObject(line);
			break;
		case PATH:
			if(pathIndex == -1){
				id = LoadPathHeader(line, pathType);
				pathIndex = 0;
			}else{
				if(pathType == 0)
					LoadPedPathNode(line, id, pathIndex);
				else if (pathType == 1)
					LoadCarPathNode(line, id, pathIndex, false);
				else if (pathType == 2)
					LoadCarPathNode(line, id, pathIndex, true);
				pathIndex++;
				if(pathIndex == 12)
					pathIndex = -1;
			}
			break;
		case TWODFX:
			Load2dEffect(line);
			break;
		}
	}
	CFileMgr::CloseFile(fd);

	for(id = minID; id <= maxID; id++){
		CSimpleModelInfo *mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(id);
		if(mi && mi->IsBuilding())
			mi->SetupBigBuilding(minID, maxID);
	}
}

void
SetModelInfoFlags(CSimpleModelInfo *mi, uint32 flags)
{
	mi->m_wetRoadReflection =	!!(flags & 1);
	mi->m_noFade =	!!(flags & 2);
	mi->m_drawLast =	!!(flags & (4|8));
	mi->m_additive =	!!(flags & 8);
	mi->m_isSubway =	!!(flags & 0x10);
	mi->m_ignoreLight =	!!(flags & 0x20);
	mi->m_noZwrite =	!!(flags & 0x40);
	mi->m_noShadows =	!!(flags & 0x80);
	mi->m_ignoreDrawDist =	!!(flags & 0x100);
	mi->m_isCodeGlass =	!!(flags & 0x200);
	mi->m_isArtistGlass =	!!(flags & 0x400);
}

int
CFileLoader::LoadObject(const char *line)
{
	int id, numObjs;
	char model[24], txd[24];
	float dist[3];
	uint32 flags;
	int damaged;
	CSimpleModelInfo *mi;

	if(sscanf(line, "%d %s %s %d", &id, model, txd, &numObjs) != 4)
		return 0;	// game returns return value

	switch(numObjs){
	case 1:
		sscanf(line, "%d %s %s %d %f %d",
			&id, model, txd, &numObjs, &dist[0], &flags);
		damaged = 0;
		break;
	case 2:
		sscanf(line, "%d %s %s %d %f %f %d",
			&id, model, txd, &numObjs, &dist[0], &dist[1], &flags);
		damaged = dist[0] < dist[1] ?	// Are distances increasing?
			0 :	// Yes, no damage model
			1;	// No, 1 is damaged
		break;
	case 3:
		sscanf(line, "%d %s %s %d %f %f %f %d",
			&id, model, txd, &numObjs, &dist[0], &dist[1], &dist[2], &flags);
		damaged = dist[0] < dist[1] ?	// Are distances increasing?
				(dist[1] < dist[2] ? 0 : 2) :	// Yes, only 2 can still be a damage model
			1;	// No, 1 and 2 are damaged
		break;
	}

	mi = CModelInfo::AddSimpleModel(id);
	mi->SetModelName(model);
	mi->SetNumAtomics(numObjs);
	mi->SetLodDistances(dist);
	SetModelInfoFlags(mi, flags);
	mi->m_firstDamaged = damaged;
	mi->SetTexDictionary(txd);
	MatchModelString(model, id);

	return id;
}

int
CFileLoader::LoadTimeObject(const char *line)
{
	int id, numObjs;
	char model[24], txd[24];
	float dist[3];
	uint32 flags;
	int timeOn, timeOff;
	int damaged;
	CTimeModelInfo *mi, *other;

	if(sscanf(line, "%d %s %s %d", &id, model, txd, &numObjs) != 4)
		return 0;	// game returns return value

	switch(numObjs){
	case 1:
		sscanf(line, "%d %s %s %d %f %d %d %d",
			&id, model, txd, &numObjs, &dist[0], &flags, &timeOn, &timeOff);
		damaged = 0;
		break;
	case 2:
		sscanf(line, "%d %s %s %d %f %f %d %d %d",
			&id, model, txd, &numObjs, &dist[0], &dist[1], &flags, &timeOn, &timeOff);
		damaged = dist[0] < dist[1] ?	// Are distances increasing?
			0 :	// Yes, no damage model
			1;	// No, 1 is damaged
		break;
	case 3:
		sscanf(line, "%d %s %s %d %f %f %f %d %d %d",
			&id, model, txd, &numObjs, &dist[0], &dist[1], &dist[2], &flags, &timeOn, &timeOff);
		damaged = dist[0] < dist[1] ?	// Are distances increasing?
				(dist[1] < dist[2] ? 0 : 2) :	// Yes, only 2 can still be a damage model
			1;	// No, 1 and 2 are damaged
		break;
	}

	mi = CModelInfo::AddTimeModel(id);
	mi->SetModelName(model);
	mi->SetNumAtomics(numObjs);
	mi->SetLodDistances(dist);
	SetModelInfoFlags(mi, flags);
	mi->m_firstDamaged = damaged;
	mi->SetTimes(timeOn, timeOff);
	mi->SetTexDictionary(txd);
	other = mi->FindOtherTimeModel();
	if(other)
		other->SetOtherTimeModel(id);
	MatchModelString(model, id);

	return id;
}

int
CFileLoader::LoadWeaponObject(const char *line)
{
	int id, numObjs;
	char model[24], txd[24], animFile[16];
	float dist;
	CWeaponModelInfo *mi;

	sscanf(line, "%d %s %s %s %d %f", &id, model, txd, animFile, &numObjs, &dist);

	mi = CModelInfo::AddWeaponModel(id);
	mi->SetModelName(model);
	mi->SetNumAtomics(1);
	mi->m_lodDistances[0] = dist;
	mi->SetTexDictionary(txd);
	mi->SetAnimFile(animFile);
	mi->SetColModel(&CTempColModels::ms_colModelWeapon);
	MatchModelString(model, id);
	return id;
}

void
CFileLoader::LoadClumpObject(const char *line)
{
	int id;
	char model[24], txd[24];
	CClumpModelInfo *mi;

	if(sscanf(line, "%d %s %s", &id, model, txd) == 3){
		mi = CModelInfo::AddClumpModel(id);
		mi->SetModelName(model);
		mi->SetTexDictionary(txd);
		mi->SetColModel(&CTempColModels::ms_colModelBBox);
	}
}

void
CFileLoader::LoadVehicleObject(const char *line)
{
	int id;
	char model[24], txd[24];
	char type[8], handlingId[16], gamename[32], animFile[16], vehclass[12];
	uint32 frequency, comprules;
	int32 level, misc;
	float wheelScale;
	CVehicleModelInfo *mi;
	char *p;

	sscanf(line, "%d %s %s %s %s %s %s %s %d %d %x %d %f",
		&id, model, txd,
		type, handlingId, gamename, animFile, vehclass,
		&frequency, &level, &comprules, &misc, &wheelScale);

	mi = CModelInfo::AddVehicleModel(id);
	mi->SetModelName(model);
	mi->SetTexDictionary(txd);
	mi->SetAnimFile(animFile);
	for(p = gamename; *p; p++)
		if(*p == '_') *p = ' ';
	strcpy(mi->m_gameName, gamename);
	mi->m_level = level;
	mi->m_compRules = comprules;

	if(strcmp(type, "car") == 0){
		mi->m_wheelId = misc;
		mi->m_wheelScale = wheelScale;
		mi->m_vehicleType = VEHICLE_TYPE_CAR;
	}else if(strcmp(type, "boat") == 0){
		mi->m_vehicleType = VEHICLE_TYPE_BOAT;
	}else if(strcmp(type, "train") == 0){
		mi->m_vehicleType = VEHICLE_TYPE_TRAIN;
	}else if(strcmp(type, "heli") == 0){
		mi->m_vehicleType = VEHICLE_TYPE_HELI;
	}else if(strcmp(type, "plane") == 0){
		mi->m_planeLodId = misc;
		mi->m_wheelScale = 1.0f;
		mi->m_vehicleType = VEHICLE_TYPE_PLANE;
	}else if(strcmp(type, "bike") == 0){
		mi->m_bikeSteerAngle = misc;
		mi->m_wheelScale = wheelScale;
		mi->m_vehicleType = VEHICLE_TYPE_BIKE;
	}else
		assert(0);

	mi->m_handlingId = mod_HandlingManager.GetHandlingId(handlingId);

	if(strcmp(vehclass, "normal") == 0)
		mi->m_vehicleClass = CCarCtrl::NORMAL;
	else if(strcmp(vehclass, "poorfamily") == 0)
		mi->m_vehicleClass = CCarCtrl::POOR;
	else if(strcmp(vehclass, "richfamily") == 0)
		mi->m_vehicleClass = CCarCtrl::RICH;
	else if(strcmp(vehclass, "executive") == 0)
		mi->m_vehicleClass = CCarCtrl::EXEC;
	else if(strcmp(vehclass, "worker") == 0)
		mi->m_vehicleClass = CCarCtrl::WORKER;
	else if(strcmp(vehclass, "big") == 0)
		mi->m_vehicleClass = CCarCtrl::BIG;
	else if(strcmp(vehclass, "taxi") == 0)
		mi->m_vehicleClass = CCarCtrl::TAXI;
	else if(strcmp(vehclass, "moped") == 0)
		mi->m_vehicleClass = CCarCtrl::MOPED;
	else if(strcmp(vehclass, "motorbike") == 0)
		mi->m_vehicleClass = CCarCtrl::MOTORBIKE;
	else if(strcmp(vehclass, "leisureboat") == 0)
		mi->m_vehicleClass = CCarCtrl::LEISUREBOAT;
	else if(strcmp(vehclass, "workerboat") == 0)
		mi->m_vehicleClass = CCarCtrl::WORKERBOAT;
	else if(strcmp(vehclass, "ignore") == 0) {
		mi->m_vehicleClass = -1;
		return;
	}
	CCarCtrl::AddToCarArray(id, mi->m_vehicleClass);
	mi->m_frequency = frequency;
}

void
CFileLoader::LoadPedObject(const char *line)
{
	int id;
	char model[24], txd[24];
	char pedType[24], pedStats[24], animGroup[24], animFile[16];
	int carsCanDrive;
	CPedModelInfo *mi;
	int animGroupId;
	int radio1, radio2;

	sscanf(line, "%d %s %s %s %s %s %x %s %d %d",
	          &id, model, txd,
	          pedType, pedStats, animGroup, &carsCanDrive,
		  animFile, &radio1, &radio2);

	mi = CModelInfo::AddPedModel(id);
	mi->SetModelName(model);
	mi->SetTexDictionary(txd);
	mi->SetAnimFile(animFile);
	mi->SetColModel(&CTempColModels::ms_colModelPed1);
	mi->m_pedType = CPedType::FindPedType(pedType);
	mi->m_pedStatType = CPedStats::GetPedStatType(pedStats);
	for(animGroupId = 0; animGroupId < NUM_ANIM_ASSOC_GROUPS; animGroupId++)
		if(strcmp(animGroup, CAnimManager::GetAnimGroupName((AssocGroupId)animGroupId)) == 0)
			break;
	assert(animGroupId < NUM_ANIM_ASSOC_GROUPS);
	mi->m_animGroup = animGroupId;
	mi->m_carsCanDrive = carsCanDrive;
	mi->radio1 = radio1;
	mi->radio2 = radio2;
}

int
CFileLoader::LoadPathHeader(const char *line, int &type)
{
	int id;
	char modelname[32];

	sscanf(line, "%d %d %s", &type, &id, modelname);
	return id;
}

void
CFileLoader::LoadPedPathNode(const char *line, int id, int node)
{
	int type, next, cross, numLeft, numRight, speed, flags;
	float x, y, z, width, spawnRate;

	if(sscanf(line, "%d %d %d %f %f %f %f %d %d %d %d %f",
			&type, &next, &cross, &x, &y, &z, &width, &numLeft, &numRight,
			&speed, &flags, &spawnRate) != 12)
		spawnRate = 1.0f;

	if(id == -1)
		ThePaths.StoreDetachedNodeInfoPed(node, type, next, x, y, z,
			width, !!cross, !!(flags&1), !!(flags&4), spawnRate*15.0f);
	else
		ThePaths.StoreNodeInfoPed(id, node, type, next, x, y, z,
			width, !!cross, spawnRate*15.0f);
}

void
CFileLoader::LoadCarPathNode(const char *line, int id, int node, bool waterPath)
{
	int type, next, cross, numLeft, numRight, speed, flags;
	float x, y, z, width, spawnRate;

	if(sscanf(line, "%d %d %d %f %f %f %f %d %d %d %d %f",
			&type, &next, &cross, &x, &y, &z, &width, &numLeft, &numRight,
			&speed, &flags, &spawnRate) != 12)
		spawnRate = 1.0f;

	if(id == -1)
		ThePaths.StoreDetachedNodeInfoCar(node, type, next, x, y, z, width, numLeft, numRight,
			!!(flags&1), !!(flags&4), speed, !!(flags&2), waterPath, spawnRate * 15, false);
	else
		ThePaths.StoreNodeInfoCar(id, node, type, next, x, y, z, 0, numLeft, numRight,
			!!(flags&1), !!(flags&4), speed, !!(flags&2), waterPath, spawnRate * 15);
}


void
CFileLoader::Load2dEffect(const char *line)
{
	int id, r, g, b, a, type, ptype;
	float x, y, z;
	char corona[32], shadow[32];
	int shadowIntens, lightType, roadReflection, flare, flags, probability;
	CBaseModelInfo *mi;
	C2dEffect *effect;
	char *p;

	sscanf(line, "%d %f %f %f %d %d %d %d %d", &id, &x, &y, &z, &r, &g, &b, &a, &type);

	CTxdStore::PushCurrentTxd();
	CTxdStore::SetCurrentTxd(CTxdStore::FindTxdSlot("particle"));

	mi = CModelInfo::GetModelInfo(id);
	effect = CModelInfo::Get2dEffectStore().Alloc();
	mi->Add2dEffect(effect);
	effect->pos = CVector(x, y, z);
	effect->col = CRGBA(r, g, b, a);
	effect->type = type;

	switch(effect->type){
	case EFFECT_LIGHT:
		while(*line++ != '"');
		p = corona;
		while(*line != '"') *p++ = *line++;
		*p = '\0';
		line++;

		while(*line++ != '"');
		p = shadow;
		while(*line != '"') *p++ = *line++;
		*p = '\0';
		line++;

		sscanf(line, "%f %f %f %f %d %d %d %d %d",
			&effect->light.dist,
			&effect->light.range,
			&effect->light.size,
			&effect->light.shadowSize,
			&shadowIntens, &lightType, &roadReflection, &flare, &flags);
		effect->light.corona = RwTextureRead(corona, nil);
		effect->light.shadow = RwTextureRead(shadow, nil);
		effect->light.shadowIntensity = shadowIntens;
		effect->light.lightType = lightType;
		effect->light.roadReflection = roadReflection;
		effect->light.flareType = flare;

		if(flags & LIGHTFLAG_FOG_ALWAYS)
			flags &= ~LIGHTFLAG_FOG_NORMAL;
		effect->light.flags = flags;
		break;

	case EFFECT_PARTICLE:
		sscanf(line, "%d %f %f %f %d %d %d %d %d %d %f %f %f %f",
			&id, &x, &y, &z, &r, &g, &b, &a, &type,
			&effect->particle.particleType,
			&effect->particle.dir.x,
			&effect->particle.dir.y,
			&effect->particle.dir.z,
			&effect->particle.scale);
		break;

	case EFFECT_ATTRACTOR:
		sscanf(line, "%d %f %f %f %d %d %d %d %d %d %f %f %f %d",
			&id, &x, &y, &z, &r, &g, &b, &a, &type,
			&flags,
			&effect->attractor.dir.x,
			&effect->attractor.dir.y,
			&effect->attractor.dir.z,
			&probability);
		effect->attractor.type = flags;
#ifdef FIX_BUGS
		effect->attractor.probability = Clamp(probability, 0, 255);
#else
		effect->attractor.probability = probability;
#endif
		break;
	case EFFECT_PED_ATTRACTOR:
		sscanf(line, "%d %f %f %f %d %d %d %d %d %d %f %f %f %f %f %f",
			&id, &x, &y, &z, &r, &g, &b, &a, &type,
			&ptype,
			&effect->pedattr.queueDir.x,
			&effect->pedattr.queueDir.y,
			&effect->pedattr.queueDir.z,
			&effect->pedattr.useDir.x,
			&effect->pedattr.useDir.y,
			&effect->pedattr.useDir.z);
		effect->pedattr.type = ptype;
		break;
	}

	CTxdStore::PopCurrentTxd();
}

void
CFileLoader::LoadScene(const char *filename)
{
	enum {
		NONE,
		INST,
		ZONE,
		CULL,
		OCCL,
		PICK,
		PATH,
	};
	char *line;
	int fd;
	int section;
	int pathType, pathIndex;

	section = NONE;
	pathIndex = -1;
	debug("Creating objects from %s...\n", filename);

	fd = CFileMgr::OpenFile(filename, "rb");
	assert(fd > 0);
	for(line = CFileLoader::LoadLine(fd); line; line = CFileLoader::LoadLine(fd)){
		if(*line == '\0' || *line == '#')
			continue;

		if(section == NONE){
			if(isLine4(line, 'i','n','s','t')) section = INST;
			else if(isLine4(line, 'z','o','n','e')) section = ZONE;
			else if(isLine4(line, 'c','u','l','l')) section = CULL;
			else if(isLine4(line, 'p','i','c','k')) section = PICK;
			else if(isLine4(line, 'p','a','t','h')) section = PATH;
			else if(isLine4(line, 'o','c','c','l')) section = OCCL;
		}else if(isLine3(line, 'e','n','d')){
			section = NONE;
		}else switch(section){
		case INST:
			LoadObjectInstance(line);
			break;
		case ZONE:
			LoadZone(line);
			break;
		case CULL:
			LoadCullZone(line);
			break;
		case OCCL:
			LoadOcclusionVolume(line);
			break;
		case PICK:
			// unused
			LoadPickup(line);
			break;
		case PATH:
			if(pathIndex == -1){
				LoadPathHeader(line, pathType);
				pathIndex = 0;
			}else{
				if(pathType == 0)
					LoadPedPathNode(line, -1, pathIndex);
				else if (pathType == 1)
					LoadCarPathNode(line, -1, pathIndex, false);
				else if (pathType == 2)
					LoadCarPathNode(line, -1, pathIndex, true);
				pathIndex++;
				if(pathIndex == 12)
					pathIndex = -1;
			}
			break;
		}
	}
	CFileMgr::CloseFile(fd);

	debug("Finished loading IPL\n");
}

void
CFileLoader::LoadObjectInstance(const char *line)
{
	int id;
	char name[24];
	RwV3d trans, scale, axis;
	float angle;
	CSimpleModelInfo *mi;
	RwMatrix *xform;
	CEntity *entity;
	float area;

	if(sscanf(line, "%d %s %f %f %f %f %f %f %f %f %f %f %f",
	          &id, name, &area,
	          &trans.x, &trans.y, &trans.z,
	          &scale.x, &scale.y, &scale.z,
	          &axis.x, &axis.y, &axis.z, &angle) != 13){
		if(sscanf(line, "%d %s %f %f %f %f %f %f %f %f %f %f",
		          &id, name,
		          &trans.x, &trans.y, &trans.z,
		          &scale.x, &scale.y, &scale.z,
		          &axis.x, &axis.y, &axis.z, &angle) != 12)
			return;
		area = 0;
	}

	mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(id);
	if(mi == nil)
		return;
	assert(mi->IsSimple());

	if(!CStreaming::IsObjectInCdImage(id))
		debug("Not in cdimage %s\n", mi->GetModelName());

	angle = -RADTODEG(2.0f * Acos(angle));
	xform = RwMatrixCreate();
	RwMatrixRotate(xform, &axis, angle, rwCOMBINEREPLACE);
	RwMatrixTranslate(xform, &trans, rwCOMBINEPOSTCONCAT);

	if(mi->GetObjectID() == -1){
		if(ThePaths.IsPathObject(id)){
			entity = new CTreadable;
			ThePaths.RegisterMapObject((CTreadable*)entity);
		}else
			entity = new CBuilding;
		entity->SetModelIndexNoCreate(id);
		entity->GetMatrix() = CMatrix(xform);
		entity->m_level = CTheZones::GetLevelFromPosition(&entity->GetPosition());
		entity->m_area = area;
		if(mi->IsBuilding()){
			if(mi->m_isBigBuilding)
				entity->SetupBigBuilding();
			if(mi->m_isSubway)
				entity->bIsSubway = true;
		}
		if(mi->GetLargestLodDistance() < 2.0f)
			entity->bIsVisible = false;
		CWorld::Add(entity);

		CColModel *col = entity->GetColModel();
		if(col->numSpheres || col->numBoxes || col->numTriangles){
			if(col->level != 0)
				CColStore::GetBoundingBox(col->level).ContainRect(entity->GetBoundRect());
		}else
			entity->bUsesCollision = false;

		if(entity->GetPosition().z + col->boundingBox.min.z < 6.0f)
			entity->bUnderwater = true;
	}else{
		entity = new CDummyObject;
		entity->SetModelIndexNoCreate(id);
		entity->GetMatrix() = CMatrix(xform);
		CWorld::Add(entity);
		if(IsGlass(entity->GetModelIndex()) && !mi->m_isArtistGlass)
			entity->bIsVisible = false;
		entity->m_level = CTheZones::GetLevelFromPosition(&entity->GetPosition());
		entity->m_area = area;
	}

	RwMatrixDestroy(xform);
}

void
CFileLoader::LoadZone(const char *line)
{
	char name[24];
	int type, level;
	float minx, miny, minz;
	float maxx, maxy, maxz;

	if(sscanf(line, "%s %d %f %f %f %f %f %f %d", name, &type, &minx, &miny, &minz, &maxx, &maxy, &maxz, &level) == 9)
		CTheZones::CreateZone(name, (eZoneType)type, minx, miny, minz, maxx, maxy, maxz, (eLevelName)level);
}

void
CFileLoader::LoadCullZone(const char *line)
{
	CVector pos;
	float minx, miny, minz;
	float maxx, maxy, maxz;
	int flags;
	int wantedLevelDrop = 0;

	sscanf(line, "%f %f %f %f %f %f %f %f %f %d %d",
		&pos.x, &pos.y, &pos.z,
		&minx, &miny, &minz,
		&maxx, &maxy, &maxz,
		&flags, &wantedLevelDrop);
	CCullZones::AddCullZone(pos, minx, maxx, miny, maxy, minz, maxz, flags, wantedLevelDrop);
}

// unused
void
CFileLoader::LoadPickup(const char *line)
{
	int id;
	float x, y, z;

	sscanf(line, "%d %f %f %f", &id, &x, &y, &z);
}

void
CFileLoader::LoadOcclusionVolume(const char *line)
{
	float x, y, z;
	float width, length, height;
	float angle;

	sscanf(line, "%f %f %f %f %f %f %f",
		&x, &y, &z,
		&width, &length, &height,
		&angle);
	COcclusion::AddOne(x, y, z + height/2.0f, width, length, height, angle);
}


// unused
void
CFileLoader::ReloadPaths(const char *filename)
{
	enum {
		NONE,
		PATH,
	};
	char *line;
	int section = NONE;
	int id, pathType, pathIndex = -1;
	debug("Reloading paths from %s...\n", filename);

	int fd = CFileMgr::OpenFile(filename, "r");
	assert(fd > 0);
	for (line = CFileLoader::LoadLine(fd); line; line = CFileLoader::LoadLine(fd)) {
		if (*line == '\0' || *line == '#')
			continue;

		if (section == NONE) {
			if (isLine4(line, 'p','a','t','h')) {
				section = PATH;
				ThePaths.AllocatePathFindInfoMem(4500);
			}
		} else if (isLine3(line, 'e','n','d')) {
			section = NONE;
		} else {
			switch (section) {
				case PATH:
					if (pathIndex == -1) {
						id = LoadPathHeader(line, pathType);
						pathIndex = 0;
					} else {
						if(pathType == 0)
							LoadPedPathNode(line, id, pathIndex);
						else if (pathType == 1)
							LoadCarPathNode(line, id, pathIndex, false);
						else if (pathType == 2)
							LoadCarPathNode(line, id, pathIndex, true);
						pathIndex++;
						if (pathIndex == 12)
							pathIndex = -1;
					}
					break;
				default:
					break;
			}
		}
	}
	CFileMgr::CloseFile(fd);
}

void
CFileLoader::ReloadObjectTypes(const char *filename)
{
	enum {
		NONE,
		OBJS,
		TOBJ,
		TWODFX
	};
	char *line;
	int section = NONE;
	CModelInfo::ReInit2dEffects();
	debug("Reloading object types from %s...\n", filename);

	CFileMgr::ChangeDir("\\DATA\\MAPS\\");
	int fd = CFileMgr::OpenFile(filename, "r");
	assert(fd > 0);
	CFileMgr::ChangeDir("\\");
	for (line = CFileLoader::LoadLine(fd); line; line = CFileLoader::LoadLine(fd)) {
		if (*line == '\0' || *line == '#')
			continue;

		if (section == NONE) {
			if (isLine4(line, 'o','b','j','s')) section = OBJS;
			else if (isLine4(line, 't','o','b','j')) section = TOBJ;
			else if (isLine4(line, '2','d','f','x')) section = TWODFX;
		} else if (isLine3(line, 'e','n','d')) {
			section = NONE;
		} else {
			switch (section) {
				case OBJS:
				case TOBJ:
					ReloadObject(line);
					break;
				case TWODFX:
					Load2dEffect(line);
					break;
				default:
					break;
			}
		}
	}
	CFileMgr::CloseFile(fd);
}

void
CFileLoader::ReloadObject(const char *line)
{
	int id, numObjs;
	char model[24], txd[24];
	float dist[3];
	uint32 flags;
	CSimpleModelInfo *mi;

	if(sscanf(line, "%d %s %s %d", &id, model, txd, &numObjs) != 4)
		return;

	switch(numObjs){
	case 1:
		sscanf(line, "%d %s %s %d %f %d",
			&id, model, txd, &numObjs, &dist[0], &flags);
		break;
	case 2:
		sscanf(line, "%d %s %s %d %f %f %d",
			&id, model, txd, &numObjs, &dist[0], &dist[1], &flags);
		break;
	case 3:
		sscanf(line, "%d %s %s %d %f %f %f %d",
			&id, model, txd, &numObjs, &dist[0], &dist[1], &dist[2], &flags);
		break;
	}

	mi = (CSimpleModelInfo*) CModelInfo::GetModelInfo(id);
	if (
#ifdef FIX_BUGS
		mi &&
#endif
	    mi->GetModelType() == MITYPE_SIMPLE && !strcmp(mi->GetModelName(), model) && mi->m_numAtomics == numObjs) {
		mi->SetLodDistances(dist);
		SetModelInfoFlags(mi, flags);
	} else {
		printf("Can't reload %s\n", model);
	}
}

// unused mobile function - crashes
void
CFileLoader::ReLoadScene(const char *filename)
{
	char *line;
	CFileMgr::ChangeDir("\\DATA\\");
	int fd = CFileMgr::OpenFile(filename, "r");
	assert(fd > 0);
	CFileMgr::ChangeDir("\\");

	for (line = CFileLoader::LoadLine(fd); line; line = CFileLoader::LoadLine(fd)) {
		if (*line == '#')
			continue;

		if (strncmp(line, "EXIT", 4) == 0)
			break;

		if (strncmp(line, "IDE", 3) == 0) {
			LoadObjectTypes(line + 4);
		}
	}
	CFileMgr::CloseFile(fd);
}
