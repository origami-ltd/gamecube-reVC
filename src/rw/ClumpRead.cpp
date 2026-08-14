#include "common.h"

struct rpGeometryList
{
	RpGeometry **geometries;
	int32 numGeoms;
};

struct rpAtomicBinary
{
	RwInt32 frameIndex;
	RwInt32 geomIndex;
	RwInt32 flags;
	RwInt32 unused;
};

static int32 numberGeometrys;
static uint32 streamPosition = UINT32_MAX;
static uint32 geometryListEnd;
static uint32 clumpEnd = UINT32_MAX;
static rpGeometryList gGeomList;
static rwFrameList gFrameList;
static RpClumpChunkInfo gClumpInfo;

void GeometryListDeinitialize(rpGeometryList *geomlist);
static void DestroyFrameList(rwFrameList *frameList);

static bool
FindChunkWithin(RwStream *stream, RwUInt32 type, RwUInt32 *size,
                RwUInt32 *version, uint32 end)
{
	uint32 position = STREAMPOS(stream);
	if(position == UINT32_MAX || position > end || end - position < 12 ||
	   !RwStreamFindChunk(stream, type, size, version))
		return false;
	position = STREAMPOS(stream);
	return position != UINT32_MAX && position <= end && *size <= end - position;
}

static void
InvalidateClumpStream(void)
{
	streamPosition = UINT32_MAX;
	geometryListEnd = 0;
	clumpEnd = UINT32_MAX;
	numberGeometrys = 0;
}

static bool
FailClumpStreamStart(void)
{
	GeometryListDeinitialize(&gGeomList);
	DestroyFrameList(&gFrameList);
	InvalidateClumpStream();
	return false;
}

static void
DestroyFrameList(rwFrameList *frameList)
{
	if(frameList->frames)
		for(int32 i = frameList->numFrames-1; i >= 0; i--)
			if(frameList->frames[i])
				RwFrameDestroy(frameList->frames[i]);
	rwFrameListDeinitialize(frameList);
	frameList->numFrames = 0;
}

rpGeometryList*
GeometryListStreamRead1(RwStream *stream, rpGeometryList *geomlist, uint32 listSize)
{
	int i;
	RwUInt32 size, version, geometrySize;
	RwInt32 numGeoms;

	numberGeometrys = 0;
	uint32 listStart = STREAMPOS(stream);
	if(listStart == UINT32_MAX || listSize < 16 || listSize > UINT32_MAX - listStart)
		return nil;
	geometryListEnd = listStart + listSize;
	if(!FindChunkWithin(stream, rwID_STRUCT, &size, &version, geometryListEnd))
		return nil;
	if(size != sizeof(numGeoms) || !ReadStreamLE32(stream, numGeoms))
		return nil;
	if(numGeoms < 0 || (uint32)numGeoms > (listSize - 16)/12 ||
	   (uint32)numGeoms > SIZE_MAX/sizeof(RpGeometry*))
		return nil;

	numberGeometrys = numGeoms/2;
	geomlist->numGeoms = numGeoms;
	if(geomlist->numGeoms > 0){
		geomlist->geometries = (RpGeometry**)RwMalloc(geomlist->numGeoms * sizeof(RpGeometry*));
		if(geomlist->geometries == nil){
			geomlist->numGeoms = 0;
			return nil;
		}
		memset(geomlist->geometries, 0, geomlist->numGeoms * sizeof(RpGeometry*));
	}else
		geomlist->geometries = nil;

	for(i = 0; i < numberGeometrys; i++){
		if(!FindChunkWithin(stream, rwID_GEOMETRY, &geometrySize, &version,
		                   geometryListEnd)){
			GeometryListDeinitialize(geomlist);
			return nil;
		}
		geomlist->geometries[i] = RpGeometryStreamRead(stream);
		if(geomlist->geometries[i] == nil){
			GeometryListDeinitialize(geomlist);
			return nil;
		}
		if(STREAMPOS(stream) > geometryListEnd){
			GeometryListDeinitialize(geomlist);
			return nil;
		}
	}

	return geomlist;
}

rpGeometryList*
GeometryListStreamRead2(RwStream *stream, rpGeometryList *geomlist)
{
	int i;
	RwUInt32 version, geometrySize;

	for(i = numberGeometrys; i < geomlist->numGeoms; i++){
		if(!FindChunkWithin(stream, rwID_GEOMETRY, &geometrySize, &version,
		                   geometryListEnd))
			return nil;
		geomlist->geometries[i] = RpGeometryStreamRead(stream);
		if(geomlist->geometries[i] == nil)
			return nil;
		if(STREAMPOS(stream) > geometryListEnd)
			return nil;
	}
	if(STREAMPOS(stream) != geometryListEnd)
		return nil;

	return geomlist;
}

void
GeometryListDeinitialize(rpGeometryList *geomlist)
{
	int i;

	if(geomlist->geometries)
		for(i = 0; i < geomlist->numGeoms; i++)
			if(geomlist->geometries[i])
				RpGeometryDestroy(geomlist->geometries[i]);

	if(geomlist->geometries){
		RwFree(geomlist->geometries);
		geomlist->geometries = nil;
	}
	geomlist->numGeoms = 0;
}

RpAtomic*
ClumpAtomicStreamRead(RwStream *stream, rwFrameList *frmList,
                      rpGeometryList *geomList, uint32 atomicEnd)
{
	RwUInt32 size, version;
	rpAtomicBinary a;
	uint8 data[sizeof(rpAtomicBinary)];
	RpAtomic *atomic;

	numberGeometrys = 0;
	if(!FindChunkWithin(stream, rwID_STRUCT, &size, &version, atomicEnd))
		return nil;
	if(version < 0x30400 || size != sizeof(rpAtomicBinary))
		return nil;
	if(RwStreamRead(stream, data, size) != size)
		return nil;
	a.frameIndex = (int32)ReadLE32(&data[0]);
	a.geomIndex = (int32)ReadLE32(&data[4]);
	a.flags = (int32)ReadLE32(&data[8]);
	a.unused = (int32)ReadLE32(&data[12]);
	if((frmList->numFrames > 0 && (a.frameIndex < 0 || a.frameIndex >= frmList->numFrames)) ||
	   (geomList->numGeoms > 0 && (a.geomIndex < 0 || a.geomIndex >= geomList->numGeoms)))
		return nil;

	atomic = RpAtomicCreate();
	if(atomic == nil)
		return nil;

	RpAtomicSetFlags(atomic, a.flags);

	if(frmList->numFrames){
		RpAtomicSetFrame(atomic, frmList->frames[a.frameIndex]);
	}

	if(geomList->numGeoms){
		RpAtomicSetGeometry(atomic, geomList->geometries[a.geomIndex], 0);
	}else{
		RpGeometry *geom;
		RwUInt32 geometrySize;
		if(!FindChunkWithin(stream, rwID_GEOMETRY, &geometrySize, &version,
		                   atomicEnd)){
			RpAtomicDestroy(atomic);
			return nil;
		}
		geom = RpGeometryStreamRead(stream);
		if(geom == nil || STREAMPOS(stream) == UINT32_MAX ||
		   STREAMPOS(stream) > atomicEnd){
			if(geom)
				RpGeometryDestroy(geom);
			RpAtomicDestroy(atomic);
			return nil;
		}
		RpAtomicSetGeometry(atomic, geom, 0);
		RpGeometryDestroy(geom);
	}

	return atomic;
}

bool
RpClumpGtaStreamRead1(RwStream *stream, uint32 payloadSize)
{
	RwUInt32 size, version;
	RwUInt32 expectedSize;
	RwUInt32 geometryListSize;
	RwUInt32 frameListSize;
	uint32 frameListEnd;
	uint32 frameListStart;
	uint8 data[sizeof(RpClumpChunkInfo)];
	GeometryListDeinitialize(&gGeomList);
	DestroyFrameList(&gFrameList);
	InvalidateClumpStream();

	if(!GetRwStreamEnd(stream, payloadSize, clumpEnd) ||
	   !FindChunkWithin(stream, rwID_STRUCT, &size, &version, clumpEnd))
		return FailClumpStreamStart();
	expectedSize = version >= 0x33000 ? 12 : 4;
	if(size != expectedSize)
		return FailClumpStreamStart();
	memset(&gClumpInfo, 0, sizeof(gClumpInfo));
	if(RwStreamRead(stream, data, size) != size)
		return FailClumpStreamStart();
	gClumpInfo.numAtomics = (int32)ReadLE32(&data[0]);
	if(size == sizeof(RpClumpChunkInfo)){
		gClumpInfo.numLights = (int32)ReadLE32(&data[4]);
		gClumpInfo.numCameras = (int32)ReadLE32(&data[8]);
	}
	if(gClumpInfo.numAtomics < 0 || gClumpInfo.numLights < 0 || gClumpInfo.numCameras < 0 ||
	   (uint64)(uint32)gClumpInfo.numAtomics + (uint32)gClumpInfo.numLights +
	   (uint32)gClumpInfo.numCameras > INT32_MAX)
		return FailClumpStreamStart();

	if(!FindChunkWithin(stream, rwID_FRAMELIST, &frameListSize, &version, clumpEnd))
		return FailClumpStreamStart();
	frameListStart = STREAMPOS(stream);
	frameListEnd = frameListStart + frameListSize;
	if(rwFrameListStreamRead(stream, &gFrameList) == nil)
		return FailClumpStreamStart();
	if(STREAMPOS(stream) != frameListEnd){
		return FailClumpStreamStart();
	}
	if(gFrameList.numFrames <= 0){
		return FailClumpStreamStart();
	}

	if(!FindChunkWithin(stream, rwID_GEOMETRYLIST, &geometryListSize, &version,
	                   clumpEnd)){
		return FailClumpStreamStart();
	}
	if(GeometryListStreamRead1(stream, &gGeomList, geometryListSize) == nil){
		return FailClumpStreamStart();
	}
	streamPosition = STREAMPOS(stream);
	if(streamPosition == UINT32_MAX || streamPosition > clumpEnd){
		return FailClumpStreamStart();
	}
	return true;
}

RpClump*
RpClumpGtaStreamRead2(RwStream *stream)
{
	int i;
	RwUInt32 version, atomicSize;
	uint32 atomicStart, atomicEnd;
	uint32 position = STREAMPOS(stream);
	RpAtomic *atomic;
	RpClump *clump;
	if(streamPosition == UINT32_MAX || clumpEnd == UINT32_MAX ||
	   position == UINT32_MAX || position > streamPosition)
		goto failBeforeClump;
	RwStreamSkip(stream, streamPosition - position);
	if(STREAMPOS(stream) != streamPosition)
		goto failBeforeClump;

	if(GeometryListStreamRead2(stream, &gGeomList) == nil)
		goto failBeforeClump;

	clump = RpClumpCreate();
	if(clump == nil)
		goto failBeforeClump;

	RpClumpSetFrame(clump, gFrameList.frames[0]);

	for(i = 0; i < gClumpInfo.numAtomics; i++){
		if(!FindChunkWithin(stream, rwID_ATOMIC, &atomicSize, &version, clumpEnd))
			goto failAfterClump;
		atomicStart = STREAMPOS(stream);
		atomicEnd = atomicStart + atomicSize;

		atomic = ClumpAtomicStreamRead(stream, &gFrameList, &gGeomList, atomicEnd);
		position = STREAMPOS(stream);
		if(atomic == nil || position == UINT32_MAX || position > atomicEnd){
			if(atomic)
				RpAtomicDestroy(atomic);
			goto failAfterClump;
		}

		RpClumpAddAtomic(clump, atomic);
	}

	GeometryListDeinitialize(&gGeomList);
	rwFrameListDeinitialize(&gFrameList);
	InvalidateClumpStream();
	return clump;

failAfterClump:
	GeometryListDeinitialize(&gGeomList);
	rwFrameListDeinitialize(&gFrameList);
	RpClumpDestroy(clump);
	InvalidateClumpStream();
	return nil;

failBeforeClump:
	GeometryListDeinitialize(&gGeomList);
	DestroyFrameList(&gFrameList);
	InvalidateClumpStream();
	return nil;
}

void
RpClumpGtaCancelStream(void)
{
	GeometryListDeinitialize(&gGeomList);
	DestroyFrameList(&gFrameList);
	InvalidateClumpStream();
}
