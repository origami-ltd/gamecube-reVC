#include "common.h"

#include "General.h"
#include "Pad.h"
#include "Hud.h"
#include "Text.h"
#include "Clock.h"
#include "Renderer.h"
#include "ModelInfo.h"
#include "TxdStore.h"
#include "ModelIndices.h"
#include "Pools.h"
#include "Wanted.h"
#include "Directory.h"
#include "RwHelper.h"
#include "World.h"
#include "Entity.h"
#include "FileMgr.h"
#include "FileLoader.h"
#include "Zones.h"
#include "Radar.h"
#include "Camera.h"
#include "Record.h"
#include "CarCtrl.h"
#include "Population.h"
#include "Gangs.h"
#include "CutsceneMgr.h"
#include "CdStream.h"
#include "Streaming.h"
#include "Replay.h"
#include "main.h"
#include "ColStore.h"
#include "DMAudio.h"
#include "Script.h"
#include "MemoryMgr.h"
#include "MemoryHeap.h"
#include "Font.h"
#include "Frontend.h"
#include "VarConsole.h"
#ifdef GTA_OGC
#include <malloc.h>
size_t gOgcHeapUsedAtInit;
#endif

#include <new>
extern "C" { extern volatile const char *gMainWhere; }   // gamecube.cpp watchdog checkpoint

bool CStreaming::ms_disableStreaming;
#ifdef GTA_OGC
// The staging pair is capped at 512K instead of sized to the largest file
// (2044K, one interior TXD, held for the whole run). A TXD bigger than the
// pair — 84 of them, every one an HD building or interior — streams through
// it: the first fill arrives on channel 0 like any load, and CdPullStream
// refills the pair in place from the disc as the dictionary parser consumes
// it. Texels go to ARAM chunk by chunk (gxraster.cpp readNativeTexture), so
// the file never exists whole in MEM1. Reads past the first fill are
// synchronous on the main thread, as the blocking loader's always were.
// ponytail: forward-only; the TXD reader never seeks back. No DFF is oversize.
enum { STREAM_BUFFER_CAP_SECTORS = 256 };         // 512K total, 256K a channel
static uint32 gPullCd;   // cd sector of the file whose first fill is in the pair
class CdPullStream : public rw::Stream
{
	uint8 *pair, *win;
	uint32 pairBytes, have, base, pos, fileBytes, sector;
	int failed;
	bool fill(void){
		if(failed || base + have >= fileBytes) return false;
		base += have;
		uint32 n = fileBytes - base;
		if(n > pairBytes) n = pairBytes;
		uint32 sectors = (n + CDSTREAM_SECTOR_SIZE-1)/CDSTREAM_SECTOR_SIZE;
		gMainWhere = "pull-read";
		int tries = 0, status;
		do{
			if(++tries > 8){ failed = 1; return false; }
			status = CdStreamRead(0, pair, sector, sectors);
		}while(CdStreamSync(0) || status == STREAM_NONE);
		gMainWhere = "pull-parse";
		win = pair; sector += sectors; have = n;
		return true;
	}
public:
	// first: where the file's first fill sits inside the pair
	void init(uint8 *pairStart, uint32 pairSize, uint8 *first, uint32 cdSector, uint32 total){
		pair = pairStart; pairBytes = pairSize; win = first; fileBytes = total;
		have = pairSize - (uint32)(first - pairStart);
		if(have > total) have = total;
		base = 0; pos = 0; failed = 0;
		sector = cdSector + (have + CDSTREAM_SECTOR_SIZE-1)/CDSTREAM_SECTOR_SIZE;
	}
	uint32 write8(const void*, uint32) { return 0; }
	uint32 read8(void *data, uint32 length){
		uint8 *d = (uint8*)data;
		uint32 got = 0;
		while(length){
			if(pos >= base + have && !fill()) break;
			uint32 n = base + have - pos;
			if(n > length) n = length;
			memcpy(d, win + (pos - base), n);
			d += n; pos += n; length -= n; got += n;
		}
		return got;
	}
	void seek(int32 offset, int32 whence = 1){
		uint32 np = whence == 0 ? (uint32)offset : whence == 1 ? pos + offset : fileBytes - offset;
		if(np < base){ failed = 1; printf("STREAM pull: backward seek %u < %u\n", np, base); return; }
		pos = np;   // forward: read8 refills through whatever is skipped
	}
	uint32 tell(void) { return pos; }
	bool eof(void) { return failed || pos >= fileBytes; }
};

// Hard floor on the real heap. The budget only counts what the streamer
// itself loaded; entity clones, particles and RW frames allocate past it,
// and B17 died on a 1K must-allocate at 400K free. Sampled once a frame in
// Update; below it the per-frame loader issues no reads and sheds one
// unreferenced model a frame. The blocking loader is not gated.
enum { STREAM_HEAP_FLOOR = 512*1024 };   // B87: general heap holds fewer big blocks now   // B76: back to 640K. 1MB (B73) + the 512K reserve left ~50K of headroom: near set stuck at 2/16, black docks
enum { STREAM_BUDGET_CAP = 0 };   // B27 tried 2MB: the budget is soft, str stayed at 8-10MB and the churn sharpened
// Just-loaded protection (B59): the blocking loader evicted what it had just
// loaded (farthest from the old camera) while loading the rest — the first
// cutscene started empty. A model loaded under 5s ago is not evictable.
// B114: FRAMES, not CTimer seconds. The clock is frozen through a blocking
// load and the first Update then adds the whole load (38.8 s at New Game),
// so every model of the scene lost its grace on the same frame: evict +33 at
// 5/s right after the load, near 38/84 -> 1/17, "geometry lost when the
// loading finishes". Frames do not advance while main is inside a load.
static uint16 gLoadFrame[NUMSTREAMINFO];   // B106: TXDs too — 'STREAM rm txd icons9' every 500 ms was a TXD dropped and re-pulled with no grace
static inline void StampLoaded(int32 id)
{
	if(id < 0 || id >= NUMSTREAMINFO) return;
	uint16 f = (uint16)CTimer::GetFrameCounter();
	gLoadFrame[id] = f ? f : 1;
}
static inline bool LoadedRecently(int32 id)
{
	if(id < 0 || id >= NUMSTREAMINFO || gLoadFrame[id] == 0) return false;
	return (uint16)((uint16)CTimer::GetFrameCounter() - gLoadFrame[id]) < 300;   // ~5 s at 60 fps
}
static int gEvictTrace = 300;   // B58: the first evictions of the run, named
static void
TraceEvict(int32 id)
{
	if(gEvictTrace <= 0 || id < 0 || id >= MODELINFOSIZE) return;
	gEvictTrace--;
	printf("EVICT %d %s\n", id, CModelInfo::GetModelInfo(id)->GetModelName());
}

static bool
HasPendingLods(void)
{
	for(CStreamingInfo *si = CStreaming::ms_startRequestedList.m_next;
	    si && si != &CStreaming::ms_endRequestedList; si = si->m_next)
		if(si->m_flags & STREAMFLAGS_LOD)
			return true;
	for(int ch = 0; ch < 2; ch++){
		if(CStreaming::ms_channel[ch].state == CHANNELSTATE_IDLE) continue;
		for(int i = 0; i < 4; i++){
			int id = CStreaming::ms_channel[ch].streamIds[i];
			if(id < 0) continue;
			CStreamingInfo &si = CStreaming::ms_aInfoForModel[id];
			if((si.m_flags & STREAMFLAGS_LOD) &&
			   (si.m_loadState == STREAMSTATE_READING || si.m_loadState == STREAMSTATE_STARTED))
				return true;
		}
	}
	return false;
}

static bool
LodPassAllows(int32 id)
{
	return (CStreaming::ms_aInfoForModel[id].m_flags & STREAMFLAGS_LOD) ||
	    (id >= STREAM_OFFSET_COL && id < STREAM_OFFSET_ANIM);
}

#define STREAM_FLOOR_ENABLED 1
static size_t gStreamHeapFree = STREAM_HEAP_FLOOR;
#endif

extern "C" int gcBootDone(void);   // gamecube.cpp: 1 from the first GS_PLAYING_GAME frame (B144)
volatile int gcEssentialLoad;   // B150: read by gcArenaCarve (gamecube.cpp)
static uint8 gLoadFails[NUMSTREAMINFO];   // bounded retry counters, see FailedLoad
static uint8 gFailSec[NUMSTREAMINFO];     // B133: second of the last failure; the cap is a 60 s cooldown, not a life sentence
static uint32 gSpecialWaitFrame;          // B142: frame of the last HasSpecialCharLoaded miss (the script is waiting)
static const char *gFailWhy = "?";   // last reason handed to FailedLoad
extern uint32 gStrEvict;
extern "C" { extern void *gHeapReserve; extern volatile unsigned gHeapEmergency; void gcHeapReserveArm(void); }   // gamecube.cpp heap reserve (B73)
extern "C" size_t gcHeapFreeTotal(void);   // B79: general free + big-block chunk free
// B107: the honest budget in one place. What the streamer holds plus what it
// can still get: the general heap above its slack, and — now that every RW
// block of 1K+ lives in the permanent 2MB chunks (B106) — the chunk room in
// full. Counting only fordblks (B51) left 139-578K of headroom against a
// 7-8MB set, so MakeSpaceFor evicted on nearly every load; Init2's boot value
// (1831K) was never refreshed during the blocking New Game loads, so the
// scene evicted its own models (near 38/84 at the first census).
static void
HonestBudget(void)
{
	size_t general = mallinfo().fordblks;
	size_t total = gcHeapFreeTotal();
	size_t chunkFree = total > general ? total - general : 0;
	size_t slack = STREAM_HEAP_FLOOR + 256*1024;
	size_t head = (general > slack ? general - slack : 0) + chunkFree;
	CStreaming::ms_memoryAvailable = CStreaming::ms_memoryUsed + head;
	if(CStreaming::ms_memoryAvailable < 4*1024*1024) CStreaming::ms_memoryAvailable = 4*1024*1024;
}
extern unsigned gxAramBytes, gxAramStoreBytes;   // gxraster.cpp texel store
bool CStreaming::ms_bLoadingBigModel;
int32 CStreaming::ms_numModelsRequested;
CStreamingInfo CStreaming::ms_aInfoForModel[NUMSTREAMINFO];
CStreamingInfo CStreaming::ms_startLoadedList;
CStreamingInfo CStreaming::ms_endLoadedList;
CStreamingInfo CStreaming::ms_startRequestedList;
CStreamingInfo CStreaming::ms_endRequestedList;
int32 CStreaming::ms_oldSectorX;
int32 CStreaming::ms_oldSectorY;
int32 CStreaming::ms_streamingBufferSize;
#ifndef ONE_THREAD_PER_CHANNEL
int8 *CStreaming::ms_pStreamingBuffer[2];
#else
int8 *CStreaming::ms_pStreamingBuffer[4];
#endif
size_t CStreaming::ms_memoryUsed;
CStreamingChannel CStreaming::ms_channel[2];
int32 CStreaming::ms_channelError;
int32 CStreaming::ms_numVehiclesLoaded;
int32 CStreaming::ms_numPedsLoaded;
int32 CStreaming::ms_vehiclesLoaded[MAXVEHICLESLOADED];
int32 CStreaming::ms_lastVehicleDeleted;
bool CStreaming::ms_bIsPedFromPedGroupLoaded[NUMMODELSPERPEDGROUP];
CDirectory *CStreaming::ms_pExtraObjectsDir;
int32 CStreaming::ms_numPriorityRequests;
int32 CStreaming::ms_currentPedGrp;
int32 CStreaming::ms_currentPedLoading;
int32 CStreaming::ms_lastCullZone;
uint16 CStreaming::ms_loadedGangs;
uint16 CStreaming::ms_loadedGangCars;
int32 CStreaming::ms_imageOffsets[NUMCDIMAGES];
int32 CStreaming::ms_lastImageRead;
int32 CStreaming::ms_imageSize;
size_t CStreaming::ms_memoryAvailable;

int32 desiredNumVehiclesLoaded = 12;

CEntity *pIslandLODmainlandEntity;
CEntity *pIslandLODbeachEntity;
int32 islandLODmainland;
int32 islandLODbeach;

#ifndef MASTER
bool gbPrintStats;
bool gbPrintVehiclesInMemory;  // TODO
bool gbPrintStreamingBuffer; // TODO
#endif

bool
CStreamingInfo::GetCdPosnAndSize(uint32 &posn, uint32 &size)
{
	if(m_size == 0)
		return false;
	posn = m_position;
	size = m_size;
	return true;
}

void
CStreamingInfo::SetCdPosnAndSize(uint32 posn, uint32 size)
{
	m_position = posn;
	m_size = size;
}

void
CStreamingInfo::AddToList(CStreamingInfo *link)
{
	// Insert this after link
	m_next = link->m_next;
	m_prev = link;
	link->m_next = this;
	m_next->m_prev = this;
}

void
CStreamingInfo::RemoveFromList(void)
{
	m_next->m_prev = m_prev;
	m_prev->m_next = m_next;
	m_next = nil;
	m_prev = nil;
}

bool
CStreaming::Init2(void)
{
	int i;

	for(i = 0; i < NUMSTREAMINFO; i++){
		ms_aInfoForModel[i].m_loadState = STREAMSTATE_NOTLOADED;
		ms_aInfoForModel[i].m_next = nil;
		ms_aInfoForModel[i].m_prev = nil;
		ms_aInfoForModel[i].m_nextID = -1;
		ms_aInfoForModel[i].m_size = 0;
		ms_aInfoForModel[i].m_position = 0;
	}

	ms_channelError = -1;

	// init lists

	ms_startLoadedList.m_next = &ms_endLoadedList;
	ms_startLoadedList.m_prev = nil;
	ms_endLoadedList.m_prev = &ms_startLoadedList;
	ms_endLoadedList.m_next = nil;

	ms_startRequestedList.m_next = &ms_endRequestedList;
	ms_startRequestedList.m_prev = nil;
	ms_endRequestedList.m_prev = &ms_startRequestedList;
	ms_endRequestedList.m_next = nil;

	// init misc

	ms_oldSectorX = 0;
	ms_oldSectorY = 0;
	ms_streamingBufferSize = 0;
	ms_disableStreaming = false;
	ms_memoryUsed = 0;
	ms_bLoadingBigModel = false;

	// init channels

	ms_channel[0].state = CHANNELSTATE_IDLE;
	ms_channel[1].state = CHANNELSTATE_IDLE;
	for(i = 0; i < 4; i++){
		ms_channel[0].streamIds[i] = -1;
		ms_channel[0].offsets[i] = -1;
		ms_channel[1].streamIds[i] = -1;
		ms_channel[1].offsets[i] = -1;
	}

	// init stream info, mark things that are already loaded

	for(i = 0; i < MODELINFOSIZE; i++){
		CBaseModelInfo *mi = CModelInfo::GetModelInfo(i);
		if(mi && mi->GetRwObject()){
			ms_aInfoForModel[i].m_loadState = STREAMSTATE_LOADED;
			ms_aInfoForModel[i].m_flags = STREAMFLAGS_DONT_REMOVE;
			if(mi->IsSimple())
				((CSimpleModelInfo*)mi)->m_alpha = 255;
		}
	}

	for(i = 0; i < TXDSTORESIZE; i++)
		if(CTxdStore::GetSlot(i) && CTxdStore::GetSlot(i)->texDict)
			ms_aInfoForModel[i + STREAM_OFFSET_TXD].m_loadState = STREAMSTATE_LOADED;


	for(i = 0; i < MAXVEHICLESLOADED; i++)
		ms_vehiclesLoaded[i] = -1;
	ms_numVehiclesLoaded = 0;
	ms_numPedsLoaded = 8;

	for(i = 0; i < ARRAY_SIZE(ms_bIsPedFromPedGroupLoaded); i++)
		ms_bIsPedFromPedGroupLoaded[i] = false;

	ms_pExtraObjectsDir = new(std::nothrow) CDirectory(EXTRADIRSIZE);
	if(ms_pExtraObjectsDir == nil || !ms_pExtraObjectsDir->IsValid()){
		delete ms_pExtraObjectsDir;
		ms_pExtraObjectsDir = nil;
		return false;
	}
	ms_numPriorityRequests = 0;
	ms_currentPedGrp = -1;
	ms_lastCullZone = -1;		// unused because RemoveModelsNotVisibleFromCullzone is gone
	ms_loadedGangs = 0;
	ms_currentPedLoading = NUMMODELSPERPEDGROUP;	// unused, whatever it is

	if(!LoadCdDirectory()){
		delete ms_pExtraObjectsDir;
		ms_pExtraObjectsDir = nil;
		return false;
	}
	if(ms_streamingBufferSize <= 0){
		delete ms_pExtraObjectsDir;
		ms_pExtraObjectsDir = nil;
		return false;
	}
	size_t roundedStreamingBufferSize = (size_t)ms_streamingBufferSize;
	if(roundedStreamingBufferSize & 1)
		roundedStreamingBufferSize++;
#ifdef GTA_OGC
	// See StreamTarget: the buffer no longer has to hold the largest file.
	if(roundedStreamingBufferSize > STREAM_BUFFER_CAP_SECTORS)
		roundedStreamingBufferSize = STREAM_BUFFER_CAP_SECTORS;
#endif
	size_t allocationMultiplier = 1;
#ifdef ONE_THREAD_PER_CHANNEL
	allocationMultiplier = 2;
#endif
	if(roundedStreamingBufferSize > SIZE_MAX/CDSTREAM_SECTOR_SIZE/allocationMultiplier){
		delete ms_pExtraObjectsDir;
		ms_pExtraObjectsDir = nil;
		return false;
	}

	// allocate streaming buffers
	ms_streamingBufferSize = (int32)roundedStreamingBufferSize;
#ifndef ONE_THREAD_PER_CHANNEL
	ms_pStreamingBuffer[0] = (int8*)RwMallocAlign((size_t)ms_streamingBufferSize*CDSTREAM_SECTOR_SIZE, CDSTREAM_SECTOR_SIZE);
	if(ms_pStreamingBuffer[0] == nil){
		delete ms_pExtraObjectsDir;
		ms_pExtraObjectsDir = nil;
		return false;
	}
	ms_streamingBufferSize /= 2;
	ms_pStreamingBuffer[1] = ms_pStreamingBuffer[0] + (size_t)ms_streamingBufferSize*CDSTREAM_SECTOR_SIZE;
	printf("STREAM buffer: %d sectors a channel at %p\n", ms_streamingBufferSize, ms_pStreamingBuffer[0]);
#else
	ms_pStreamingBuffer[0] = (int8*)RwMallocAlign((size_t)ms_streamingBufferSize*2*CDSTREAM_SECTOR_SIZE, CDSTREAM_SECTOR_SIZE);
	if(ms_pStreamingBuffer[0] == nil){
		delete ms_pExtraObjectsDir;
		ms_pExtraObjectsDir = nil;
		return false;
	}
	ms_streamingBufferSize /= 2;
	ms_pStreamingBuffer[1] = ms_pStreamingBuffer[0] + (size_t)ms_streamingBufferSize*CDSTREAM_SECTOR_SIZE;
	printf("STREAM buffer: %d sectors a channel at %p\n", ms_streamingBufferSize, ms_pStreamingBuffer[0]);
	ms_pStreamingBuffer[2] = ms_pStreamingBuffer[1] + (size_t)ms_streamingBufferSize*CDSTREAM_SECTOR_SIZE;
	ms_pStreamingBuffer[3] = ms_pStreamingBuffer[2] + (size_t)ms_streamingBufferSize*CDSTREAM_SECTOR_SIZE;
#endif
	debug("Streaming buffer size is %d sectors", ms_streamingBufferSize);

	// PC only, figure out how much memory we got
#ifdef GTA_PC
#define MB (1024*1024)
#if defined(GTA_OGC)
	// The PC formula floors at 65MB — an impossible promise against a 24MB
	// console; streaming would keep loading until allocation fails. Derive
	// the budget from the real arena instead and keep few cached vehicles.
	{
		extern size_t _dwMemAvailPhys;
		// GX rasters are 16-bit tiled (RGB5A3), so resident texture cost
		// tracks the on-disc accounting closely enough for this to hold.
		// ponytail: 6MB reserve — 8MB left the budget == resident world
		// (7.1MB budget vs 6.8MB world at spawn), so every near vehicle/
		// ped/tree model churned in and out each frame (visible flicker).
		// Texture/geometry allocs soft-fail now, so an undersized reserve
		// degrades to a skipped stream load, not exit(1).
		// 8MB reserve. Trimming this to 5-6MB to buy draw distance put the
		// arena on a knife edge: some boots OOM'd outright (exit 1), and
		// the ones that survived failed texture allocations instead, so
		// gxGetTexture returned nil and the world rendered flat white.
		// Stability first — this is the value the port shipped with.
		// 8MB — the value with the most verified-good runs. Raising it to
		// 10MB only moved the free-bytes-at-crash from 2.2MB to 3.5MB
		// without preventing the crash, which says the failing allocation
		// is a large one (Geometry::create realloc) hitting fragmentation,
		// not a shortage of total free memory. Reserve tuning cannot fix
		// that; smaller resident textures or real accounting can.
		// 4MB, down from 8MB. The 8MB was picked while a failed allocation
		// killed the process, so it bought crash-avoidance at the cost of
		// half the arena. Both reasons are gone: Geometry::create now fails
		// softly, and gxGetTexture no longer rebuilds already-built textures
		// from a fabricated staging buffer (that leak is what made resident
		// cost look ~1.85x its accounted size).
		//
		// The budget is the LOD bug. VC map objects have exactly one atomic
		// each (verified across all 3511 entries in the IDEs), so LOD is not
		// per-atomic — it is separate LODxxx entities with 700-1200 draw
		// distances against 70-140 for the detailed model. Look at
		// SetupBigBuildingVisibility: the far LOD is only hidden when the
		// detailed model is resident and fully faded in. Starve streaming and
		// the detailed model never survives, so the world is drawn entirely
		// from LODs and every reload shows up as flicker. Give streaming a
		// real budget and both symptoms go with it.
		// 6MB. Measured either side of this: at 8MB the streamer only reached
		// 7238K and detailed buildings never stayed resident, so the world
		// drew from LODs and flickered. At 4MB it reached ~11450K accounted,
		// which measured as ~14MB really used out of a 15620K arena — about
		// 1.5MB spare, not enough for a burst like skipping a cutscene, and
		// the game froze there. 6MB puts the accounted cap near 9.5MB and
		// leaves roughly 3.5MB of real headroom for transients.
		// 4MB, down from 6MB — and this is the trade the handoff says not to
		// make, so here is why the precondition now holds. Cutting the reserve
		// is only dangerous when it takes real headroom the exterior needs.
		// gxPackGeometry now quantises every streamed geometry's positions and
		// texcoords to int16 and frees the float arrays, and that reclaim was
		// measured in the same alley, textures resident, at the same 6MB
		// reserve: free bytes went 2890K -> 5157K. So 2.2MB of real headroom
		// exists that did not exist when 6MB was chosen, and spending 2MB of it
		// on budget leaves ~3.1MB free — the same order as the "roughly 3.5MB
		// for transients" the 6MB value was picked to provide.
		//
		// What this buys: arena 16408K at a 6MB reserve gave budget 10264K,
		// while measured strMem oscillated 9830-12570K. The streamer was
		// permanently AT OR ABOVE its cap, so MakeSpaceFor evicted on every
		// request and strReq never drained below 43. That constant churn is the
		// LOD fallback: SetupBigBuildingVisibility keeps the far shell whenever
		// the detailed model is not resident and faded in, and nothing stayed
		// resident. 4MB puts the budget at 12312K, above that working set.
		//
		// 2MB, down from 4MB, and this one is backed by the number the earlier
		// versions of this comment were guessing at. The stated reason not to
		// trust free bytes was fragmentation — 5423K spread over 12085 chunks
		// averages 460 bytes, so a big Geometry::create could fail with
		// megabytes free. mallinfo has no largest-block field, so that stayed
		// a fear rather than a measurement. The heartbeat now probes it
		// directly (malloc, halving down from 2MB, freed immediately) and the
		// answer at a 4MB reserve was **maxblk=2048K in every single sample** —
		// the probe's own ceiling, never once lower. The arena is not
		// fragmented in any way that matters; total free was the honest number
		// after all.
		//
		// What forces the spend: with the budget at 12300K and the player
		// standing still, the streamer logged ev=280 evictions against ld=325
		// loads every five seconds. Nearly every load costs a model, and the
		// evicted model is immediately re-requested. That is thrash, and it is
		// what leaves ~17 on-screen entities per frame holding a far LOD shell
		// instead of their detailed model.
		//
		// 6MB minus 256K, set deliberately and against one measurement, so the
		// measurement is written down rather than lost. Compared in the same
		// intro cutscene: at a 2MB reserve free bytes were 616K, at 6MB they
		// were 908K. Four megabytes of reserve bought under 300K of real
		// headroom, and at 6MB the characters rendered as black silhouettes
		// against a fully drawn background — their textures had stopped
		// fitting. So in that scene the reserve is not what consumes memory,
		// the cutscene's own working set is, and raising it is close to a no-op
		// on safety while costing visible detail.
		//
		// It is set high anyway because the audio backend is about to need
		// MEM1 that nothing currently accounts for: mixing buffers and sample
		// pages coming back from ARAM. Better to reserve for it now than to
		// discover it as another freeze.
		//
		// This is one number and it is the one to move when experimenting.
		// Measure any change in the intro cutscene, not in a stationary street:
		// an earlier revision measured 4652-5812K free in an alley, concluded
		// there was room to spare, cut this to 2MB and shipped a freeze.
		// A RESERVE again, and 2MB, because that is the shape and the value
		// that renders correctly. An earlier revision restated this as a direct
		// budget of 3MB, which sounds similar and is not remotely: a 2MB reserve
		// leaves the streamer about 14MB, so "budget = 3MB" cut resident world to
		// a fifth and broke the picture. Reserve is the leftover, budget is
		// arena minus it; do not swap the two again.
		//
		// This value is unreasonably sensitive — every move in either direction
		// has broken something (6MB: characters lost their textures and drew as
		// black silhouettes; 3MB budget: world stopped rendering; 2MB reserve
		// measured in an alley: froze in the cutscene). That fragility is itself
		// the bug and is not understood yet. It most likely means an allocation
		// failure path is being reached silently rather than that 2MB is magic.
		// Do not tune this again without first finding out why it is a cliff.
		size_t reserve = 2*MB; // engine late allocs, render targets
		// The cliff above is now understood. _dwMemAvailPhys is the WHOLE
		// arena, sampled at psInitialize before anything was allocated; by the
		// time this runs, pools, paths, collision and the engine already hold
		// most of it (measured: usedNow 15273K of a 16032K arena at this line).
		// Budgeting "arena minus reserve" then granted the streamer ~14MB on
		// top of a heap with ~200K left — 29MB of demand in 16MB, and the
		// overcommit is the OOM. Budget what is actually left, the way dca3 sizes its fixed
		// STREAMING_MEM_SIZE against a known resident set.
		extern size_t gOgcHeapUsedAtInit;
		struct mallinfo mi = mallinfo();
		gOgcHeapUsedAtInit = mi.uordblks;
		size_t left = _dwMemAvailPhys > (size_t)mi.uordblks ?
		    _dwMemAvailPhys - (size_t)mi.uordblks : 0;
		ms_memoryAvailable = left > reserve + 4*MB ? left - reserve : 4*MB;
		// User experiment 09-01 (B27): a hard 2MB streaming budget, with the
		// texels already out of MEM1 (ARAM tier). Governs geometry only now.
		if(STREAM_BUDGET_CAP && ms_memoryAvailable > STREAM_BUDGET_CAP)
			ms_memoryAvailable = STREAM_BUDGET_CAP;
		// With the texels in ARAM the heap left at init says nothing about the
		// geometry the world needs (B29: 4MB budget against a 8-10MB set, so
		// MakeSpaceFor looped on every load). The heap floor in Update is the
		// real limiter; this only decides when farthest-first eviction starts.
		// 12MB. B33 tried 10.5MB to pay for the audio and MakeSpaceFor looped
		// again (evict 1068 in 2.5 min, LOD blink back). The heap floor in
		// Update is what yields MEM1 to the audio: far models, one a frame.
		// 10MB (user, B37: "reduce the streaming memory, use the DVD more").
		// The near set is protected everywhere now, so the budget only sets how
		// far the HD world reaches before the LOD shells take over.
		if(ms_memoryAvailable < 10*MB)
			ms_memoryAvailable = left > 3*MB ? left - 1536*1024 : left/2;   // B94: the flat 10MB let the boot burst fill the heap to the last byte (B93 corrupted there); the cadence grows it honestly afterwards
		printf("STREAM budget: %uK (heap left %uK)\n", (unsigned)(ms_memoryAvailable/1024), (unsigned)(left/1024));
		gcHeapReserveArm();   // B77: arm while the heap is still wide open
		desiredNumVehiclesLoaded = 12; // the PS2's number — user rule 09-01: at least PS2 levels of cars and peds
	}
#elif defined(FIX_BUGS)
	// do what gta3 does
	extern size_t _dwMemAvailPhys;
	ms_memoryAvailable = (_dwMemAvailPhys - 10*MB)/2;
	if(ms_memoryAvailable < 65*MB)
		ms_memoryAvailable = 65*MB;
	desiredNumVehiclesLoaded = (int32)((ms_memoryAvailable / MB - 65) / 3 + 12);
	if(desiredNumVehiclesLoaded > MAXVEHICLESLOADED)
		desiredNumVehiclesLoaded = MAXVEHICLESLOADED;
#else
	ms_memoryAvailable = 65 * MB;
	desiredNumVehiclesLoaded = 25;
	debug("Memory allocated to Streaming is %zuMB", ms_memoryAvailable/MB); // original modifier was %d
#endif
#undef MB
#endif

	// find island LODs

	pIslandLODmainlandEntity = nil;
	pIslandLODbeachEntity = nil;
	islandLODmainland = -1;
	islandLODbeach = -1;
	CModelInfo::GetModelInfo("IslandLODmainland", &islandLODmainland);
	CModelInfo::GetModelInfo("IslandLODbeach", &islandLODbeach);

#ifndef MASTER
	VarConsole.Add("Streaming Debug", &gbPrintStats, true);
	VarConsole.Add("Streaming Vehicle Debug", &gbPrintVehiclesInMemory, true);
	VarConsole.Add("Printf Streaming Buffer contents", &gbPrintStreamingBuffer, true);
#endif
	return true;
}

bool
CStreaming::Init(void)
{
#ifdef USE_TXD_CDIMAGE
	if(!CanVideoCardDoDXT()){
		int txdHandle = CFileMgr::OpenFile("MODELS\\TXD.IMG", "r");
		if (txdHandle)
			CFileMgr::CloseFile(txdHandle);
		if (!CheckVideoCardCaps() && txdHandle) {
			if(!CdStreamAddImage("MODELS\\TXD.IMG") || !CStreaming::Init2())
				return false;
		} else {
			if(!CStreaming::Init2())
				return false;
			if (CreateTxdImageForVideoCard()) {
				CStreaming::Shutdown();
				if(!CdStreamAddImage("MODELS\\TXD.IMG") || !CStreaming::Init2())
					return false;
			}
		}
	} else
		return CStreaming::Init2();
	return true;
#else
	return CStreaming::Init2();
#endif
}

void
CStreaming::ReInit(void)
{
	int i;
	CStreaming::FlushRequestList();
	CStreaming::DeleteAllRwObjects();
	CStreaming::RemoveAllUnusedModels();
	for(i = 0; i < MODELINFOSIZE; i++)
		if(CModelInfo::GetModelInfo(i) && ms_aInfoForModel[i].m_flags & STREAMFLAGS_SCRIPTOWNED)
			SetMissionDoesntRequireModel(i);
	CStreaming::ms_disableStreaming = false;
}

void
CStreaming::Shutdown(void)
{
	if(ms_pStreamingBuffer[0])
		RwFreeAlign(ms_pStreamingBuffer[0]);
	for(int32 i = 0; i < ARRAY_SIZE(ms_pStreamingBuffer); i++)
		ms_pStreamingBuffer[i] = nil;
	ms_streamingBufferSize = 0;
	if(ms_pExtraObjectsDir) {
		delete ms_pExtraObjectsDir;
		ms_pExtraObjectsDir = nil;
	}
}

#ifndef MASTER
uint64 timeProcessingTXD;
uint64 timeProcessingDFF;
#endif

void
CStreaming::Update(void)
{
	CStreamingInfo *si, *prev;
	bool requestedSubway = false;

#ifndef MASTER
	timeProcessingTXD = 0;
	timeProcessingDFF = 0;
#endif

	UpdateMemoryUsed();
#ifdef GTA_OGC
	{
		extern unsigned rwGeoAllocFails;
		static unsigned compactedFailures;
		if(gcBootDone() && compactedFailures != rwGeoAllocFails){
			compactedFailures = rwGeoAllocFails;
			CGame::TidyUpMemory(false, false);
		}
	}
	// B177: the B20 ten-second memset of gLoadFails is gone. FailedLoad already
	// forgives a model 60 s after its last failure (B133); zeroing every 10 s
	// meant RequestModel's 60 s block never held, and b171 re-read nbt_hotel05
	// 14 times in 40 s (each read also shedding up to 24 models).
	gMainWhere = "stream-update";
	{
		// The heap walk (mallinfo is O(chunks)) every 8 frames.
		static uint32 cadence;
		cadence++;
		if((cadence & 7) == 0){
			gStreamHeapFree = mallinfo().fordblks; /* B85: the floor guards the GENERAL heap; chunk room does not serve a 3K malloc */
			// Honest budget (B51): what the streamer holds plus what the heap can
			// still give above the floor. MakeSpaceFor then evicts before an
			// allocation fails, instead of the floor shedding
			// after one did. A fixed 10MB overcommitted the heap by 1-3MB.
			HonestBudget();   // B107: general heap above slack + chunk room, see the helper
		}
	}
	// Heap emergency (B73): the reserve went out; shed hard until there is
	// room to re-arm it, and re-arm as soon as there is.
	if(gHeapEmergency){
		// B114: shed to the level that ENDS the emergency (floor+512K), not
		// 512K above it — free idles between the two, so the loop cycled.
		for(int k = 0; k < 8 && gStreamHeapFree < STREAM_HEAP_FLOOR + 512*1024; k++){
			if(!RemoveLeastUsedModel(0)) break;
			gStrEvict++;
			gStreamHeapFree = mallinfo().fordblks; /* B85: the floor guards the GENERAL heap; chunk room does not serve a 3K malloc */
		}
		// B105: the emergency ENDS here. gcHeapReserveArm is a stub since B78
		// (the BSS arena replaced the reserve) and nothing ever zeroed the
		// flag, so after the first failed malloc this loop ran every frame for
		// the rest of the run: free idles at 0.9-1.3MB, always under the 1.5MB
		// target, and models were evicted the moment their 5 s LoadedRecently
		// grace ended — the HD/LOD flicker.
		if(gStreamHeapFree >= STREAM_HEAP_FLOOR + 512*1024){
			gcHeapReserveArm();
			printf("HEAP: emergency over, free %uK after %u events\n", (unsigned)(gStreamHeapFree/1024), (unsigned)gHeapEmergency);
			gHeapEmergency = 0;
		}
	}else if(gHeapReserve == nil && gStreamHeapFree > STREAM_HEAP_FLOOR + 512*1024)   // B77: 1536K was never reached in play (free idles ~1.2MB), so the reserve never existed
		gcHeapReserveArm();
	// ARAM pressure (B66): TXDs stay resident at refcount 0 (TxdStore.cpp), so
	// the texel store fills and new TXDs — the pause menu's included — fail
	// their ARAM allocation. Drop unreferenced TXDs while under 1.5MB is left.
	for(int k = 0; k < 4 && gxAramStoreBytes && gxAramBytes + 1536*1024 > gxAramStoreBytes; k++){   // mip chains: a streamed dictionary is up to ~1.5MB, and a full store now fails it instead of spilling into MEM1 (B114 had cut the margin to 768K)
		CStreamingInfo *si; bool dropped = false;
		for(si = ms_endLoadedList.m_prev; si != &ms_startLoadedList; si = si->m_prev){
			int32 id = si - ms_aInfoForModel;
			if(id >= STREAM_OFFSET_TXD && id < STREAM_OFFSET_COL &&
			   CTxdStore::GetNumRefs(id - STREAM_OFFSET_TXD) == 0 && !IsTxdUsedByRequestedModels(id - STREAM_OFFSET_TXD) &&
			   !LoadedRecently(id)){   // B106: 5 s grace for TXDs as for models
				RemoveModel(id); gStrEvict++; dropped = true; break;
			}
		}
		if(!dropped){
			// Everything resident is referenced: free a far model so its TXD can go.
			// B107: one per frame, not four — the burst was visible as LOD flips
			// while 'blink' stayed 0 (B72 relief kept, throttled).
			if(RemoveLeastUsedModel(0)) gStrEvict++;
			break;
		}
	}
	if(STREAM_FLOOR_ENABLED && gStreamHeapFree < STREAM_HEAP_FLOOR){
		static uint32 lastSaid;
		if(CTimer::GetTimeInMilliseconds() - lastSaid > 1000){
			lastSaid = CTimer::GetTimeInMilliseconds();
			printf("STREAM floor: free %uK, shedding\n", (unsigned)(gStreamHeapFree/1024));
		}
		// Re-sample after a shed so one eviction is not repeated eight times;
		// when nothing is evictable, wait for the next sample instead of walking
		// the loaded list every frame for nothing.
		if(RemoveLeastUsedModel(0)){ gStrEvict++; gStreamHeapFree = mallinfo().fordblks; /* B85: the floor guards the GENERAL heap; chunk room does not serve a 3K malloc */ }
		else gStreamHeapFree = STREAM_HEAP_FLOOR;
	}
#endif

	if(ms_channelError != -1){
		RetryLoadFile(ms_channelError);
		return;
	}

	if(CTimer::GetIsPaused())
		return;

	LoadBigBuildingsWhenNeeded();
	if(!ms_disableStreaming && TheCamera.GetPosition().z < 55.0f)
		AddModelsToRequestList(TheCamera.GetPosition(), 0);

	DeleteFarAwayRwObjects(TheCamera.GetPosition());

	if(!ms_disableStreaming &&
	   !CCutsceneMgr::IsCutsceneProcessing() &&
	   ms_numModelsRequested < 5 &&
	   !CRenderer::m_loadingPriority &&
	   CGame::currArea == AREA_MAIN_MAP &&
	   !CReplay::IsPlayingBack()){
		StreamVehiclesAndPeds();
		StreamZoneModels(FindPlayerCoors());
	}

	LoadRequestedModels();

	if(CWorld::Players[0].m_pRemoteVehicle){
		CColStore::AddCollisionNeededAtPosn(FindPlayerCoors());
		CColStore::LoadCollision(CWorld::Players[0].m_pRemoteVehicle->GetPosition());
		CColStore::EnsureCollisionIsInMemory(CWorld::Players[0].m_pRemoteVehicle->GetPosition());
	}else{
		CColStore::LoadCollision(FindPlayerCoors());
		CColStore::EnsureCollisionIsInMemory(FindPlayerCoors());
	}

	// TODO: PrintRequestList
	//if (CPad::GetPad(1)->GetLeftShoulder2JustDown() && CPad::GetPad(1)->GetRightShoulder1() && CPad::GetPad(1)->GetRightShoulder2())
	//	PrintRequestList();

	for(si = ms_endRequestedList.m_prev; si != &ms_startRequestedList; si = prev){
		prev = si->m_prev;
		if((si->m_flags & (STREAMFLAGS_KEEP_IN_MEMORY|STREAMFLAGS_PRIORITY)) == 0)
			RemoveModel(si - ms_aInfoForModel);
	}
}

bool
CStreaming::LoadCdDirectory(void)
{
	char dirname[132];
	int i;

#ifdef GTA_PC
	ms_imageOffsets[0] = 0;
	ms_imageOffsets[1] = -1;
	ms_imageOffsets[2] = -1;
	ms_imageOffsets[3] = -1;
	ms_imageOffsets[4] = -1;
	ms_imageOffsets[5] = -1;
	ms_imageSize = GetGTA3ImgSize();
	if(ms_imageSize == 0 || ms_imageSize % CDSTREAM_SECTOR_SIZE != 0)
		return false;
	// PS2 uses CFileMgr::GetCdFile on all IMG files to fill the array
#endif

	i = CdStreamGetNumImages();
	if(i <= 0)
		return false;
	while(i-- >= 1){
		char *imageName = CdStreamGetImageName(i);
		if(imageName == nil || strlen(imageName) >= sizeof(dirname))
			return false;
		strcpy(dirname, imageName);
		char *extension = strrchr(dirname, '.');
		if(extension == nil || extension[1] == '\0' || strlen(extension + 1) != 3)
			return false;
		memcpy(extension + 1, "DIR", 3);
		if(!LoadCdDirectory(dirname, i))
			return false;
	}

	ms_lastImageRead = 0;
	ms_imageSize /= CDSTREAM_SECTOR_SIZE;
	return true;
}

bool
CStreaming::LoadCdDirectory(const char *dirname, int n)
{
	int fd, lastID;
	int32 status;
	uint32 imgSelector;
	int modelId;
	CDirectory::DirectoryInfo direntry;
	char *dot;
#ifdef GTA_OGC
	uint32 imageSectorCount;
#endif

	lastID = -1;
	if(n < 0 || n >= MAX_CDIMAGES)
		return false;
	fd = CFileMgr::OpenFile(dirname, "rb");
	if(fd <= 0)
		return false;
#ifdef GTA_OGC
	imageSectorCount = CdStreamGetImageSectorCount(n);
	if(imageSectorCount == 0){
		CFileMgr::CloseFile(fd);
		return false;
	}
#endif

	imgSelector = (uint32)n<<24;
	assert(sizeof(direntry) == 32);
	while((status = CDirectory::ReadEntry(fd, direntry)) > 0){
		if(direntry.offset >= 0x1000000 || direntry.size == 0 ||
		   direntry.size > UINT32_MAX/CDSTREAM_SECTOR_SIZE
#ifdef GTA_OGC
		   || direntry.offset > imageSectorCount || direntry.size > imageSectorCount - direntry.offset
#endif
		  ){
			status = -1;
			break;
		}
	}
	if(status < 0 || !CFileMgr::Seek(fd, 0, SEEK_SET)){
		CFileMgr::CloseFile(fd);
		return false;
	}
	while((status = CDirectory::ReadEntry(fd, direntry)) > 0){
		bool bAddToStreaming = false;
		size_t nameLen = strlen(direntry.name);

		if(direntry.size > (uint32)ms_streamingBufferSize)
			ms_streamingBufferSize = direntry.size;
		dot = strrchr(direntry.name, '.');
		if(dot == nil || dot == direntry.name || dot - direntry.name > 19 ||
		   dot + 4 != direntry.name + nameLen){
			CFileMgr::CloseFile(fd);
			return false;
		}

		*dot = '\0';

		if(strncasecmp(dot+1, "DFF", 3) == 0){
			if(CModelInfo::GetModelInfo(direntry.name, &modelId)){
				if(modelId < 0 || modelId >= STREAM_OFFSET_TXD){
					CFileMgr::CloseFile(fd);
					return false;
				}
				bAddToStreaming = true;
			}else{
#ifdef FIX_BUGS
				if(!ms_pExtraObjectsDir->AddItem(direntry, n)){
					CFileMgr::CloseFile(fd);
					return false;
				}
#else
				if(!ms_pExtraObjectsDir->AddItem(direntry)){
					CFileMgr::CloseFile(fd);
					return false;
				}
#endif
				lastID = -1;
			}
		}else if(strncasecmp(dot+1, "TXD", 3) == 0){
			modelId = CTxdStore::FindTxdSlot(direntry.name);
			if(modelId == -1)
				modelId = CTxdStore::AddTxdSlot(direntry.name);
			if(modelId < 0 || modelId >= STREAM_OFFSET_COL - STREAM_OFFSET_TXD){
				CFileMgr::CloseFile(fd);
				return false;
			}
			modelId += STREAM_OFFSET_TXD;
			bAddToStreaming = true;
		}else if(strncasecmp(dot+1, "COL", 3) == 0){
			modelId = CColStore::FindColSlot(direntry.name);
			if(modelId == -1)
				modelId = CColStore::AddColSlot(direntry.name);
			if(modelId < 0 || modelId >= STREAM_OFFSET_ANIM - STREAM_OFFSET_COL){
				CFileMgr::CloseFile(fd);
				return false;
			}
			modelId += STREAM_OFFSET_COL;
			bAddToStreaming = true;
		}else if(strncasecmp(dot+1, "IFP", 3) == 0){
			modelId = CAnimManager::RegisterAnimBlock(direntry.name);
			if(modelId < 0 || modelId >= NUMANIMBLOCKS){
				CFileMgr::CloseFile(fd);
				return false;
			}
			modelId += STREAM_OFFSET_ANIM;
			bAddToStreaming = true;
		}else{
			*dot = '.';
			lastID = -1;
		}

		if(bAddToStreaming && (modelId < 0 || modelId >= NUMSTREAMINFO)){
			CFileMgr::CloseFile(fd);
			return false;
		}
		if(bAddToStreaming){
			if(ms_aInfoForModel[modelId].GetCdSize()){
				debug("%s.%s appears more than once in %s\n", direntry.name, dot+1, dirname);
				lastID = -1;
			}else{
				direntry.offset |= imgSelector;
				ms_aInfoForModel[modelId].SetCdPosnAndSize(direntry.offset, direntry.size);
				if(lastID != -1)
					ms_aInfoForModel[lastID].m_nextID = modelId;
				lastID = modelId;
			}
		}
	}

	CFileMgr::CloseFile(fd);
	return status == 0;
}

static char*
GetObjectName(int streamId)
{
	static char objname[32];
	if(streamId < STREAM_OFFSET_TXD)
		sprintf(objname, "%s.dff", CModelInfo::GetModelInfo(streamId)->GetModelName());
	else if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL)
		sprintf(objname, "%s.txd", CTxdStore::GetTxdName(streamId-STREAM_OFFSET_TXD));
	else if(streamId >= STREAM_OFFSET_COL && streamId < STREAM_OFFSET_ANIM)
		sprintf(objname, "%s.col", CColStore::GetColName(streamId-STREAM_OFFSET_COL));
	else{
		assert(streamId < NUMSTREAMINFO);
		sprintf(objname, "%s.ifp", CAnimManager::GetAnimationBlock(streamId-STREAM_OFFSET_ANIM)->name);
	}
	return objname;
}

#ifdef USE_CUSTOM_ALLOCATOR
RpAtomic*
RegisterAtomicMemPtrsCB(RpAtomic *atomic, void *data)
{
	// empty because we expect models to be pre-instanced
	return atomic;
}
#endif

#ifdef GTA_OGC
// Resident cost is accounted at cd size, x1 — deliberately, do not "fix"
// this by scaling it up.
//
// A periodic TrimStreamedModels() used to run at the top of
// ConvertBufferToObject, evicting 64 least-used models every 16th call
// regardless of memory pressure. RemoveLeastUsedModel protects models with
// live refs, but a model that has just streamed in has *no* refs until its
// entity instantiates it — so the trim's favourite targets were exactly the
// models the streamer had only just finished loading, and its cadence was
// driven by loading itself. The more the game streamed, the more it threw
// away: trees and lamp posts never survived to render, interior models went
// missing, and LOD siblings vanished and returned, which is what read as
// LOD flicker (GetAtomicFromDistance hands the renderer a nil m_atomics[i]
// for a model evicted out from under it). The trim is gone; MakeSpaceFor
// already evicts correctly, bounded by the budget and only as much as the
// incoming load needs.
//
// The cd size is NOT the resident cost. Measured: the streamer sat at
// ms_memoryUsed = 7238K (cd bytes) while the real arena had dropped from
// 15620K to 2198K free — about 1.85x more resident than it believed. On PC
// this never matters, because reVC hands streaming a 65MB floor (see Init
// below) and the budget is never the binding constraint. Here it is 7428K,
// roughly 9x less, so an accounting error of 1.85x is the difference between
// a working open world and one that silently fails to load.
//
// Overcommitting is what produced every downstream symptom: allocations start
// failing, and since they now fail softly instead of exit(1)-ing, models and
// their collision simply never appear — chunks that never load, falling
// through the map, and a white world once the texture allocator is starved.
// Scaling this makes MakeSpaceFor stop *before* malloc fails, which is the
// only state where the streamer's own eviction can do its job.
//
// ponytail: one measured scalar. The honest version is USE_CUSTOM_ALLOCATOR
// accounting real bytes; this buys correctness now without that surgery.
// The 1.85x that used to be here was measured while gxGetTexture was being
// re-run on already-built textures (rasterLock handed back a fabricated,
// zeroed staging buffer and rasterUnlock marked the raster dirty), so every
// texture was re-tiled and re-allocated over and over. That leak is fixed, so
// the measurement that justified the scale no longer holds — and overstating
// resident cost keeps ms_memoryUsed permanently above ms_memoryAvailable,
// which makes MakeSpaceFor evict on every single load. That constant churn is
// what shows on screen as trees, props and LODs flickering in and out.
// Real resident cost per stream entry, in bytes actually taken from the heap,
// measured across the load. Cd size is not resident cost: a TXD arrives as
// DXT/pal8 on disc and lives as tiled GX texture memory, a DFF arrives as a
// chunk stream and lives as RW objects. The expansion differs per asset type,
// so no single scale factor is right — and getting it wrong is what has been
// oscillating this port between two failure modes. Under-count and the
// streamer overcommits until allocations fail silently (chunks that never
// load, falling through the map, white world). Over-count and it sits above
// its budget, evicting on every request, which shows up as the world drawn
// from far LODs that flicker in and out even while standing still.
static uint32 gResidentCost[NUMSTREAMINFO];
// A load that fails (a TXD whose texel block the heap could not give, a DFF
// the reader rejected) used to be answered with RemoveModel + ReRequestModel,
// unconditionally. At init LoadAllRequestedModels drains the request list, so
// one asset that fails every time keeps the loader spinning forever — the
// loading screen that never ends (B9: one 32KB texel alloc failed after the
// vehicle preload, then silence). Retry twice, then drop it: the world can
// still request it again later, but the streamer no longer re-requests its
// own failure. Reset on a successful load.
static void
FailedLoad(int32 streamId);

static void
FailedLoad(int32 streamId)
{
	CStreaming::RemoveModel(streamId);
	{
		uint8 sec = (uint8)(CTimer::GetTimeInMillisecondsPauseMode()/1000); if(sec == 0) sec = 1;
		if(gFailSec[streamId] && (uint8)(sec - gFailSec[streamId]) > 60) gLoadFails[streamId] = 0;   // B133: a model dropped a minute ago may try again (anim blocks 7910-7915 died at boot in b132)
		gFailSec[streamId] = sec;
	}
	if(gLoadFails[streamId] < 255)
		gLoadFails[streamId]++;
	// B124: a model the script or a dependency is waiting for must not be
	// dropped after three tries — the intro parked for 15 minutes on a black
	// screen (b121/b123, docks scene) waiting for vehicles whose 45-62K
	// geometry failed three times in a row. Each retry now runs the
	// size-aware shed, so the fourth try has a real hole to land in.
	// B131: 24 only for what the SCRIPT waits on; b129 spent 12 minutes at the
	// docks retrying six DEPENDENCY zone vehicles 24 times each.
	uint8 limit = (CStreaming::ms_aInfoForModel[streamId].m_flags & STREAMFLAGS_SCRIPTOWNED) || streamId >= STREAM_OFFSET_COL ? 24 : 3;   // B146: collision/anim blocks keep trying (b132: col 2K = white screen)
	if(gLoadFails[streamId] < limit)
		CStreaming::ReRequestModel(streamId);
	else if(gLoadFails[streamId] == limit)
		printf("streaming: %d failed to load %d times (%s), not retrying\n", streamId, limit, gFailWhy);
}

// Texture bytes the resident-cost measurement cannot see.
//
// gResidentCost is a heap delta taken across the load. rasterFromImage tiles
// eagerly, so textures built there land inside that window and are already
// charged — but gxGetTexture also runs from the draw path, for rasters that
// were not built eagerly or whose tiled buffer was released, and those
// allocations happen after the window has closed. Up to 512KB each, charged to
// nobody. ms_memoryUsed then sits under the truth, the streamer believes it
// has room it does not have, and the texture allocation eventually fails —
// silently, which is why it shows up as black silhouettes rather than as an
// out-of-memory.
//
// Charging only what falls OUTSIDE the window is exact by construction: no
// double counting, and no guessing at a scale factor.
bool gStreamMeasuring;

struct StreamMeasurementScope {
	bool previous;
	StreamMeasurementScope() : previous(gStreamMeasuring) { gStreamMeasuring = true; }
	~StreamMeasurementScope() { gStreamMeasuring = previous; }
};

// The ARAM cache sizes its slots from this: the largest single request the
// streamer can make, which is what ms_streamingBufferSize already is.
int32
CStreamingBufferSectors(void)
{
	return CStreaming::ms_streamingBufferSize;
}

extern "C" void
CStreamingTexBytes(long delta)
{
	if(delta >= 0)
		CStreaming::ms_memoryUsed += (size_t)delta;
	else{
		size_t d = (size_t)(-delta);
		CStreaming::ms_memoryUsed -= d <= CStreaming::ms_memoryUsed ?
		    d : CStreaming::ms_memoryUsed;
	}
}

// True while a load's heap delta is being measured, so the hook above knows
// the allocation is already accounted for.
extern "C" int
CStreamingMeasuring(void)
{
	return gStreamMeasuring ? 1 : 0;
}


// Reported per heartbeat interval, see MakeSpaceFor for what the pair means.
uint32 gStrEvict, gStrLoad;
extern "C" { uint32 gLoadAllN; }   // DIAG b176: blocking LoadAllRequestedModels calls
extern "C" void gcHeapCheck(uint32 tag, uint32 arg);   // gamecube.cpp, dvd:/heapcheck.txt

// mallinfo's uordblks underflows on a 24MB console (it reports ~305MB), but
// arena minus fordblks is sound: total heap obtained, less what is free.
size_t
OgcHeapResident(void)
{
	struct mallinfo mi = mallinfo();
	size_t arena = (size_t)mi.arena;
	// B87: general holes plus at most 768K of chunk room. All of it (B85) let str grow until 3K mallocs failed; none of it (B86) evicted the near set forever (near 0/13, evict +500/min).
	size_t chunkFree = gcHeapFreeTotal() - (size_t)mi.fordblks;
	size_t freeb = (size_t)mi.fordblks + (chunkFree < 768*1024 ? chunkFree : 768*1024);
	return arena > freeb ? arena - freeb : 0;
}

static uint32
StreamedSize(int32 streamId)
{
	// Fallback for the request path, which has to guess before the load
	// happens. Once loaded, gResidentCost holds the measured value.
	return CStreaming::ms_aInfoForModel[streamId].GetCdSize() *
	    CDSTREAM_SECTOR_SIZE;
}
#else
#define StreamedSize(id) (CStreaming::ms_aInfoForModel[id].GetCdSize() * CDSTREAM_SECTOR_SIZE)
#endif

bool
CStreaming::ConvertBufferToObject(int8 *buf, int32 streamId)
{
#ifdef GTA_OGC
	// B150: while a SCRIPTOWNED model converts, the emergency arena accepts any size.
	struct EssentialScope { EssentialScope(bool on){ gcEssentialLoad = on; } ~EssentialScope(){ gcEssentialLoad = 0; } } essential((ms_aInfoForModel[streamId].m_flags & STREAMFLAGS_SCRIPTOWNED) != 0);
#endif
#ifdef GTA_OGC
	if(gcBootDone() && streamId < STREAM_OFFSET_TXD &&
	   !(ms_aInfoForModel[streamId].m_flags & (STREAMFLAGS_SCRIPTOWNED | STREAMFLAGS_LOD)) &&
	   mallinfo().fordblks < STREAM_HEAP_FLOOR){
		static uint32 said; if(said++ < 30) printf("STREAM defer %d: heap %uK free\n", streamId, (unsigned)(mallinfo().fordblks/1024));
		RemoveModel(streamId);
		return false;
	}
#endif

	RwMemory mem;
	RwStream *stream;
	int cdsize;
	uint32 startTime, endTime, timeDiff;
	CBaseModelInfo *mi;
	bool success;

#ifdef GTA_OGC
	size_t residentBefore = OgcHeapResident();
	StreamMeasurementScope measurement;
	gStrLoad++;
#endif

	startTime = CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond();

	cdsize = ms_aInfoForModel[streamId].GetCdSize();
	mem.start = (uint8*)buf;
	mem.length = cdsize * CDSTREAM_SECTOR_SIZE;
	stream = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &mem);

	if(streamId < STREAM_OFFSET_TXD){
		// Model
		mi = CModelInfo::GetModelInfo(streamId);

		// Txd and anim have to be loaded
		int animId = mi->GetAnimFileIndex();
#ifdef FIX_BUGS
		if(!HasTxdLoaded(mi->GetTxdSlot()) ||
#else
		// texDict will exist even if only first part has loaded
		if(CTxdStore::GetSlot(mi->GetTxdSlot())->texDict == nil ||
#endif
		   animId != -1 && !CAnimManager::GetAnimationBlock(animId)->isLoaded){
			{
				static char why[48];
				int ts = mi->GetTxdSlot();
				snprintf(why, sizeof(why), "txd %d state %d anim %d", ts, ms_aInfoForModel[ts + STREAM_OFFSET_TXD].m_loadState, animId);
				gFailWhy = why;
			}
			FailedLoad(streamId);
			RwStreamClose(stream, &mem);
			return false;
		}

		// Set Txd and anims to use
		CTxdStore::AddRef(mi->GetTxdSlot());
#if GTA_VERSION > GTAVC_PS2
		if(animId != -1)
			CAnimManager::AddAnimBlockRef(animId);
#endif

		PUSH_MEMID(MEMID_STREAM_MODELS);
		CTxdStore::SetCurrentTxd(mi->GetTxdSlot());
		if(mi->IsSimple()){
			success = CFileLoader::LoadAtomicFile(stream, streamId);
			// TODO(MIAMI)? complain if file is not pre-instanced. we hardly are interested in that
		} else if (mi->GetModelType() == MITYPE_VEHICLE) {
			// load vehicles in two parts
			CModelInfo::GetModelInfo(streamId)->AddRef();
			success = CFileLoader::StartLoadClumpFile(stream, streamId);
			if(success)
				ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_STARTED;
			else
				CModelInfo::GetModelInfo(streamId)->RemoveRef();   // the ref above belongs to the second part, which never comes
		}else{
			success = CFileLoader::LoadClumpFile(stream, streamId);
#ifdef USE_CUSTOM_ALLOCATOR
			if(success)
				RpClumpForAllAtomics((RpClump*)mi->GetRwObject(), RegisterAtomicMemPtrsCB, nil);
#endif
		}
		POP_MEMID();
		UpdateMemoryUsed();

		// Txd and anims no longer needed unless we only read part of the file
		if(ms_aInfoForModel[streamId].m_loadState != STREAMSTATE_STARTED){
			CTxdStore::RemoveRefWithoutDelete(mi->GetTxdSlot());
#if GTA_VERSION > GTAVC_PS2
			if(animId != -1)
				CAnimManager::RemoveAnimBlockRefWithoutDelete(animId);
#endif
		}

		if(!success){
			debug("Failed to load %s\n", CModelInfo::GetModelInfo(streamId)->GetModelName());
			gFailWhy = "model load";
			FailedLoad(streamId);
			RwStreamClose(stream, &mem);
			return false;
		}
	}else if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL){
		// Txd
		if((ms_aInfoForModel[streamId].m_flags & STREAMFLAGS_KEEP_IN_MEMORY) == 0 &&
		   !IsTxdUsedByRequestedModels(streamId - STREAM_OFFSET_TXD)){
			RemoveModel(streamId);
			RwStreamClose(stream, &mem);
			return false;
		}

		PUSH_MEMID(MEMID_STREAM_TEXUTRES);
#ifdef GTA_OGC
		if(cdsize > 2*ms_streamingBufferSize){
			// bigger than the staging pair: DVD -> pair -> ARAM, see CdPullStream
			static CdPullStream pull;
			pull.init((uint8*)ms_pStreamingBuffer[0], (uint32)ms_streamingBufferSize*2*CDSTREAM_SECTOR_SIZE,
			    (uint8*)buf, gPullCd, (uint32)cdsize*CDSTREAM_SECTOR_SIZE);
			success = CTxdStore::LoadTxd(streamId - STREAM_OFFSET_TXD, &pull);
			printf("STREAM pull: txd %d %s %uK %s in %ums\n", streamId - STREAM_OFFSET_TXD, CTxdStore::GetTxdName(streamId - STREAM_OFFSET_TXD), (unsigned)cdsize*2, success ? "ok" : "FAILED",
			    (unsigned)(CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond() - startTime));
		}else
#endif
		if(ms_bLoadingBigModel || cdsize > 200){
			success = CTxdStore::StartLoadTxd(streamId - STREAM_OFFSET_TXD, stream);
			if(success)
				ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_STARTED;
		}else
		        success = CTxdStore::LoadTxd(streamId - STREAM_OFFSET_TXD, stream);
		POP_MEMID();
		UpdateMemoryUsed();

		if(!success){
			debug("Failed to load %s.txd\n", CTxdStore::GetTxdName(streamId - STREAM_OFFSET_TXD));
			gFailWhy = "txd load";
			FailedLoad(streamId);
			RwStreamClose(stream, &mem);
			return false;
		}
	}else if(streamId >= STREAM_OFFSET_COL && streamId < STREAM_OFFSET_ANIM){
		PUSH_MEMID(MEMID_STREAM_COLLISION);
		bool success = CColStore::LoadCol(streamId-STREAM_OFFSET_COL, mem.start, mem.length);
		POP_MEMID();
		if(!success){
			debug("Failed to load %s.col\n", CColStore::GetColName(streamId - STREAM_OFFSET_COL));
			FailedLoad(streamId);
			RwStreamClose(stream, &mem);
			return false;
		}
	}else if(streamId >= STREAM_OFFSET_ANIM){
		assert(streamId < NUMSTREAMINFO);
		if((ms_aInfoForModel[streamId].m_flags & STREAMFLAGS_KEEP_IN_MEMORY) == 0 &&
		   !AreAnimsUsedByRequestedModels(streamId - STREAM_OFFSET_ANIM)){
			RemoveModel(streamId);
			RwStreamClose(stream, &mem);
			return false;
		}
		PUSH_MEMID(MEMID_STREAM_ANIMATION);
		bool success = CAnimManager::LoadAnimFile(stream, true, nil);
		if(success)
			CAnimManager::CreateAnimAssocGroups();
		POP_MEMID();
		if(!success){
			debug("Failed to load animation block %d\n", streamId - STREAM_OFFSET_ANIM);
			FailedLoad(streamId);
			RwStreamClose(stream, &mem);
			return false;
		}
	}

	RwStreamClose(stream, &mem);

	if(streamId < STREAM_OFFSET_TXD){
		// Model
		// Vehicles and Peds not in loaded list
		if (mi->GetModelType() != MITYPE_VEHICLE && mi->GetModelType() != MITYPE_PED) {
			CSimpleModelInfo *smi = (CSimpleModelInfo*)mi;

			// Set fading for some objects
			if(mi->IsSimple() && !smi->m_isBigBuilding){
				if(ms_aInfoForModel[streamId].m_flags & STREAMFLAGS_NOFADE)
					smi->m_alpha = 255;
				else
					smi->m_alpha = 0;
			}

			if(CanRemoveModel(streamId))
				ms_aInfoForModel[streamId].AddToList(&ms_startLoadedList);
		}
	}else if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL ||
	         streamId >= STREAM_OFFSET_ANIM){
		assert(streamId < NUMSTREAMINFO);
		// Txd and anims
		if(CanRemoveModel(streamId))
			ms_aInfoForModel[streamId].AddToList(&ms_startLoadedList);
	}

	// Mark objects as loaded
	if(ms_aInfoForModel[streamId].m_loadState != STREAMSTATE_STARTED){
		ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_LOADED;
#ifdef GTA_OGC
		{
			size_t after = OgcHeapResident();
			uint32 cost = after > residentBefore ?
			    (uint32)(after - residentBefore) : 0;
			// Never charge zero. A shared TXD that was already resident
			// measures a ~0 delta, and an entry charged 0 refunds 0 when it
			// is removed — so MakeSpaceFor evicts model after model without
			// ms_memoryUsed ever falling, and clears the entire world before
			// giving up. Opening the pause menu was enough to trigger it.
			// Floor at the cd size so every eviction makes progress.
			// A TXD's texels live in ARAM: charge it the cd size and MakeSpaceFor
			// evicts real geometry to admit a file that costs MEM1 nothing.
			uint32 floorCost = streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL ? 4*1024 : StreamedSize(streamId);
			gResidentCost[streamId] = cost > floorCost ? cost : floorCost;
			ms_memoryUsed += gResidentCost[streamId];
			StampLoaded(streamId);
			gLoadFails[streamId] = 0;
		}
#elif !defined(USE_CUSTOM_ALLOCATOR)
		ms_memoryUsed += StreamedSize(streamId);
#endif
	}

	endTime = CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond();
	timeDiff = endTime - startTime;
	if(timeDiff > 5)
		debug("%s took %d ms\n", GetObjectName(streamId), timeDiff);

	return true;
}

bool
CStreaming::FinishLoadingLargeFile(int8 *buf, int32 streamId)
{
	RwMemory mem;
	RwStream *stream;
	uint32 startTime, endTime, timeDiff;
	CBaseModelInfo *mi;
	bool success;

#ifdef GTA_OGC
	size_t residentBefore = OgcHeapResident();
	StreamMeasurementScope measurement;
	gStrLoad++;
#endif

	startTime = CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond();

	if(ms_aInfoForModel[streamId].m_loadState != STREAMSTATE_STARTED){
		if(streamId < STREAM_OFFSET_TXD)
			CModelInfo::GetModelInfo(streamId)->RemoveRef();
		return false;
	}

	mem.start = (uint8*)buf;
	mem.length = ms_aInfoForModel[streamId].GetCdSize() * CDSTREAM_SECTOR_SIZE;
	stream = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &mem);

	if(streamId < STREAM_OFFSET_TXD){
		// Model
		mi = CModelInfo::GetModelInfo(streamId);
		PUSH_MEMID(MEMID_STREAM_MODELS);
		CTxdStore::SetCurrentTxd(mi->GetTxdSlot());
		success = CFileLoader::FinishLoadClumpFile(stream, streamId);
		if(success){
#ifdef USE_CUSTOM_ALLOCATOR
			RpClumpForAllAtomics((RpClump*)mi->GetRwObject(), RegisterAtomicMemPtrsCB, nil);
#endif
			success = AddToLoadedVehiclesList(streamId);
		}
		POP_MEMID();
		mi->RemoveRef();
		CTxdStore::RemoveRefWithoutDelete(mi->GetTxdSlot());
#if GTA_VERSION > GTAVC_PS2
		if(mi->GetAnimFileIndex() != -1)
			CAnimManager::RemoveAnimBlockRefWithoutDelete(mi->GetAnimFileIndex());
#endif
	}else if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL){
		// Txd
		CTxdStore::AddRef(streamId - STREAM_OFFSET_TXD);
		PUSH_MEMID(MEMID_STREAM_TEXUTRES);
		success = CTxdStore::FinishLoadTxd(streamId - STREAM_OFFSET_TXD, stream);
		POP_MEMID();
		CTxdStore::RemoveRefWithoutDelete(streamId - STREAM_OFFSET_TXD);
	}else{
		assert(0 && "invalid streamId");
	}

	RwStreamClose(stream, &mem);

	ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_LOADED;
#ifdef GTA_OGC
	{
		size_t after = OgcHeapResident();
		uint32 cost = after > residentBefore ?
		    (uint32)(after - residentBefore) : 0;
		uint32 floorCost = streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL ? 4*1024 : StreamedSize(streamId);
		gResidentCost[streamId] = cost > floorCost ? cost : floorCost;
		ms_memoryUsed += gResidentCost[streamId];
	}
#elif !defined(USE_CUSTOM_ALLOCATOR)
	ms_memoryUsed += StreamedSize(streamId);
#endif

	if(!success){
		FailedLoad(streamId);
		UpdateMemoryUsed();
		return false;
	}

	UpdateMemoryUsed();

	endTime = CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond();
	timeDiff = endTime - startTime;
	if(timeDiff > 5)
		debug("%s took %d ms\n", GetObjectName(streamId), timeDiff);

	return true;
}

void
CStreaming::RequestModel(int32 id, int32 flags)
{
	CSimpleModelInfo *mi;

#ifdef GTA_OGC
	if(id < STREAM_OFFSET_TXD){
		mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(id);
		if(mi && mi->IsSimple() && mi->m_isBigBuilding)
			flags |= STREAMFLAGS_LOD;
	}
	if(flags & STREAMFLAGS_LOD)
		flags |= STREAMFLAGS_PRIORITY;
	// B145: admission control instead of read-then-defer. b144 read every
	// deferred model three times per pass and the renderer re-requested it
	// every frame: a 100 s scene load with the DVD churning. Below the floor,
	// or inside a model's fail cooldown, an ordinary request is simply not
	// queued; the script's own requests always are.
	if(gcBootDone() && id < STREAM_OFFSET_TXD && ms_aInfoForModel[id].m_loadState == STREAMSTATE_NOTLOADED && !(flags & STREAMFLAGS_SCRIPTOWNED) && !(ms_aInfoForModel[id].m_flags & STREAMFLAGS_SCRIPTOWNED)){   // B146: MODELS only — b145 deferred the zone's collision block and the car fell through the world
		uint8 sec = (uint8)(CTimer::GetTimeInMillisecondsPauseMode()/1000); if(sec == 0) sec = 1;
		if(gLoadFails[id] >= 3 && gFailSec[id] && (uint8)(sec - gFailSec[id]) <= 60)
			return;
		if(mallinfo().fordblks < STREAM_HEAP_FLOOR && !(flags & STREAMFLAGS_LOD))
			return;
	}
#endif
	if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_INQUEUE){
		// updgrade to priority
		if(flags & STREAMFLAGS_PRIORITY && !ms_aInfoForModel[id].IsPriority()){
			ms_numPriorityRequests++;
			ms_aInfoForModel[id].m_flags |= STREAMFLAGS_PRIORITY;
		}
#ifdef GTA_OGC
		if(id < STREAM_OFFSET_TXD && (flags & (STREAMFLAGS_PRIORITY | STREAMFLAGS_LOD))){
			CBaseModelInfo *model = CModelInfo::GetModelInfo(id);
			int dependencyFlags = flags & (STREAMFLAGS_PRIORITY | STREAMFLAGS_LOD);
			ReRequestModel(model->GetTxdSlot() + STREAM_OFFSET_TXD, dependencyFlags);
			int anim = model->GetAnimFileIndex();
			if(anim != -1)
				ReRequestModel(anim + STREAM_OFFSET_ANIM, dependencyFlags);
		}
#endif
	}else if(ms_aInfoForModel[id].m_loadState != STREAMSTATE_NOTLOADED){
		flags &= ~STREAMFLAGS_PRIORITY;
	}
	ms_aInfoForModel[id].m_flags |= flags;

	if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_LOADED){
		// Already loaded, only check changed flags

		if(ms_aInfoForModel[id].m_flags & STREAMFLAGS_NOFADE && id < STREAM_OFFSET_TXD){
			mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(id);
			if(mi->IsSimple())
				mi->m_alpha = 255;
		}

		// reinsert into list
		if(ms_aInfoForModel[id].m_next){
			ms_aInfoForModel[id].RemoveFromList();
			if(CanRemoveModel(id))
				ms_aInfoForModel[id].AddToList(&ms_startLoadedList);
		}
	}else if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_NOTLOADED ||
	         ms_aInfoForModel[id].m_loadState == STREAMSTATE_LOADED){	// how can this be true again?

		if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_NOTLOADED){
			if(id < STREAM_OFFSET_TXD){
				mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(id);
				RequestTxd(mi->GetTxdSlot(), flags);
				int anim = mi->GetAnimFileIndex();
				if(anim != -1)
					RequestAnim(anim, STREAMFLAGS_DEPENDENCY | (flags & (STREAMFLAGS_PRIORITY | STREAMFLAGS_LOD)));
			}
			ms_aInfoForModel[id].AddToList(&ms_startRequestedList);
			ms_numModelsRequested++;
			if(flags & STREAMFLAGS_PRIORITY)
				ms_numPriorityRequests++;
		}

		ms_aInfoForModel[id].m_loadState = STREAMSTATE_INQUEUE;
		ms_aInfoForModel[id].m_flags = flags;
	}
}

#define BIGBUILDINGFLAGS STREAMFLAGS_DONT_REMOVE

void
CStreaming::RequestBigBuildings(eLevelName level)
{
	int i, n;
	CBuilding *b;

	n = CPools::GetBuildingPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		b = CPools::GetBuildingPool()->GetSlot(i);
		if(b && b->bIsBIGBuilding
#ifdef NO_ISLAND_LOADING
		   && (((FrontEndMenuManager.m_PrefsIslandLoading != CMenuManager::ISLAND_LOADING_LOW) && (b != pIslandLODmainlandEntity) &&
		        (b != pIslandLODbeachEntity)) ||
		       (b->m_level == level))
#else
		   && b->m_level == level
#endif
			)
			if(!b->bStreamBIGBuilding)
				RequestModel(b->GetModelIndex(), BIGBUILDINGFLAGS);
	}
	RequestIslands(level);
}

void
CStreaming::RequestBigBuildings(eLevelName level, const CVector &pos)
{
	int i, n;
	CBuilding *b;

	n = CPools::GetBuildingPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		b = CPools::GetBuildingPool()->GetSlot(i);
		if(b && b->bIsBIGBuilding
#ifdef NO_ISLAND_LOADING
		    && (((FrontEndMenuManager.m_PrefsIslandLoading != CMenuManager::ISLAND_LOADING_LOW) && (b != pIslandLODmainlandEntity) && (b != pIslandLODbeachEntity)
				) || (b->m_level == level))
#else
		   && b->m_level == level
#endif
		)
			if(b->bStreamBIGBuilding){
				if(CRenderer::ShouldModelBeStreamed(b, pos))
					RequestModel(b->GetModelIndex(), 0);
			}else
				RequestModel(b->GetModelIndex(), BIGBUILDINGFLAGS);
	}
	RequestIslands(level);
}

void
CStreaming::InstanceBigBuildings(eLevelName level, const CVector &pos)
{
	int i, n;
	CBuilding *b;

	n = CPools::GetBuildingPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		b = CPools::GetBuildingPool()->GetSlot(i);
		if(b && b->bIsBIGBuilding && b->m_level == level &&
		   b->bStreamBIGBuilding && b->m_rwObject == nil)
			if(CRenderer::ShouldModelBeStreamed(b, pos))
				b->CreateRwObject();
	}
}

void
CStreaming::InstanceLoadedModelsInSectorList(CPtrList &list)
{
	CPtrNode *node;
	CEntity *e;
	for(node = list.first; node; node = node->next) {
		e = (CEntity *)node->item;
		if(IsAreaVisible(e->m_area) && e->m_rwObject == nil)
			e->CreateRwObject();
	}
}

void
CStreaming::InstanceLoadedModels(const CVector &pos)
{
	int minX = CWorld::GetSectorIndexX(pos.x - 80.0f);
	if(minX <= 0) minX = 0;

	int minY = CWorld::GetSectorIndexY(pos.y - 80.0f);
	if(minY <= 0) minY = 0;

	int maxX = CWorld::GetSectorIndexX(pos.x + 80.0f);
	if(maxX >= NUMSECTORS_X) maxX = NUMSECTORS_X - 1;

	int maxY = CWorld::GetSectorIndexY(pos.y + 80.0f);
	if(maxY >= NUMSECTORS_Y) maxY = NUMSECTORS_Y - 1;

	int x, y;
	for(y = minY; y <= maxY; y++){
		for(x = minX; x <= maxX; x++){
			CSector *sector = CWorld::GetSector(x, y);
			InstanceLoadedModelsInSectorList(sector->m_lists[ENTITYLIST_BUILDINGS]);
			InstanceLoadedModelsInSectorList(sector->m_lists[ENTITYLIST_BUILDINGS_OVERLAP]);
			InstanceLoadedModelsInSectorList(sector->m_lists[ENTITYLIST_OBJECTS]);
			InstanceLoadedModelsInSectorList(sector->m_lists[ENTITYLIST_DUMMIES]);
		}
	}
}

void
CStreaming::RequestIslands(eLevelName level)
{
	ISLAND_LOADING_ISNT(HIGH)
	switch(level){
	case LEVEL_MAINLAND:
		if(islandLODbeach != -1)
			RequestModel(islandLODbeach, BIGBUILDINGFLAGS);
		break;
	case LEVEL_BEACH:
		if(islandLODmainland != -1)
			RequestModel(islandLODmainland, BIGBUILDINGFLAGS);
		break;
	default: break;
	}
}

static char *IGnames[] = {
	"player",
	"player2",
	"player3",
	"player4",
	"player5",
	"player6",
	"player7",
	"player8",
	"player9",
	"play10",
	"play11",
	"igken",
	"igcandy",
	"igsonny",
	"igbuddy",
	"igjezz",
	"ighlary",
	"igphil",
	"igmerc",
	"igdick",
	"igdiaz",
	""
};

static char *CSnames[] = {
	"csplay",
	"csplay2",
	"csplay3",
	"csplay4",
	"csplay5",
	"csplay6",
	"csplay7",
	"csplay8",
	"csplay9",
	"csplay10",
	"csplay11",
	"csken",
	"cscandy",
	"cssonny",
	"csbuddy",
	"csjezz",
	"cshlary",
	"csphil",
	"csmerc",
	"csdick",
	"csdiaz",
	""
};

void
CStreaming::RequestSpecialModel(int32 modelId, const char *modelName, int32 flags)
{
	CBaseModelInfo *mi;
	int txdId;
	char oldName[48];
	uint32 pos, size;
	int i, n;

	mi = CModelInfo::GetModelInfo(modelId);
	if(strncasecmp("CSPlay", modelName, 6) == 0){
		char *curname = CModelInfo::GetModelInfo(MI_PLAYER)->GetModelName();
		for(int i = 0; CSnames[i][0]; i++){
			if(strcasecmp(curname, IGnames[i]) == 0){
				modelName = CSnames[i];
				break;
			}
		}
	}
	if(!CGeneral::faststrcmp(mi->GetModelName(), modelName)){
		// Already have the correct name, just request it
		RequestModel(modelId, flags | STREAMFLAGS_PRIORITY);   // B131: the script WAITs on these — ahead of the zone vehicles
		return;
	}

	if(mi->GetNumRefs() > 0){
		n = CPools::GetPedPool()->GetSize()-1;
		for(i = n; i >= 0 && mi->GetNumRefs() > 0; i--){
			CPed *ped = CPools::GetPedPool()->GetSlot(i);
			if(ped && ped->GetModelIndex() == modelId &&
			   !ped->IsPlayer() && ped->CanBeDeletedEvenInVehicle())
				CTheScripts::RemoveThisPed(ped);
		}
		n = CPools::GetObjectPool()->GetSize()-1;
		for(i = n; i >= 0 && mi->GetNumRefs() > 0; i--){
			CObject *obj = CPools::GetObjectPool()->GetSlot(i);
			if(obj && obj->GetModelIndex() == modelId && obj->CanBeDeleted()){
				CWorld::Remove(obj);
				CWorld::RemoveReferencesToDeletedObject(obj);
				delete obj;
			}
		}
	}

	strcpy(oldName, mi->GetModelName());
	mi->SetModelName(modelName);

	// What exactly is going on here?
	if(CModelInfo::GetModelInfo(oldName, nil)){
		txdId = CTxdStore::FindTxdSlot(oldName);
		if(txdId != -1 && CTxdStore::GetSlot(txdId)->texDict){
			CTxdStore::AddRef(txdId);
			RemoveModel(modelId);
			CTxdStore::RemoveRefWithoutDelete(txdId);
		}else
			RemoveModel(modelId);
	}else
		RemoveModel(modelId);

	bool found = ms_pExtraObjectsDir->FindItem(modelName, pos, size);
	assert(found);
	mi->ClearTexDictionary();
	if(CTxdStore::FindTxdSlot(modelName) == -1)
		mi->SetTexDictionary("generic");
	else
		mi->SetTexDictionary(modelName);
	ms_aInfoForModel[modelId].SetCdPosnAndSize(pos, size);
	RequestModel(modelId, flags);
}

void
CStreaming::RequestSpecialChar(int32 charId, const char *modelName, int32 flags)
{
	RequestSpecialModel(charId + MI_SPECIAL01, modelName, flags);
}

bool
CStreaming::HasSpecialCharLoaded(int32 id)
{
	// B131 diagnostic: the intro's WAIT on six special characters parked
	// b121-b129 at the docks; say what state the missing one is in.
	if(!HasModelLoaded(id + MI_SPECIAL01)){
		gSpecialWaitFrame = CTimer::GetFrameCounter(); if(gSpecialWaitFrame == 0) gSpecialWaitFrame = 1;   // B142: the sweeps relax while this is fresh
		static uint32 lastMs; uint32 now = CTimer::GetTimeInMillisecondsPauseMode();
		// B138: the B137 self-heal (RequestModel from here) froze b137 in
		// CdStreamSync at the office scene — never request from a poll.
		if(now - lastMs > 5000){
			lastMs = now;
			int32 m = id + MI_SPECIAL01;
			printf("STREAM specialchar %d model %d state %d flags %x fails %d req %d\n", id, m, (int)ms_aInfoForModel[m].m_loadState,
			    (unsigned)ms_aInfoForModel[m].m_flags, (int)gLoadFails[m], (int)(ms_aInfoForModel[m].m_next != nil));
		}
		return false;
	}
	return true;
}

void
CStreaming::SetMissionDoesntRequireSpecialChar(int32 id)
{
	return SetMissionDoesntRequireModel(id + MI_SPECIAL01);
}

void
CStreaming::DecrementRef(int32 id)
{
	ms_numModelsRequested--;
	if(ms_aInfoForModel[id].IsPriority()){
		ms_aInfoForModel[id].m_flags &= ~STREAMFLAGS_PRIORITY;
		ms_numPriorityRequests--;
	}
}

void
CStreaming::RemoveModel(int32 id)
{
	int i;

	if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_NOTLOADED)
		return;
#ifdef GTA_OGC
	// B53 tracer: who evicts a loaded TXD (the 710K pull-every-500ms loop).
	if(id >= STREAM_OFFSET_TXD && id < STREAM_OFFSET_COL && ms_aInfoForModel[id].m_loadState == STREAMSTATE_LOADED){
		static uint32 lastMs;
		uint32 now = CTimer::GetTimeInMilliseconds();
		if(now - lastMs > 250){
			lastMs = now;
			printf("STREAM rm txd %d %s refs %d req %d by %p\n", id - STREAM_OFFSET_TXD, CTxdStore::GetTxdName(id - STREAM_OFFSET_TXD),
			    CTxdStore::GetNumRefs(id - STREAM_OFFSET_TXD), IsTxdUsedByRequestedModels(id - STREAM_OFFSET_TXD),
			    __builtin_return_address(0));
		}
	}
#endif

	if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_LOADED){
		if(id < STREAM_OFFSET_TXD)
			CModelInfo::GetModelInfo(id)->DeleteRwObject();
		else if(id >= STREAM_OFFSET_TXD && id < STREAM_OFFSET_COL)
			CTxdStore::RemoveTxd(id - STREAM_OFFSET_TXD);
		else if(id >= STREAM_OFFSET_COL && id < STREAM_OFFSET_ANIM)
			CColStore::RemoveCol(id - STREAM_OFFSET_COL);
		else if(id >= STREAM_OFFSET_ANIM){
			assert(id < NUMSTREAMINFO);
			CAnimManager::RemoveAnimBlock(id - STREAM_OFFSET_ANIM);
		}
#ifdef GTA_OGC
		// Refund exactly what this entry was charged, not a re-derived
		// estimate, or ms_memoryUsed drifts away from reality over time.
		ms_memoryUsed -= ms_memoryUsed >= gResidentCost[id] ?
		    gResidentCost[id] : ms_memoryUsed;
		gResidentCost[id] = 0;
#else
		ms_memoryUsed -= StreamedSize(id);
#endif
	}

	if(ms_aInfoForModel[id].m_next){
		// Remove from list, model is neither loaded nor requested
		if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_INQUEUE)
			DecrementRef(id);
		ms_aInfoForModel[id].RemoveFromList();
	}else if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_READING){
		for(i = 0; i < 4; i++){
			if(ms_channel[0].streamIds[i] == id)
				ms_channel[0].streamIds[i] = -1;
			if(ms_channel[1].streamIds[i] == id)
				ms_channel[1].streamIds[i] = -1;
		}
	}

	if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_STARTED){
		if(id < STREAM_OFFSET_TXD)
			RpClumpGtaCancelStream();
		else if(id >= STREAM_OFFSET_TXD && id < STREAM_OFFSET_COL)
			CTxdStore::RemoveTxd(id - STREAM_OFFSET_TXD);
		else if(id >= STREAM_OFFSET_COL && id < STREAM_OFFSET_ANIM)
			CColStore::RemoveCol(id - STREAM_OFFSET_COL);
		else if(id >= STREAM_OFFSET_ANIM){
			assert(id < NUMSTREAMINFO);
			CAnimManager::RemoveAnimBlock(id - STREAM_OFFSET_ANIM);
		}
	}

	ms_aInfoForModel[id].m_loadState = STREAMSTATE_NOTLOADED;
}

void
CStreaming::RemoveUnusedBuildings(eLevelName level)
{
	if(level != LEVEL_BEACH)
		RemoveBuildings(LEVEL_BEACH);
	if(level != LEVEL_MAINLAND)
		RemoveBuildings(LEVEL_MAINLAND);
}

void
CStreaming::RemoveBuildings(eLevelName level)
{
	int i, n;
	CEntity *e;
	CBaseModelInfo *mi;

	n = CPools::GetBuildingPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetBuildingPool()->GetSlot(i);
		if(e && e->m_level == level){
			mi = CModelInfo::GetModelInfo(e->GetModelIndex());
			if(!e->bImBeingRendered){
				e->DeleteRwObject();
				if (mi->GetNumRefs() == 0)
					RemoveModel(e->GetModelIndex());
			}
		}
	}

	n = CPools::GetTreadablePool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetTreadablePool()->GetSlot(i);
		if(e && e->m_level == level){
			mi = CModelInfo::GetModelInfo(e->GetModelIndex());
			if(!e->bImBeingRendered){
				e->DeleteRwObject();
				if (mi->GetNumRefs() == 0)
					RemoveModel(e->GetModelIndex());
			}
		}
	}

	n = CPools::GetObjectPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetObjectPool()->GetSlot(i);
		if(e && e->m_level == level){
			mi = CModelInfo::GetModelInfo(e->GetModelIndex());
			if(!e->bImBeingRendered && ((CObject*)e)->ObjectCreatedBy == GAME_OBJECT){
				e->DeleteRwObject();
				if (mi->GetNumRefs() == 0)
					RemoveModel(e->GetModelIndex());
			}
		}
	}

	n = CPools::GetDummyPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetDummyPool()->GetSlot(i);
		if(e && e->m_level == level){
			mi = CModelInfo::GetModelInfo(e->GetModelIndex());
			if(!e->bImBeingRendered){
				e->DeleteRwObject();
				if (mi->GetNumRefs() == 0)
					RemoveModel(e->GetModelIndex());
			}
		}
	}
}

void
CStreaming::RemoveBuildingsNotInArea(int32 area)
{
	int i, n;
	CEntity *e;

	n = CPools::GetBuildingPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetBuildingPool()->GetSlot(i);
		if(e && e->m_rwObject && !IsAreaVisible(area) &&
		   (!e->bIsBIGBuilding || e->bStreamBIGBuilding)){
			if(e->bIsBIGBuilding)
				RequestModel(e->GetModelIndex(), 0);
			if(!e->bImBeingRendered)
				e->DeleteRwObject();
		}
	}

	n = CPools::GetTreadablePool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetTreadablePool()->GetSlot(i);
		if(e && e->m_rwObject && !IsAreaVisible(area) &&
		   (!e->bIsBIGBuilding || e->bStreamBIGBuilding)){
			if(e->bIsBIGBuilding)
				RequestModel(e->GetModelIndex(), 0);
			if(!e->bImBeingRendered)
				e->DeleteRwObject();
		}
	}

	n = CPools::GetObjectPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetObjectPool()->GetSlot(i);
		if(e && e->m_rwObject && !IsAreaVisible(area) &&
		   (!e->bIsBIGBuilding || e->bStreamBIGBuilding)){
			if(e->bIsBIGBuilding)
				RequestModel(e->GetModelIndex(), 0);
			if(!e->bImBeingRendered)
				e->DeleteRwObject();
		}
	}

	n = CPools::GetDummyPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetDummyPool()->GetSlot(i);
		if(e && e->m_rwObject && !IsAreaVisible(area) &&
		   (!e->bIsBIGBuilding || e->bStreamBIGBuilding)){
			if(e->bIsBIGBuilding)
				RequestModel(e->GetModelIndex(), 0);
			if(!e->bImBeingRendered)
				e->DeleteRwObject();
		}
	}
}

void
CStreaming::RemoveUnusedBigBuildings(eLevelName level)
{
	ISLAND_LOADING_IS(LOW)
	{
	if(level != LEVEL_BEACH)
		RemoveBigBuildings(LEVEL_BEACH);
	if(level != LEVEL_MAINLAND)
		RemoveBigBuildings(LEVEL_MAINLAND);
	}
	RemoveIslandsNotUsed(level);
}

void
DeleteIsland(CEntity *island)
{
	if(island == nil)
		return;
	if(island->bImBeingRendered)
		debug("Didn't delete island because it was being rendered\n");
	else{
		island->DeleteRwObject();
		CStreaming::RemoveModel(island->GetModelIndex());
	}
}

void
CStreaming::RemoveIslandsNotUsed(eLevelName level)
{
	int i;
	if(pIslandLODmainlandEntity == nil)
	for(i = CPools::GetBuildingPool()->GetSize()-1; i >= 0; i--){
		CBuilding *building = CPools::GetBuildingPool()->GetSlot(i);
		if(building == nil)
			continue;
		if(building->GetModelIndex() == islandLODmainland)
			pIslandLODmainlandEntity = building;
		if(building->GetModelIndex() == islandLODbeach)
			pIslandLODbeachEntity = building;
	}
#ifdef NO_ISLAND_LOADING
	if(FrontEndMenuManager.m_PrefsIslandLoading == CMenuManager::ISLAND_LOADING_HIGH) {
		DeleteIsland(pIslandLODmainlandEntity);
		DeleteIsland(pIslandLODbeachEntity);
	} else
#endif
	switch(level){
	case LEVEL_MAINLAND:
		DeleteIsland(pIslandLODmainlandEntity);
		break;
	case LEVEL_BEACH:
		DeleteIsland(pIslandLODbeachEntity);

		break;
	}
}

void
CStreaming::RemoveBigBuildings(eLevelName level)
{
	int i, n;
	CEntity *e;
	CBaseModelInfo *mi;

	n = CPools::GetBuildingPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetBuildingPool()->GetSlot(i);
		if(e && e->bIsBIGBuilding && e->m_level == level){
			mi = CModelInfo::GetModelInfo(e->GetModelIndex());
			if(!e->bImBeingRendered){
				e->DeleteRwObject();
				if (mi->GetNumRefs() == 0)
					RemoveModel(e->GetModelIndex());
			}
		}
	}
}

bool
CStreaming::RemoveLoadedVehicle(void)
{
	int i, id;

	for(i = 0; i < MAXVEHICLESLOADED; i++){
		ms_lastVehicleDeleted++;
		if(ms_lastVehicleDeleted == MAXVEHICLESLOADED)
			ms_lastVehicleDeleted = 0;
		id = ms_vehiclesLoaded[ms_lastVehicleDeleted];
		if(id != -1 && CanRemoveModel(id) && CModelInfo::GetModelInfo(id)->GetNumRefs() == 0 &&
		   ms_aInfoForModel[id].m_loadState == STREAMSTATE_LOADED)
			goto found;
	}
	return false;
found:
	RemoveModel(ms_vehiclesLoaded[ms_lastVehicleDeleted]);
	ms_numVehiclesLoaded--;
	ms_vehiclesLoaded[ms_lastVehicleDeleted] = -1;
	CVehicleModelInfo* pVehicleInfo = (CVehicleModelInfo*)CModelInfo::GetModelInfo(id);
	if (pVehicleInfo->m_vehicleClass != -1)
		CCarCtrl::RemoveFromLoadedVehicleArray(id, pVehicleInfo->m_vehicleClass);
	return true;
}

// B77: a malloc failed with free memory in the arena (fragmentation): B76 died in
// rw::Skin::init with 1.27MB free. mustmalloc exits on the first NULL, so this
// runs INSIDE the failing malloc: drop unreferenced loaded models until a block
// of the wanted size exists, then the caller retries. Only the main thread may
// touch the streaming tables.
#ifdef GTA_OGC
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#endif
extern "C" void *__real_malloc(size_t);
extern "C" void *gcBigAlloc(size_t); extern "C" void gcBigFree(void*); extern "C" int gcBigContains(const void*);
extern "C" lwp_t gMainLwp;   // gamecube.cpp main(): only the main thread may touch the streaming tables

// ARAM is a cache for texels: when a dictionary does not fit the store
// (gxraster.cpp readNativeTexture), free one thing nothing draws and let the
// caller retry — an unreferenced dictionary first, else the least-used model
// (its dictionary goes on a later call). b193 failed the dictionary instead,
// and the intro's blocking load retried hotelroomint for over a minute of
// black screen. Returns 0 when nothing more can go.
extern "C" int
gcAramReclaim(unsigned bytes)
{
	static bool8 inReclaim;
	(void)bytes;
	if(inReclaim || CStreaming::ms_endLoadedList.m_prev == nil || LWP_GetSelf() != gMainLwp)
		return 0;
	inReclaim = TRUE;
	int freed = 0;
	for(CStreamingInfo *si = CStreaming::ms_endLoadedList.m_prev; si != &CStreaming::ms_startLoadedList; si = si->m_prev){
		int32 id = si - CStreaming::ms_aInfoForModel;
		if(id >= STREAM_OFFSET_TXD && id < STREAM_OFFSET_COL && !(si->m_flags & STREAMFLAGS_CANT_REMOVE) &&
		   CTxdStore::GetNumRefs(id - STREAM_OFFSET_TXD) == 0 &&
		   !CStreaming::IsTxdUsedByRequestedModels(id - STREAM_OFFSET_TXD)){
			CStreaming::RemoveModel(id);
			freed = 1;
			break;
		}
	}
	if(!freed)
		freed = CStreaming::RemoveLeastUsedModel(0, true);
	inReclaim = FALSE;
	return freed;
}

extern "C" int
gcStreamEmergencyShed(unsigned need)
{
	static bool8 inShed;
	if(inShed) return 0;
	// B108: before CStreaming::Init the lists are all-zero; walking them from a
	// failed malloc inside CFileLoader::LoadLevel spun RemoveLeastUsedModel
	// forever (PC at 'lwz r30,4(r30)', loading bar frozen at half).
	if(CStreaming::ms_endLoadedList.m_prev == nil){ static int said; if(said++ < 3) printf("HEAP: shed refused before streaming init (%u bytes)\n", need); return 0; }
	if(LWP_GetSelf() != gMainLwp){ printf("HEAP: shed refused off the main thread (%uK)\n", need/1024); return 0; }
	inShed = TRUE;
	int dropped = 0;
	bool ready = false;
	int cap = need >= 32*1024 ? 24 : 4;   // B114/B115: 48 stripped whole blocks for a 3K request; a 700K mesh still needs room
	// B177: LOD shells and their TXDs are what the city is drawn with while a
	// detail model is missing. The LRU fallback in RemoveLeastUsedModel took
	// them for ordinary detail loads (b171/b172: 6 LODs among 21 victims, and
	// the 102K od_northstar block still failed), leaving neither. Only what the
	// script is waiting for may spend them.
	uint32 keep = gcEssentialLoad ? 0 : STREAMFLAGS_LOD;
	for(int k = 0; k <= cap; k++){
		void *probe = need >= 1024 ? gcBigAlloc(need) : __real_malloc(need);
		if(probe == NULL && need >= 1024) probe = __real_malloc(need);
		if(probe){
			if(gcBigContains(probe)) gcBigFree(probe); else free(probe);
			ready = true;
			break;
		}
		if(k == cap) break;
		if(!CStreaming::RemoveLeastUsedModel(keep) && !CStreaming::RemoveLeastUsedModel(keep, true)){   // B140: second pass ignores LoadedRecently
			// B149: nothing unreferenced — every building near the scene still
			// holds its RW object (refs > 0). Drop the RW objects behind the
			// camera the way the budget sweep would, then look again. b147:
			// prop 295 (81K) failed 24x with 'shed 0 models' and 2.2MB free.
			CStreaming::DeleteRwObjectsBehindCamera(CStreaming::ms_memoryUsed > 2*need ? CStreaming::ms_memoryUsed - 2*need : 0);
			if(!CStreaming::RemoveLeastUsedModel(keep, true)) break;
		}
		dropped++;
	}
	static u64 lastSaid; static unsigned muted;
	if(dropped || ticks_to_millisecs(gettime() - lastSaid) > 2000){
		printf("HEAP: emergency shed %d models for %uK, free now %uK (%u muted)\n", dropped, need/1024, (unsigned)(gcHeapFreeTotal()/1024), muted);
		lastSaid = gettime(); muted = 0;
	}else muted++;
	inShed = FALSE;
	return ready;
}

// B88 census: what the streaming bytes are (buildings, clumps, vehicles, peds, txds, other).
extern "C" void gcStreamClassCensus(unsigned out[6])
{
	for(int k = 0; k < 6; k++) out[k] = 0;
	for(int32 i = 0; i < NUMSTREAMINFO; i++){
		if(CStreaming::ms_aInfoForModel[i].m_loadState != STREAMSTATE_LOADED) continue;
		uint32 pos, size;
		if(!CStreaming::ms_aInfoForModel[i].GetCdPosnAndSize(pos, size)) continue;
		unsigned bytes = size*CDSTREAM_SECTOR_SIZE;
		int k = 5;
		if(i < STREAM_OFFSET_TXD){
			CBaseModelInfo *mi = CModelInfo::GetModelInfo(i);
			if(mi == nil) continue;
			switch(mi->GetModelType()){
			case MITYPE_SIMPLE: case MITYPE_TIME: k = 0; break;
			case MITYPE_CLUMP: case MITYPE_WEAPON: k = 1; break;
			case MITYPE_VEHICLE: k = 2; break;
			case MITYPE_PED: k = 3; break;
			}
		}else if(i < STREAM_OFFSET_COL) k = 4;
		out[k] += bytes;
	}
}

// B142: the intro's special-character WAIT runs before LOAD_CUTSCENE, so the
// cutscene test alone (B141) never fired; b140/b141 parked at the office with
// 214K free. While the script polls HasSpecialCharLoaded (stamped below) or a
// cutscene is up, just-loaded models may go too.
static inline bool StreamSceneHold(void)
{
	return CCutsceneMgr::IsCutsceneProcessing() || CCutsceneMgr::IsRunning() ||
	       (gSpecialWaitFrame != 0 && (uint32)(CTimer::GetFrameCounter() - gSpecialWaitFrame) < 120);
}

// B148: the docks script also waits on HAS_MODEL_LOADED 295-298 (cutscene
// props) after the special characters; b147 parked there with no diagnostic.
// Called from the script command when the model is not loaded.
extern "C" void gcScriptWaitDiag(int32 m)
{
	if(m < 0 || m >= NUMSTREAMINFO) return;
	gSpecialWaitFrame = CTimer::GetFrameCounter(); if(gSpecialWaitFrame == 0) gSpecialWaitFrame = 1;
	static uint32 lastMs; uint32 now = CTimer::GetTimeInMillisecondsPauseMode();
	if(now - lastMs > 5000){
		lastMs = now;
		printf("STREAM scriptwait model %d state %d flags %x fails %d req %d\n", m, (int)CStreaming::ms_aInfoForModel[m].m_loadState,
		    (unsigned)CStreaming::ms_aInfoForModel[m].m_flags, (int)gLoadFails[m], (int)(CStreaming::ms_aInfoForModel[m].m_next != nil));
	}
}

bool
CStreaming::RemoveLeastUsedModel(uint32 excludeMask, bool ignoreRecent)
{
#ifdef GTA_OGC
	// B125: never a script-owned or DONT_REMOVE model. Every caller here
	// passes 0, so the emergency shed evicted the intro's special characters
	// (109-114, unstamped = "farthest") right after LOAD_ALL_MODELS_NOW; nobody
	// re-requests them and the script's HAS_SPECIAL_CHARACTER_LOADED wait
	// never ends — the black "Vice City Docks" screen (b121/b123, 15+ min).
	excludeMask |= STREAMFLAGS_CANT_REMOVE | STREAMFLAGS_20;
#endif
	CStreamingInfo *si;
	int streamId;

#ifdef GTA_OGC
	if(StreamSceneHold())
		ignoreRecent = true;
#endif

	for(si = ms_endLoadedList.m_prev; si != &ms_startLoadedList; si = si->m_prev){
		if(si->m_flags & excludeMask)
			continue;
		streamId = si - ms_aInfoForModel;
		if(streamId < STREAM_OFFSET_TXD){
#ifdef GTA_OGC
			if(!ignoreRecent && LoadedRecently(streamId))
				continue;
#endif
			if (CModelInfo::GetModelInfo(streamId)->GetNumRefs() == 0) {
				TraceEvict(streamId);
				RemoveModel(streamId);
				return true;
			}
		}else if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL){
			if(CTxdStore::GetNumRefs(streamId - STREAM_OFFSET_TXD) == 0 &&
			   !IsTxdUsedByRequestedModels(streamId - STREAM_OFFSET_TXD)
#ifdef GTA_OGC
			   && !LoadedRecently(streamId)   // B106
#endif
			   ){
				RemoveModel(streamId);
				return true;
			}
		}else if(streamId >= STREAM_OFFSET_ANIM){
			assert(streamId < NUMSTREAMINFO);
			if(CAnimManager::GetNumRefsToAnimBlock(streamId - STREAM_OFFSET_ANIM) == 0 &&
			   !AreAnimsUsedByRequestedModels(streamId - STREAM_OFFSET_ANIM)){
				RemoveModel(streamId);
				return true;
			}
		}
	}
	return (ms_numVehiclesLoaded > 7 || CGame::currArea != AREA_MAIN_MAP && ms_numVehiclesLoaded > 4) && RemoveLoadedVehicle();
}

void
CStreaming::RemoveAllUnusedModels(void)
{
	int i;

	for(i = 0; i < MAXVEHICLESLOADED; i++)
		RemoveLoadedVehicle();

	for(i = NUM_DEFAULT_MODELS; i < MODELINFOSIZE; i++){
		if(ms_aInfoForModel[i].m_loadState == STREAMSTATE_LOADED &&
		    CModelInfo::GetModelInfo(i)->GetNumRefs() == 0) {
			RemoveModel(i);
			ms_aInfoForModel[i].m_loadState = STREAMSTATE_NOTLOADED;
		}
	}
}

void
CStreaming::RemoveUnusedModelsInLoadedList(void)
{
	// empty
}

bool
CStreaming::RemoveLoadedZoneModel(void)
{
	int i;

	if(ms_currentPedGrp == -1)
		return false;

	for(i = 0; i < NUMMODELSPERPEDGROUP; i++){
		int mi = CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i];
		if(mi != -1 && ms_bIsPedFromPedGroupLoaded[i] &&
		   HasModelLoaded(mi) &&  CanRemoveModel(mi) &&
		   CModelInfo::GetModelInfo(mi)->GetNumRefs() == 0){
			RemoveModel(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i]);
			ms_numPedsLoaded--;
			ms_bIsPedFromPedGroupLoaded[i] = false;
			return true;
		}
	}

	return false;
}

bool
CStreaming::IsTxdUsedByRequestedModels(int32 txdId)
{
	CStreamingInfo *si;
	int streamId;
	int i;

	for(si = ms_startRequestedList.m_next; si != &ms_endRequestedList; si = si->m_next){
		streamId = si - ms_aInfoForModel;
		if(streamId < STREAM_OFFSET_TXD &&
		   CModelInfo::GetModelInfo(streamId)->GetTxdSlot() == txdId)
			return true;
	}

	for(i = 0; i < 4; i++){
		streamId = ms_channel[0].streamIds[i];
		if(streamId != -1 && streamId < STREAM_OFFSET_TXD &&
		   CModelInfo::GetModelInfo(streamId)->GetTxdSlot() == txdId)
			return true;
		streamId = ms_channel[1].streamIds[i];
		if(streamId != -1 && streamId < STREAM_OFFSET_TXD &&
		   CModelInfo::GetModelInfo(streamId)->GetTxdSlot() == txdId)
			return true;
	}

	return false;
}

bool
CStreaming::AreAnimsUsedByRequestedModels(int32 animId)
{
	CStreamingInfo *si;
	int streamId;
	int i;

	for(si = ms_startRequestedList.m_next; si != &ms_endRequestedList; si = si->m_next){
		streamId = si - ms_aInfoForModel;
		if(streamId < STREAM_OFFSET_TXD &&
		   CModelInfo::GetModelInfo(streamId)->GetAnimFileIndex() == animId)
			return true;
	}

	for(i = 0; i < 4; i++){
		streamId = ms_channel[0].streamIds[i];
		if(streamId != -1 && streamId < STREAM_OFFSET_TXD &&
		   CModelInfo::GetModelInfo(streamId)->GetAnimFileIndex() == animId)
			return true;
		streamId = ms_channel[1].streamIds[i];
		if(streamId != -1 && streamId < STREAM_OFFSET_TXD &&
		   CModelInfo::GetModelInfo(streamId)->GetAnimFileIndex() == animId)
			return true;
	}

	return false;
}

int32
CStreaming::GetAvailableVehicleSlot(void)
{
	int i;
	for(i = 0; i < MAXVEHICLESLOADED; i++)
		if(ms_vehiclesLoaded[i] == -1)
			return i;
	return -1;
}

bool
CStreaming::AddToLoadedVehiclesList(int32 modelId)
{
	int i;
	int id;

	if(ms_numVehiclesLoaded < desiredNumVehiclesLoaded){
		// still room for vehicles
		for(i = 0; i < MAXVEHICLESLOADED; i++){
			if(ms_vehiclesLoaded[ms_lastVehicleDeleted] == -1)
				break;
			ms_lastVehicleDeleted++;
			if(ms_lastVehicleDeleted == MAXVEHICLESLOADED)
				ms_lastVehicleDeleted = 0;
		}
		assert(ms_vehiclesLoaded[ms_lastVehicleDeleted] == -1);
		ms_numVehiclesLoaded++;
	}else{
		// find vehicle we can remove
		for(i = 0; i < MAXVEHICLESLOADED; i++){
			id = ms_vehiclesLoaded[ms_lastVehicleDeleted];
			if(id != -1 && CanRemoveModel(id) &&
			   CModelInfo::GetModelInfo(id)->GetNumRefs() == 0)
				goto found;
			ms_lastVehicleDeleted++;
			if(ms_lastVehicleDeleted == MAXVEHICLESLOADED)
				ms_lastVehicleDeleted = 0;
		}
		id = -1;
found:
		if(id == -1){
			// didn't find anything, try a free slot
			id = GetAvailableVehicleSlot();
			if(id == -1)
				return false;	// still no luck
			ms_lastVehicleDeleted = id;
			// this is more than we wanted actually
			ms_numVehiclesLoaded++;
		}
		else{
			RemoveModel(id);
			CVehicleModelInfo* pVehicleInfo = (CVehicleModelInfo*)CModelInfo::GetModelInfo(id);
			if (pVehicleInfo->m_vehicleClass != -1)
				CCarCtrl::RemoveFromLoadedVehicleArray(id, pVehicleInfo->m_vehicleClass);
		}
	}

	ms_vehiclesLoaded[ms_lastVehicleDeleted++] = modelId;
	if(ms_lastVehicleDeleted == MAXVEHICLESLOADED)
		ms_lastVehicleDeleted = 0;
	CVehicleModelInfo* pVehicleInfo = (CVehicleModelInfo*)CModelInfo::GetModelInfo(modelId);
	if (pVehicleInfo->m_vehicleClass != -1)
		CCarCtrl::AddToLoadedVehicleArray(modelId, pVehicleInfo->m_vehicleClass, pVehicleInfo->m_frequency);
	return true;
}

bool
CStreaming::IsObjectInCdImage(int32 id)
{
	uint32 posn, size;
	return ms_aInfoForModel[id].GetCdPosnAndSize(posn, size);
}

void
CStreaming::SetModelIsDeletable(int32 id)
{
	ms_aInfoForModel[id].m_flags &= ~STREAMFLAGS_DONT_REMOVE;
	if ((id >= STREAM_OFFSET_TXD && id < STREAM_OFFSET_COL || CModelInfo::GetModelInfo(id)->GetModelType() != MITYPE_VEHICLE) &&
	   (ms_aInfoForModel[id].m_flags & STREAMFLAGS_SCRIPTOWNED) == 0){
		if(ms_aInfoForModel[id].m_loadState != STREAMSTATE_LOADED)
			RemoveModel(id);
		else if(ms_aInfoForModel[id].m_next == nil)
			ms_aInfoForModel[id].AddToList(&ms_startLoadedList);
	}
}

void
CStreaming::SetModelTxdIsDeletable(int32 id)
{
	SetModelIsDeletable(CModelInfo::GetModelInfo(id)->GetTxdSlot() + STREAM_OFFSET_TXD);
}

void
CStreaming::SetMissionDoesntRequireModel(int32 id)
{
	ms_aInfoForModel[id].m_flags &= ~STREAMFLAGS_SCRIPTOWNED;
	if ((id >= STREAM_OFFSET_TXD || CModelInfo::GetModelInfo(id)->GetModelType() != MITYPE_VEHICLE) &&
	   (ms_aInfoForModel[id].m_flags & STREAMFLAGS_DONT_REMOVE) == 0){
		if(ms_aInfoForModel[id].m_loadState != STREAMSTATE_LOADED)
			RemoveModel(id);
		else if(ms_aInfoForModel[id].m_next == nil)
			ms_aInfoForModel[id].AddToList(&ms_startLoadedList);
	}
}

void
CStreaming::LoadInitialPeds(void)
{
	RequestModel(MI_COP, STREAMFLAGS_DONT_REMOVE);
	RequestModel(MI_MALE01, STREAMFLAGS_DONT_REMOVE);
	RequestModel(MI_TAXI_D, STREAMFLAGS_DONT_REMOVE);
}

void
CStreaming::LoadInitialWeapons(void)
{
	CStreaming::RequestModel(MI_NIGHTSTICK, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_MISSILE, STREAMFLAGS_DONT_REMOVE);
}

void
CStreaming::LoadInitialVehicles(void)
{
	ms_numVehiclesLoaded = 0;
	ms_lastVehicleDeleted = 0;

	RequestModel(MI_POLICE, STREAMFLAGS_DONT_REMOVE);
}

void
CStreaming::StreamVehiclesAndPeds(void)
{
	int i, model;
	static int timeBeforeNextLoad = 0;
	static int modelQualityClass = 0;

	if(CRecordDataForGame::IsRecording() ||
	   CRecordDataForGame::IsPlayingBack()
#ifdef FIX_BUGS
	   || CReplay::IsPlayingBack()
#endif
		)
		return;

	if(FindPlayerPed()->m_pWanted->AreSwatRequired()){
		RequestModel(MI_ENFORCER, STREAMFLAGS_DONT_REMOVE);
		RequestModel(MI_SWAT, STREAMFLAGS_DONT_REMOVE);
	}else{
		SetModelIsDeletable(MI_ENFORCER);
		if(!HasModelLoaded(MI_ENFORCER))
			SetModelIsDeletable(MI_SWAT);
	}

	if(FindPlayerPed()->m_pWanted->AreFbiRequired()){
		RequestModel(MI_FBIRANCH, STREAMFLAGS_DONT_REMOVE);
		RequestModel(MI_FBI, STREAMFLAGS_DONT_REMOVE);
	}else{
		SetModelIsDeletable(MI_FBIRANCH);
		if(!HasModelLoaded(MI_FBIRANCH))
			SetModelIsDeletable(MI_FBI);
	}

	if(FindPlayerPed()->m_pWanted->AreArmyRequired()){
		RequestModel(MI_RHINO, STREAMFLAGS_DONT_REMOVE);
		RequestModel(MI_BARRACKS, STREAMFLAGS_DONT_REMOVE);
		RequestModel(MI_ARMY, STREAMFLAGS_DONT_REMOVE);
	}else{
		SetModelIsDeletable(MI_RHINO);
		SetModelIsDeletable(MI_BARRACKS);
		if(!HasModelLoaded(MI_RHINO) && !HasModelLoaded(MI_BARRACKS))
			SetModelIsDeletable(MI_ARMY);
	}

	if(FindPlayerPed()->m_pWanted->NumOfHelisRequired() > 0)
		RequestModel(MI_CHOPPER, STREAMFLAGS_DONT_REMOVE);
	else
		SetModelIsDeletable(MI_CHOPPER);

	if (FindPlayerPed()->m_pWanted->AreMiamiViceRequired()) {
		SetModelIsDeletable(MI_VICE1);
		SetModelIsDeletable(MI_VICE2);
		SetModelIsDeletable(MI_VICE3);
		SetModelIsDeletable(MI_VICE4);
		SetModelIsDeletable(MI_VICE5);
		SetModelIsDeletable(MI_VICE6);
		SetModelIsDeletable(MI_VICE7);
		SetModelIsDeletable(MI_VICE8);
		RequestModel(MI_VICECHEE, STREAMFLAGS_DONT_REMOVE);
		if(CPopulation::NumMiamiViceCops == 0)
			switch (CCarCtrl::MiamiViceCycle) {
			case 0:
				RequestModel(MI_VICE1, STREAMFLAGS_DONT_REMOVE);
				RequestModel(MI_VICE2, STREAMFLAGS_DONT_REMOVE);
				break;
			case 1:
				RequestModel(MI_VICE3, STREAMFLAGS_DONT_REMOVE);
				RequestModel(MI_VICE4, STREAMFLAGS_DONT_REMOVE);
				break;
			case 2:
				RequestModel(MI_VICE5, STREAMFLAGS_DONT_REMOVE);
				RequestModel(MI_VICE6, STREAMFLAGS_DONT_REMOVE);
				break;
			case 3:
				RequestModel(MI_VICE7, STREAMFLAGS_DONT_REMOVE);
				RequestModel(MI_VICE8, STREAMFLAGS_DONT_REMOVE);
				break;
			}
	}
	else {
		SetModelIsDeletable(MI_VICECHEE);
		SetModelIsDeletable(MI_VICE1);
		SetModelIsDeletable(MI_VICE2);
		SetModelIsDeletable(MI_VICE3);
		SetModelIsDeletable(MI_VICE4);
		SetModelIsDeletable(MI_VICE5);
		SetModelIsDeletable(MI_VICE6);
		SetModelIsDeletable(MI_VICE7);
		SetModelIsDeletable(MI_VICE8);
	}

	if(timeBeforeNextLoad >= 0)
		timeBeforeNextLoad--;
	else if(ms_numVehiclesLoaded <= desiredNumVehiclesLoaded){
		CZoneInfo zone;
		CVector coors = FindPlayerCoors();
		CTheZones::GetZoneInfoForTimeOfDay(&coors, &zone);
		int32 maxReq = -1;
		int32 mostRequestedRating = 0;
		for(i = 0; i < CCarCtrl::TOTAL_CUSTOM_CLASSES; i++){
			if(CCarCtrl::NumRequestsOfCarRating[i] > maxReq &&
				((i == 0 && zone.carThreshold[0] != 0) ||
#ifdef FIX_BUGS
				(i < CCarCtrl::NUM_CAR_CLASSES && zone.carThreshold[i] != zone.carThreshold[i-1]) ||
				(i == CCarCtrl::NUM_CAR_CLASSES && zone.boatThreshold[i - CCarCtrl::NUM_CAR_CLASSES] != 0) ||
				(i > CCarCtrl::NUM_CAR_CLASSES && i < CCarCtrl::TOTAL_CUSTOM_CLASSES && zone.boatThreshold[i - CCarCtrl::NUM_CAR_CLASSES] != zone.boatThreshold[i - CCarCtrl::NUM_CAR_CLASSES - 1]))) {
#else
				(i != 0 && zone.carThreshold[i] != zone.carThreshold[i-1]))) {
#endif
				maxReq = CCarCtrl::NumRequestsOfCarRating[i];
				mostRequestedRating = i;
			}
		}
		model = CCarCtrl::ChooseCarModelToLoad(mostRequestedRating);
		if(!HasModelLoaded(model)){
			RequestModel(model, STREAMFLAGS_DEPENDENCY);
			timeBeforeNextLoad = 350;
		}
		CCarCtrl::NumRequestsOfCarRating[mostRequestedRating] = 0;
	}
}

void
CStreaming::StreamZoneModels(const CVector &pos)
{
	int i, j;
	uint16 gangsToLoad, gangCarsToLoad, bit;
	CZoneInfo info;
	static int timeBeforeNextLoad = 0;

	CTheZones::GetZoneInfoForTimeOfDay(&pos, &info);

	if(info.pedGroup != ms_currentPedGrp){

		// unload pevious group
		if(ms_currentPedGrp != -1)
			for(i = 0; i < NUMMODELSPERPEDGROUP; i++){
				ms_bIsPedFromPedGroupLoaded[i] = false;
				if(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i] != -1){
					SetModelIsDeletable(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i]);
					SetModelTxdIsDeletable(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i]);
				}
			}

		ms_currentPedGrp = info.pedGroup;

		for(i = 0; i < MAXZONEPEDSLOADED; i++){
			do
				j = CGeneral::GetRandomNumberInRange(0, NUMMODELSPERPEDGROUP);
			while(ms_bIsPedFromPedGroupLoaded[j]);
			ms_bIsPedFromPedGroupLoaded[j] = true;
			if(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[j] != -1)
				RequestModel(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[j], STREAMFLAGS_DEPENDENCY);
		}
		ms_numPedsLoaded = MAXZONEPEDSLOADED;
		timeBeforeNextLoad = 300;
	}

	if(timeBeforeNextLoad >= 0)
		timeBeforeNextLoad--;
	else{
		// Switch a ped
		int oldMI;
		// Find a ped to unload
		for(i = 0; i < NUMMODELSPERPEDGROUP; i++)
			if(ms_bIsPedFromPedGroupLoaded[i]){
				oldMI = CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i];
				if(oldMI != -1 && CModelInfo::GetModelInfo(oldMI)->GetNumRefs() == 0)
					break;
			}
		// And load a new one
		if(i != NUMMODELSPERPEDGROUP || ms_numPedsLoaded < MAXZONEPEDSLOADED){
			do
				j = CGeneral::GetRandomNumberInRange(0, NUMMODELSPERPEDGROUP);
			while(ms_bIsPedFromPedGroupLoaded[j]);
			if(ms_numPedsLoaded == MAXZONEPEDSLOADED)
				ms_bIsPedFromPedGroupLoaded[i] = false;
			ms_bIsPedFromPedGroupLoaded[j] = true;
			int newMI = CPopulation::ms_pPedGroups[ms_currentPedGrp].models[j];
			if(newMI != oldMI){
				RequestModel(newMI, STREAMFLAGS_DEPENDENCY);
				debug("Request Ped %s\n", CModelInfo::GetModelInfo(newMI)->GetModelName());
				if(ms_numPedsLoaded == MAXZONEPEDSLOADED){
					SetModelIsDeletable(oldMI);
					SetModelTxdIsDeletable(oldMI);
					debug("Remove Ped %s\n", CModelInfo::GetModelInfo(oldMI)->GetModelName());
				}else
					ms_numPedsLoaded++;
				timeBeforeNextLoad = 300;
			}
		}
	}

	RequestModel(MI_MALE01, STREAMFLAGS_DONT_REMOVE);
	RequestModel(MI_TAXI_D, STREAMFLAGS_DONT_REMOVE);

	gangsToLoad = 0;
	gangCarsToLoad = 0;
	if(info.gangPedThreshold[0] != info.copPedThreshold)
		gangsToLoad = 1;
	for(i = 1; i < NUM_GANGS; i++)
		if(info.gangPedThreshold[i] != info.gangPedThreshold[i-1])
			gangsToLoad |= 1<<i;
	if(info.gangThreshold[0] != info.copThreshold)
		gangCarsToLoad = 1;
	for(i = 1; i < NUM_GANGS; i++)
		if(info.gangThreshold[i] != info.gangThreshold[i-1])
			gangCarsToLoad |= 1<<i;

	if(gangsToLoad == ms_loadedGangs && gangCarsToLoad == ms_loadedGangCars)
		return;

	int gangModelsToload = gangsToLoad | gangCarsToLoad;

	if(gangsToLoad != ms_loadedGangs || gangCarsToLoad != ms_loadedGangCars){
		for(i = 0; i < NUM_GANGS; i++){
			bit = 1<<i;

			if(gangModelsToload & bit && (ms_loadedGangs & bit) == 0){
				RequestModel(CGangs::GetGangPedModel1(i), STREAMFLAGS_DEPENDENCY);
				RequestModel(CGangs::GetGangPedModel2(i), STREAMFLAGS_DEPENDENCY);
				ms_loadedGangs |= bit;
			}else if((gangModelsToload & bit) == 0 && ms_loadedGangs & bit){
				SetModelIsDeletable(CGangs::GetGangPedModel1(i));
				SetModelIsDeletable(CGangs::GetGangPedModel2(i));
				SetModelTxdIsDeletable(CGangs::GetGangPedModel1(i));
				SetModelTxdIsDeletable(CGangs::GetGangPedModel2(i));
				ms_loadedGangs &= ~bit;
			}

			if(CGangs::GetGangVehicleModel(i) != -1){
				if((gangCarsToLoad & bit) && (ms_loadedGangCars & bit) == 0){
					RequestModel(CGangs::GetGangVehicleModel(i), STREAMFLAGS_DEPENDENCY);
				}else if((gangCarsToLoad & bit) == 0 && ms_loadedGangCars & bit){
					SetModelIsDeletable(CGangs::GetGangVehicleModel(i));
					SetModelTxdIsDeletable(CGangs::GetGangVehicleModel(i));
				}
			}
		}
		ms_loadedGangCars = gangCarsToLoad;
	}
}

void
CStreaming::RemoveCurrentZonesModels(void)
{
	int i;

	if (ms_currentPedGrp != -1)
		for (i = 0; i < NUMMODELSPERPEDGROUP; i++) {
			ms_bIsPedFromPedGroupLoaded[i] = false;
			if (CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i] != -1) {
				SetModelIsDeletable(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i]);
				SetModelTxdIsDeletable(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i]);
			}
		}

	CStreaming::RequestModel(MI_MALE01, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_TAXI_D, STREAMFLAGS_DONT_REMOVE);

	for (i = 0; i < NUM_GANGS; i++) {
		if (CGangs::GetGangPedModel1(i) != -1) {
			SetModelIsDeletable(CGangs::GetGangPedModel1(i));
			SetModelTxdIsDeletable(CGangs::GetGangPedModel1(i));
		}
		if (CGangs::GetGangPedModel2(i) != -1) {
			SetModelIsDeletable(CGangs::GetGangPedModel2(i));
			SetModelTxdIsDeletable(CGangs::GetGangPedModel2(i));
		}
		if (CGangs::GetGangVehicleModel(i) != -1) {
			SetModelIsDeletable(CGangs::GetGangVehicleModel(i));
			SetModelTxdIsDeletable(CGangs::GetGangVehicleModel(i));
		}
	}

	ms_currentPedGrp = -1;
	ms_loadedGangs = 0;
	ms_loadedGangCars = 0;
}

void
CStreaming::LoadBigBuildingsWhenNeeded(void)
{
	// Very much like CCollision::Update and CCollision::LoadCollisionWhenINeedIt
	if(CCutsceneMgr::IsCutsceneProcessing())
		return;

	if(CTheZones::m_CurrLevel == LEVEL_GENERIC || 
	   CTheZones::m_CurrLevel == CGame::currLevel)
		return;

	CTimer::Suspend();
	CGame::currLevel = CTheZones::m_CurrLevel;
	ISLAND_LOADING_IS(LOW)
	{
		DMAudio.SetEffectsFadeVol(0);
		CPad::StopPadsShaking();
		CCollision::LoadCollisionScreen(CGame::currLevel);
		DMAudio.Service();

		RemoveUnusedBigBuildings(CGame::currLevel);
		RemoveUnusedBuildings(CGame::currLevel);
		RemoveUnusedModelsInLoadedList();
		CGame::TidyUpMemory(true, true);
	}
	CReplay::EmptyReplayBuffer();
	if(CGame::currLevel != LEVEL_GENERIC)
		LoadSplash(GetLevelSplashScreen(CGame::currLevel));

	ISLAND_LOADING_IS(LOW)
		CStreaming::RequestBigBuildings(CGame::currLevel, TheCamera.GetPosition());
#ifdef NO_ISLAND_LOADING
	else if(FrontEndMenuManager.m_PrefsIslandLoading == CMenuManager::ISLAND_LOADING_MEDIUM) {
		RemoveIslandsNotUsed(CGame::currLevel);
		CStreaming::RequestIslands(CGame::currLevel);
	}
#endif

	CStreaming::LoadAllRequestedModels(false);

	CGame::TidyUpMemory(true, true);
	CTimer::Resume();

	ISLAND_LOADING_IS(LOW)
		DMAudio.SetEffectsFadeVol(127);
}


// Find starting offset of the cdimage we next want to read
// Not useful at all on PC...
int32
CStreaming::GetCdImageOffset(int32 lastPosn)
{
	int offset, off;
	int i, img;
	int dist, mindist;

	img = -1;
	mindist = INT32_MAX;
	offset = ms_imageOffsets[ms_lastImageRead];
	if(lastPosn <= offset || lastPosn > offset + ms_imageSize){
		// last read position is not in last image
		for(i = 0; i < NUMCDIMAGES; i++){
			off = ms_imageOffsets[i];
			if(off == -1) continue;
			if((uint32)lastPosn > (uint32)off)
				// after start of image, get distance from end
				// negative if before end!
				dist = lastPosn - (off + ms_imageSize);
			else
				// before image, get offset to start
				// this will never be negative
				dist = off - lastPosn;
			if(dist < mindist){
				img = i;
				mindist = dist;
			}
		}
		assert(img >= 0);
		offset = ms_imageOffsets[img];
		ms_lastImageRead = img;
	}
	return offset;
}

inline bool
ModelNotLoaded(int32 modelId)
{
	CStreamingInfo *si = &CStreaming::ms_aInfoForModel[modelId];
	return si->m_loadState != STREAMSTATE_LOADED && si->m_loadState != STREAMSTATE_READING;
}

inline bool TxdNotLoaded(int32 txdId) { return ModelNotLoaded(txdId + STREAM_OFFSET_TXD); }
inline bool AnimNotLoaded(int32 animId) { return animId != -1 && ModelNotLoaded(animId + STREAM_OFFSET_ANIM); }

// Find stream id of next requested file in cdimage
int32
CStreaming::GetNextFileOnCd(int32 lastPosn, bool priority)
{
	CStreamingInfo *si, *next;
	bool dependenciesChanged = false;
	int streamId;
	uint32 posn, size;
	int streamIdFirst, streamIdNext;
	uint32 posnFirst, posnNext;
#ifdef GTA_OGC
	bool lodPass = HasPendingLods();
#endif

	streamIdFirst = -1;
	streamIdNext = -1;
	posnFirst = UINT32_MAX;
	posnNext = UINT32_MAX;

	for(si = ms_startRequestedList.m_next; si != &ms_endRequestedList; si = next){
		next = si->m_next;
		streamId = si - ms_aInfoForModel;

#ifdef GTA_OGC
		if(lodPass && !LodPassAllows(streamId))
			continue;
#endif
		// only priority requests if there are any
		if(priority && ms_numPriorityRequests != 0 && !si->IsPriority())
			continue;

		// request Txds or anims if necessary
		if(streamId < STREAM_OFFSET_TXD){
			int txdId = CModelInfo::GetModelInfo(streamId)->GetTxdSlot();
			if(TxdNotLoaded(txdId)){
				CStreamingInfo &dependency = ms_aInfoForModel[txdId + STREAM_OFFSET_TXD];
				int state = dependency.m_loadState, flags = dependency.m_flags;
				ReRequestModel(txdId + STREAM_OFFSET_TXD, si->m_flags & (STREAMFLAGS_PRIORITY | STREAMFLAGS_LOD));
				dependenciesChanged |= state != dependency.m_loadState || flags != dependency.m_flags;
				continue;
			}
			int animId = CModelInfo::GetModelInfo(streamId)->GetAnimFileIndex();
			if(AnimNotLoaded(animId)){
				CStreamingInfo &dependency = ms_aInfoForModel[animId + STREAM_OFFSET_ANIM];
				int state = dependency.m_loadState, flags = dependency.m_flags;
				ReRequestModel(animId + STREAM_OFFSET_ANIM, si->m_flags & (STREAMFLAGS_PRIORITY | STREAMFLAGS_LOD));
				dependenciesChanged |= state != dependency.m_loadState || flags != dependency.m_flags;
				continue;
			}
		}else if(streamId >= STREAM_OFFSET_ANIM && CCutsceneMgr::IsCutsceneProcessing())
			continue;

		if(ms_aInfoForModel[streamId].GetCdPosnAndSize(posn, size)){
			if(posn < posnFirst){
				// find first requested file in image
				streamIdFirst = streamId;
				posnFirst = posn;
			}
			if(posn < posnNext && posn >= (uint32)lastPosn){
				// find first requested file after last read position
				streamIdNext = streamId;
				posnNext = posn;
			}
		}else{
			// empty file
			DecrementRef(streamId);
			si->RemoveFromList();
			si->m_loadState = STREAMSTATE_LOADED;
		}
	}

	if(dependenciesChanged)
		return GetNextFileOnCd(lastPosn, priority);

	// wrap around
	if(streamIdNext == -1)
		streamIdNext = streamIdFirst;

#ifdef GTA_OGC
	if(streamIdNext == -1 && lodPass)
		return HasPendingLods() ? -1 : GetNextFileOnCd(lastPosn, priority);
#endif
	if(streamIdNext == -1 && ms_numPriorityRequests != 0){
		// try non-priority files
		ms_numPriorityRequests = 0;
		streamIdNext = GetNextFileOnCd(lastPosn, false);
	}

	return streamIdNext;
}

/*
 * Streaming buffer size is half of the largest file.
 * Files larger than the buffer size can only be loaded by channel 0,
 * which then uses both buffers, while channel 1 is idle.
 * ms_bLoadingBigModel is set to true to indicate this state.
 */

// Make channel read from disc
void
CStreaming::RequestModelStream(int32 ch)
{
	int lastPosn, imgOffset, streamId;
	int totalSize;
	uint32 posn, size, unused;
	int i;
	int haveBigFile, havePed;

	lastPosn = CdStreamGetLastPosn();
	imgOffset = GetCdImageOffset(lastPosn);
	streamId = GetNextFileOnCd(lastPosn - imgOffset, true);

	// remove Txds and Anims that aren't requested anymore
	while(streamId != -1){
		if(ms_aInfoForModel[streamId].m_flags & STREAMFLAGS_KEEP_IN_MEMORY)
			break;
		if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL){
			if(IsTxdUsedByRequestedModels(streamId - STREAM_OFFSET_TXD))
				break;
		}else if(streamId >= STREAM_OFFSET_ANIM){
			assert(streamId < NUMSTREAMINFO);
			if(AreAnimsUsedByRequestedModels(streamId - STREAM_OFFSET_ANIM))
				break;
		}else
			break;
		RemoveModel(streamId);
		// so try next file
		ms_aInfoForModel[streamId].GetCdPosnAndSize(posn, size);
		streamId = GetNextFileOnCd(posn + size, true);
	}

	if(streamId == -1)
		return;

	ms_aInfoForModel[streamId].GetCdPosnAndSize(posn, size);
	if(size > (uint32)ms_streamingBufferSize){
		// Can only load big models on channel 0, and 1 has to be idle
		if(ch == 1 || ms_channel[1].state != CHANNELSTATE_IDLE)
			return;
		ms_bLoadingBigModel = true;
	}

	// Load up to 4 adjacent files
#ifdef GTA_OGC
	bool lodPass = HasPendingLods();
#endif
	haveBigFile = 0;
	havePed = 0;
	totalSize = 0;
	for(i = 0; i < 4; i++){
#ifdef GTA_OGC
		// B177: a neighbour nobody asked for used to end the batch, so the next
		// requested file 20K further on became its own command: a seek on the
		// drive (35-85 ms plus up to a 35 ms rotation). Reading through up to
		// STREAM_GAP_SECTORS of unrequested files is ~40 ms of transfer into a
		// buffer we already own; the bytes are simply not converted.
		enum { STREAM_GAP_SECTORS = 64 };   // 128K; break-even against a short seek is ~180K
		if(i > 0 && streamId != -1 && ms_aInfoForModel[streamId].m_loadState != STREAMSTATE_INQUEUE){
			int32 id = streamId;
			uint32 gap = 0, gapPosn, gapSize;
			while(id != -1 && ms_aInfoForModel[id].m_loadState != STREAMSTATE_INQUEUE &&
			      ms_aInfoForModel[id].GetCdPosnAndSize(gapPosn, gapSize) &&
			      (gap += gapSize) <= STREAM_GAP_SECTORS)
				id = ms_aInfoForModel[id].m_nextID;
			if(id != -1 && gap <= STREAM_GAP_SECTORS)
				streamId = id;
		}
#endif
		// no more files we can read
		if(streamId == -1 || ms_aInfoForModel[streamId].m_loadState != STREAMSTATE_INQUEUE)
			break;
#ifdef GTA_OGC
		if(lodPass && !LodPassAllows(streamId))
			break;
#endif

		// also stop at non-priority files
		ms_aInfoForModel[streamId].GetCdPosnAndSize(unused, size);
		if(ms_numPriorityRequests != 0 && !ms_aInfoForModel[streamId].IsPriority())
			break;

		// Can't load certain combinations of files together
		if(streamId < STREAM_OFFSET_TXD){
			if (havePed && CModelInfo::GetModelInfo(streamId)->GetModelType() == MITYPE_PED ||
			    haveBigFile && CModelInfo::GetModelInfo(streamId)->GetModelType() == MITYPE_VEHICLE ||
			    TxdNotLoaded(CModelInfo::GetModelInfo(streamId)->GetTxdSlot()) ||
			    AnimNotLoaded(CModelInfo::GetModelInfo(streamId)->GetAnimFileIndex()))
				break;
		}else{
			if(haveBigFile && size > 200)
				break;
		}

		// Now add the file
#ifdef GTA_OGC
		// B177: offsets are disc positions from the first file, so a gap read
		// through above lands every file where ProcessLoadingChannel looks.
		int32 at = (int32)(unused - posn);
		if(i > 0 && (unused < posn || at < totalSize || at + (int32)size > ms_streamingBufferSize))
			break;
		ms_channel[ch].streamIds[i] = streamId;
		ms_channel[ch].offsets[i] = at;
		totalSize = at + size;
#else
		ms_channel[ch].streamIds[i] = streamId;
		ms_channel[ch].offsets[i] = totalSize;
		totalSize += size;

		// To big for buffer, remove again
		if(totalSize > ms_streamingBufferSize && i > 0){
			totalSize -= size;
			break;
		}
#endif
		if(streamId < STREAM_OFFSET_TXD){
			if (CModelInfo::GetModelInfo(streamId)->GetModelType() == MITYPE_PED)
				havePed = 1;
			if (CModelInfo::GetModelInfo(streamId)->GetModelType() == MITYPE_VEHICLE)
				haveBigFile = 1;
		}else{
			if(size > 200)
				haveBigFile = 1;
		}
		ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_READING;
		ms_aInfoForModel[streamId].RemoveFromList();
		DecrementRef(streamId);

		streamId = ms_aInfoForModel[streamId].m_nextID;
	}

	// clear remaining slots
	for(; i < 4; i++)
		ms_channel[ch].streamIds[i] = -1;
	// Now read the data
	assert(!(ms_bLoadingBigModel && ch == 1));	// this would clobber the buffer
#ifdef GTA_OGC
	if(totalSize > 2*ms_streamingBufferSize)
		totalSize = 2*ms_streamingBufferSize;   // the rest streams through the pair, see CdPullStream
#endif
	if(CdStreamRead(ch, ms_pStreamingBuffer[ch], imgOffset+posn, totalSize) == STREAM_NONE)
		debug("FUCKFUCKFUCK\n");
	ms_channel[ch].state = CHANNELSTATE_READING;
	ms_channel[ch].field24 = 0;
	ms_channel[ch].size = totalSize;
	ms_channel[ch].position = imgOffset+posn;
	ms_channel[ch].numTries = 0;
}

// Load data previously read from disc
bool
CStreaming::ProcessLoadingChannel(int32 ch)
{
	int status;
	int i, id, cdsize;

	status = CdStreamGetStatus(ch);
	if(status != STREAM_NONE){
		// busy
		if(status != STREAM_READING && status != STREAM_WAITING){
			ms_channelError = ch;
			ms_channel[ch].state = CHANNELSTATE_ERROR;
			ms_channel[ch].status = status;
		}
		return false;
	}

	if(ms_channel[ch].state == CHANNELSTATE_STARTED){
		ms_channel[ch].state = CHANNELSTATE_IDLE;
		FinishLoadingLargeFile(&ms_pStreamingBuffer[ch][ms_channel[ch].offsets[0]*CDSTREAM_SECTOR_SIZE],
			ms_channel[ch].streamIds[0]);
		ms_channel[ch].streamIds[0] = -1;
	}else{
		ms_channel[ch].state = CHANNELSTATE_IDLE;
		for(i = 0; i < 4; i++){
			id = ms_channel[ch].streamIds[i];
			if(id == -1)
				continue;

			cdsize = ms_aInfoForModel[id].GetCdSize();
			if(id < STREAM_OFFSET_TXD && CModelInfo::GetModelInfo(id)->GetModelType() == MITYPE_VEHICLE &&
			   ms_numVehiclesLoaded >= desiredNumVehiclesLoaded &&
			   !RemoveLoadedVehicle() &&
			   (CanRemoveModel(id) || GetAvailableVehicleSlot() == -1)){
				// can't load vehicle
				RemoveModel(id);
				if(!CanRemoveModel(id))
					ReRequestModel(id);
				else if(CTxdStore::GetNumRefs(CModelInfo::GetModelInfo(id)->GetTxdSlot()) == 0)
					RemoveTxd(CModelInfo::GetModelInfo(id)->GetTxdSlot());
			}else{
				MakeSpaceFor(id >= STREAM_OFFSET_TXD && id < STREAM_OFFSET_COL ? 4*1024 : cdsize * CDSTREAM_SECTOR_SIZE);
#ifdef GTA_OGC
				gPullCd = ms_channel[ch].position + ms_channel[ch].offsets[i];
#endif
				ConvertBufferToObject(&ms_pStreamingBuffer[ch][ms_channel[ch].offsets[i]*CDSTREAM_SECTOR_SIZE],
					id);
				gcHeapCheck(0x434F4E56, id);
				if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_STARTED){
					// queue for second part
					ms_channel[ch].state = CHANNELSTATE_STARTED;
					ms_channel[ch].offsets[0] = ms_channel[ch].offsets[i];
					ms_channel[ch].streamIds[0] = id;
					if(i != 0)
						ms_channel[ch].streamIds[i] = -1;
				}else
					ms_channel[ch].streamIds[i] = -1;
			}
		}
	}

	if(ms_bLoadingBigModel && ms_channel[ch].state != CHANNELSTATE_STARTED){
		ms_bLoadingBigModel = false;
		// reset channel 1 after loading a big model
		for(i = 0; i < 4; i++)
			ms_channel[1].streamIds[i] = -1;
		ms_channel[1].state = CHANNELSTATE_IDLE;
	}

	return true;
}

void
CStreaming::RetryLoadFile(int32 ch)
{
	Const char *key;

	CPad::StopPadsShaking();

	if(ms_channel[ch].numTries >= 3){
		switch(ms_channel[ch].status){
		case STREAM_ERROR_NOCD: key = "NOCD"; break;
		case STREAM_ERROR_OPENCD: key = "OPENCD"; break;
		case STREAM_ERROR_WRONGCD: key = "WRONGCD"; break;
		default: key = "CDERROR"; break;
		}
		CHud::SetMessage(TheText.Get(key));
		CTimer::SetCodePause(true);
	}

	switch(ms_channel[ch].state){
	case CHANNELSTATE_ERROR:
		ms_channel[ch].numTries++;
		if (CdStreamGetStatus(ch) == STREAM_READING || CdStreamGetStatus(ch) == STREAM_WAITING) break;
	case CHANNELSTATE_IDLE:
		CdStreamRead(ch, ms_pStreamingBuffer[ch], ms_channel[ch].position, ms_channel[ch].size);
		ms_channel[ch].state = CHANNELSTATE_READING;
		ms_channel[ch].field24 = -600;
		break;
	case CHANNELSTATE_READING:
		if(ProcessLoadingChannel(ch)){
			ms_channelError = -1;
			CTimer::SetCodePause(false);
		}
		break;
	}
}

void
CStreaming::LoadRequestedModels(void)
{
	static int currentChannel = 0;

	// We can't read with channel 1 while channel 0 is using its buffer
	if(ms_bLoadingBigModel)
		currentChannel = 0;

	// We have data, load
	if(ms_channel[currentChannel].state == CHANNELSTATE_READING ||
	   ms_channel[currentChannel].state == CHANNELSTATE_STARTED)
		ProcessLoadingChannel(currentChannel);

	if(ms_channelError == -1){
		// Channel is idle, read more data
		if(ms_channel[currentChannel].state == CHANNELSTATE_IDLE
#ifdef GTA_OGC
		   && (!STREAM_FLOOR_ENABLED || gStreamHeapFree >= STREAM_HEAP_FLOOR || ms_numPriorityRequests > 0)
#endif
		   )
			RequestModelStream(currentChannel);
		// Switch channel
		if(ms_channel[currentChannel].state != CHANNELSTATE_STARTED)
			currentChannel = 1 - currentChannel;
	}
}


// Let's load models in 4 threads; when one of them becomes idle, process the file, and fill thread with another file. Unfortunately processing models are still single-threaded.
// Currently only supported on POSIX streamer.
// WIP - some files are loaded swapped (CdStreamPosix problem?)
#if 0 //def ONE_THREAD_PER_CHANNEL
void
CStreaming::LoadAllRequestedModels(bool priority)
{
	static bool bInsideLoadAll = false;
	int imgOffset, streamId, status;
	int i;
	uint32 posn, size;

	if(bInsideLoadAll)
		return;
	bInsideLoadAll = true;
	gMainWhere = "load-all"; gLoadAllN++;

	FlushChannels();
	imgOffset = GetCdImageOffset(CdStreamGetLastPosn());

	int streamIds[ARRAY_SIZE(ms_pStreamingBuffer)];
	int streamSizes[ARRAY_SIZE(ms_pStreamingBuffer)];
	int streamPoses[ARRAY_SIZE(ms_pStreamingBuffer)];
	int readOrder[4] = {-1}; // Channel IDs ordered by read time
	int readI = 0;
	int processI = 0;
	bool first = true;

	// All those "first" checks are because of variables aren't initialized in first pass.

	// B139: bounded. b137 parked main inside this loop for good (watchdog
	// 'frozen 104s at [cd-sync]', worker and DVD idle): a request that can
	// never convert keeps being re-requested. After 20 s of wall time the
	// pass ends; whatever is left loads through the per-frame path or not at all.
	u64 loadAllStart = gettime();
	while (true) {
		if(gcBootDone() && ticks_to_millisecs(gettime() - loadAllStart) > 20000){   // B144: the boot's LoadLevel pass may legitimately take longer
			printf("STREAM load-all: 20 s, %d requests left, giving up this pass\n", (int)ms_numModelsRequested);
			for(int c = 0; c < ARRAY_SIZE(ms_pStreamingBuffer); c++) if(!first && streamIds[c] != -1) CdStreamSync(c);
			break;
		}
		for (int i=0; i<ARRAY_SIZE(ms_pStreamingBuffer); i++) {

			// Channel has file to load
			if (!first && streamIds[i] != -1) {
				continue;
			}

			if(ms_endRequestedList.m_prev != &ms_startRequestedList){
				streamId = GetNextFileOnCd(0, priority);
				if(streamId == -1){
					streamIds[i] = -1;
					break;
				}

				if (ms_aInfoForModel[streamId].GetCdPosnAndSize(posn, size)) {
					streamIds[i] = -1;

					// Big file, needs 2 buffer
					if (size > (uint32)ms_streamingBufferSize) {
						if (i + 1 == ARRAY_SIZE(ms_pStreamingBuffer))
							break;
						else if (!first && streamIds[i+1] != -1)
							continue;

					} else {
						// Buffer of current channel is part of a "big file", pass
						if (i != 0 && streamIds[i-1] != -1 && streamSizes[i-1] > (uint32)ms_streamingBufferSize)
							continue;
					}
					ms_aInfoForModel[streamId].RemoveFromList();
					DecrementRef(streamId);

					streamIds[i] = streamId;
					streamSizes[i] = size;
					streamPoses[i] = posn;

					if (!first)
						assert(readOrder[readI] == -1);

					//printf("read: order %d, ch %d, id %d, size %d\n", readI, i, streamId, size);

					CdStreamRead(i, ms_pStreamingBuffer[i], imgOffset+posn, size);
					readOrder[readI] = i;
					if (first && readI+1 != ARRAY_SIZE(readOrder))
						readOrder[readI+1] = -1;

					readI = (readI + 1) % ARRAY_SIZE(readOrder);
				} else {
					ms_aInfoForModel[streamId].RemoveFromList();
					DecrementRef(streamId);

					ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_LOADED;
					streamIds[i] = -1;
				}
			} else {
				streamIds[i] = -1;
				break;
			}
		}

		first = false;
		int nextChannel = readOrder[processI];

		// Now start processing
		if (nextChannel == -1 || streamIds[nextChannel] == -1)
			break;

		//printf("process: order %d, ch %d, id %d\n", processI, nextChannel, streamIds[nextChannel]);

		// Try again on error — B139: four times, not forever. b137 parked main
		// here for good (watchdog: 'frozen 104s at [cd-sync]', worker and DVD
		// idle) on a read that errored instantly every time.
		int tries = 0;
		bool readOk = true;
		while (CdStreamSync(nextChannel) != STREAM_NONE) {
			if(++tries >= 4){
				printf("STREAM read error ch %d model %d off %u size %u: giving up on it\n", nextChannel, streamIds[nextChannel], (unsigned)streamPoses[nextChannel], (unsigned)streamSizes[nextChannel]);
				readOk = false;
				break;
			}
			CdStreamRead(nextChannel, ms_pStreamingBuffer[nextChannel], imgOffset+streamPoses[nextChannel], streamSizes[nextChannel]);
		}
		if(!readOk){
			gFailWhy = "read error";
			FailedLoad(streamIds[nextChannel]);
			streamIds[nextChannel] = -1;
			readOrder[processI] = -1;
			processI = (processI + 1) % ARRAY_SIZE(readOrder);
			continue;
		}
		ms_aInfoForModel[streamIds[nextChannel]].m_loadState = STREAMSTATE_READING;

		MakeSpaceFor(streamSizes[nextChannel] * CDSTREAM_SECTOR_SIZE);
		ConvertBufferToObject(ms_pStreamingBuffer[nextChannel], streamIds[nextChannel]);
		if(ms_aInfoForModel[streamIds[nextChannel]].m_loadState == STREAMSTATE_STARTED)
			FinishLoadingLargeFile(ms_pStreamingBuffer[nextChannel], streamIds[nextChannel]);

		if(streamIds[nextChannel] < STREAM_OFFSET_TXD){
			CSimpleModelInfo *mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(streamIds[nextChannel]);
			if(mi->IsSimple())
				mi->m_alpha = 255;
		}
		streamIds[nextChannel] = -1;
		readOrder[processI] = -1;
		processI = (processI + 1) % ARRAY_SIZE(readOrder);
	}

	ms_bLoadingBigModel = false;
	for(i = 0; i < 4; i++){
		ms_channel[1].streamIds[i] = -1;
		ms_channel[1].offsets[i] = -1;
	}
	ms_channel[1].state = CHANNELSTATE_IDLE;
	bInsideLoadAll = false;
}
#else
void
CStreaming::LoadAllRequestedModels(bool priority)
{
	static bool bInsideLoadAll = false;
	int imgOffset, streamId, status;
	int i;
	uint32 posn, size;

	int numRequests = 4*ms_numModelsRequested;

	if(bInsideLoadAll)
		return;
	bInsideLoadAll = true;
	gMainWhere = "load-all"; gLoadAllN++;

	if(priority)
		numRequests = ms_numPriorityRequests;

	FlushChannels();
	imgOffset = GetCdImageOffset(CdStreamGetLastPosn());

	while(ms_endRequestedList.m_prev != &ms_startRequestedList && numRequests > 0){
		numRequests--;
		streamId = GetNextFileOnCd(0, priority);
		if(streamId == -1)
			break;

		ms_aInfoForModel[streamId].RemoveFromList();
		ms_channel[0].streamIds[0] = streamId;
		DecrementRef(streamId);

		if(ms_aInfoForModel[streamId].GetCdPosnAndSize(posn, size)){
			int8 *dst = ms_pStreamingBuffer[0];
			uint32 readSize = size;
#ifdef GTA_OGC
			gPullCd = imgOffset+posn;
			if(readSize > 2*(uint32)ms_streamingBufferSize)
				readSize = 2*ms_streamingBufferSize;   // the rest streams through the pair, see CdPullStream
#endif
			do
				status = CdStreamRead(0, dst, imgOffset+posn, readSize);
			while(CdStreamSync(0) || status == STREAM_NONE);
			ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_READING;

			MakeSpaceFor(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL ? 4*1024 : size * CDSTREAM_SECTOR_SIZE);
			ConvertBufferToObject(dst, streamId);
			gcHeapCheck(0x434F4E41, streamId);
			if(ms_aInfoForModel[streamId].m_loadState == STREAMSTATE_STARTED)
				FinishLoadingLargeFile(dst, streamId);

			if(streamId < STREAM_OFFSET_TXD){
				CSimpleModelInfo *mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(streamId);
				if(mi->IsSimple())
					mi->m_alpha = 255;
			}
		}else{
			// empty
			ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_LOADED;
		}
	}

	ms_bLoadingBigModel = false;
	for(i = 0; i < 4; i++){
		ms_channel[1].streamIds[i] = -1;
		ms_channel[1].offsets[i] = -1;
	}
	ms_channel[1].state = CHANNELSTATE_IDLE;
	bInsideLoadAll = false;
}
#endif

void
CStreaming::FlushChannels(void)
{
	if(ms_channel[1].state == CHANNELSTATE_STARTED)
		ProcessLoadingChannel(1);

	if(ms_channel[0].state == CHANNELSTATE_READING){
		CdStreamSync(0);
		ProcessLoadingChannel(0);
	}
	if(ms_channel[0].state == CHANNELSTATE_STARTED)
		ProcessLoadingChannel(0);

	if(ms_channel[1].state == CHANNELSTATE_READING){
		CdStreamSync(1);
		ProcessLoadingChannel(1);
	}
	if(ms_channel[1].state == CHANNELSTATE_STARTED)
		ProcessLoadingChannel(1);
}

void
CStreaming::FlushRequestList(void)
{
	CStreamingInfo *si, *next;

	for(si = ms_startRequestedList.m_next; si != &ms_endRequestedList; si = next){
		next = si->m_next;
		RemoveModel(si - ms_aInfoForModel);
	}
#ifdef FLUSHABLE_STREAMING
	if(ms_channel[0].state == CHANNELSTATE_READING) {
		flushStream[0] = 1;
	}
	if(ms_channel[1].state == CHANNELSTATE_READING) {
		flushStream[1] = 1;
	}
#endif
	FlushChannels();
}


void
CStreaming::ImGonnaUseStreamingMemory(void)
{
	PUSH_MEMID(MEMID_STREAM);
}

void
CStreaming::IHaveUsedStreamingMemory(void)
{
	POP_MEMID();
	UpdateMemoryUsed();
}

void
CStreaming::UpdateMemoryUsed(void)
{
#ifdef USE_CUSTOM_ALLOCATOR
	ms_memoryUsed =
		gMainHeap.GetMemoryUsed(MEMID_STREAM) +
		gMainHeap.GetMemoryUsed(MEMID_STREAM_MODELS) +
		gMainHeap.GetMemoryUsed(MEMID_STREAM_TEXUTRES) +
		gMainHeap.GetMemoryUsed(MEMID_STREAM_COLLISION) +
		gMainHeap.GetMemoryUsed(MEMID_STREAM_ANIMATION);
#endif
}

#define STREAM_DIST 80.0f

void
CStreaming::AddModelsToRequestList(const CVector &pos, int32 flags)
{
	float xmin, xmax, ymin, ymax;
	int ixmin, ixmax, iymin, iymax;
	int ix, iy;
	int dx, dy, d;
	CSector *sect;

	xmin = pos.x - STREAM_DIST;
	ymin = pos.y - STREAM_DIST;
	xmax = pos.x + STREAM_DIST;
	ymax = pos.y + STREAM_DIST;

	ixmin = CWorld::GetSectorIndexX(xmin);
	if(ixmin < 0) ixmin = 0;
	ixmax = CWorld::GetSectorIndexX(xmax);
	if(ixmax >= NUMSECTORS_X) ixmax = NUMSECTORS_X-1;
	iymin = CWorld::GetSectorIndexY(ymin);
	if(iymin < 0) iymin = 0;
	iymax = CWorld::GetSectorIndexY(ymax);
	if(iymax >= NUMSECTORS_Y) iymax = NUMSECTORS_Y-1;

	CWorld::AdvanceCurrentScanCode();

	for(iy = iymin; iy <= iymax; iy++){
		dy = iy - CWorld::GetSectorIndexY(pos.y);
		for(ix = ixmin; ix <= ixmax; ix++){

			if(CRenderer::m_loadingPriority && ms_numModelsRequested > 5)
				return;

			dx = ix - CWorld::GetSectorIndexX(pos.x);
			d = dx*dx + dy*dy;
			sect = CWorld::GetSector(ix, iy);
			if(d <= 0){
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], flags);
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP], flags);
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], flags);
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], flags);
			}else if(d <= 3*3){
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], pos.x, pos.y, xmin, ymin, xmax, ymax, flags);
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP], pos.x, pos.y, xmin, ymin, xmax, ymax, flags);
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], pos.x, pos.y, xmin, ymin, xmax, ymax, flags);
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], pos.x, pos.y, xmin, ymin, xmax, ymax, flags);
			}
		}
	}
}

void
CStreaming::ProcessEntitiesInSectorList(CPtrList &list, float x, float y, float xmin, float ymin, float xmax, float ymax, int32 flags)
{
	CPtrNode *node;
	CEntity *e;
	float lodDistSq;
	CVector2D pos;

	for(node = list.first; node; node = node->next){
		e = (CEntity*)node->item;

		if(e->m_scanCode == CWorld::GetCurrentScanCode())
			continue;

		e->m_scanCode = CWorld::GetCurrentScanCode();
		if(!e->bStreamingDontDelete && IsAreaVisible(e->m_area) && !e->bDontStream && e->bIsVisible){
			CTimeModelInfo *mi = (CTimeModelInfo*)CModelInfo::GetModelInfo(e->GetModelIndex());
			if (mi->GetModelType() != MITYPE_TIME || CClock::GetIsTimeInRange(mi->GetTimeOn(), mi->GetTimeOff())) {
				lodDistSq = sq(mi->GetLargestLodDistance());
				lodDistSq = Min(lodDistSq, sq(STREAM_DIST));
				pos = CVector2D(e->GetPosition());
				if(xmin < pos.x && pos.x < xmax &&
				   ymin < pos.y && pos.y < ymax &&
				   (CVector2D(x, y) - pos).MagnitudeSqr() < lodDistSq)
					RequestModel(e->GetModelIndex(), flags);
			}
		}
	}
}

void
CStreaming::ProcessEntitiesInSectorList(CPtrList &list, int32 flags)
{
	CPtrNode *node;
	CEntity *e;

	for(node = list.first; node; node = node->next){
		e = (CEntity*)node->item;

		if(e->m_scanCode == CWorld::GetCurrentScanCode())
			continue;

		e->m_scanCode = CWorld::GetCurrentScanCode();
		if(!e->bStreamingDontDelete && IsAreaVisible(e->m_area) && !e->bDontStream && e->bIsVisible){
			CTimeModelInfo *mi = (CTimeModelInfo*)CModelInfo::GetModelInfo(e->GetModelIndex());
			if (mi->GetModelType() != MITYPE_TIME || CClock::GetIsTimeInRange(mi->GetTimeOn(), mi->GetTimeOff()))
				RequestModel(e->GetModelIndex(), flags);
		}
	}
}

void
CStreaming::DeleteFarAwayRwObjects(const CVector &pos)
{
	int posx, posy;
	int x, y;
	int r, i;
	CSector *sect;

	posx = CWorld::GetSectorIndexX(pos.x);
	posy = CWorld::GetSectorIndexY(pos.y);

	// Move oldSectorX/Y to new sector and delete RW objects in its "wake" for every step:
	// O is the old sector, <- is the direction in which we move it,
	// X are the sectors we delete RW objects from (except we go up to 10)
	//            X
	//          X X
	//        X X X
	//        X X X
	// <- O   X X X
	//        X X X
	//        X X X
	//          X X
	//            X

	while(posx != ms_oldSectorX){
		if(posx < ms_oldSectorX){
			for(r = 2; r <= 10; r++){
				x = ms_oldSectorX + r;
				if(x < 0)
					continue;
				if(x >= NUMSECTORS_X)
					break;

				for(i = -r; i <= r; i++){
					y = ms_oldSectorY + i;
					if(y < 0)
						continue;
					if(y >= NUMSECTORS_Y)
						break;

					sect = CWorld::GetSector(x, y);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS]);
					DeleteRwObjectsInOverlapSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP], ms_oldSectorX, ms_oldSectorY);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS]);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES]);
				}
			}
			ms_oldSectorX--;
		}else{
			for(r = 2; r <= 10; r++){
				x = ms_oldSectorX - r;
				if(x < 0)
					break;
				if(x >= NUMSECTORS_X)
					continue;

				for(i = -r; i <= r; i++){
					y = ms_oldSectorY + i;
					if(y < 0)
						continue;
					if(y >= NUMSECTORS_Y)
						break;

					sect = CWorld::GetSector(x, y);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS]);
					DeleteRwObjectsInOverlapSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP], ms_oldSectorX, ms_oldSectorY);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS]);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES]);
				}
			}
			ms_oldSectorX++;
		}
	}

	while(posy != ms_oldSectorY){
		if(posy < ms_oldSectorY){
			for(r = 2; r <= 10; r++){
				y = ms_oldSectorY + r;
				if(y < 0)
					continue;
				if(y >= NUMSECTORS_Y)
					break;

				for(i = -r; i <= r; i++){
					x = ms_oldSectorX + i;
					if(x < 0)
						continue;
					if(x >= NUMSECTORS_X)
						break;

					sect = CWorld::GetSector(x, y);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS]);
					DeleteRwObjectsInOverlapSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP], ms_oldSectorX, ms_oldSectorY);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS]);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES]);
				}
			}
			ms_oldSectorY--;
		}else{
			for(r = 2; r <= 10; r++){
				y = ms_oldSectorY - r;
				if(y < 0)
					break;
				if(y >= NUMSECTORS_Y)
					continue;

				for(i = -r; i <= r; i++){
					x = ms_oldSectorX + i;
					if(x < 0)
						continue;
					if(x >= NUMSECTORS_X)
						break;

					sect = CWorld::GetSector(x, y);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS]);
					DeleteRwObjectsInOverlapSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP], ms_oldSectorX, ms_oldSectorY);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS]);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES]);
				}
			}
			ms_oldSectorY++;
		}
	}
}

void
CStreaming::DeleteAllRwObjects(void)
{
	int x, y;
	CSector *sect;

	for(x = 0; x < NUMSECTORS_X; x++)
		for(y = 0; y < NUMSECTORS_Y; y++){
			sect = CWorld::GetSector(x, y);
			DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS]);
			DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP]);
			DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS]);
			DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS_OVERLAP]);
			DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES]);
			DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES_OVERLAP]);
		}
}

void
CStreaming::DeleteRwObjectsAfterDeath(const CVector &pos)
{
	int ix, iy;
	int x, y;
	CSector *sect;

	ix = CWorld::GetSectorIndexX(pos.x);
	iy = CWorld::GetSectorIndexY(pos.y);

	for(x = 0; x < NUMSECTORS_X; x++)
		for(y = 0; y < NUMSECTORS_Y; y++)
			if(Abs(ix - x) > 3.0f &&
			   Abs(iy - y) > 3.0f){
				sect = CWorld::GetSector(x, y);
				DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS]);
				DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP]);
				DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS]);
				DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS_OVERLAP]);
				DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES]);
				DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES_OVERLAP]);
			}
}

void
CStreaming::DeleteRwObjectsBehindCamera(size_t mem)
{
	int ix, iy;
	int x, y;
	int xmin, xmax, ymin, ymax;
	int inc;
	CSector *sect;

	if(ms_memoryUsed < mem)
		return;

	ix = CWorld::GetSectorIndexX(TheCamera.GetPosition().x);
	iy = CWorld::GetSectorIndexY(TheCamera.GetPosition().y);

	if(Abs(TheCamera.GetForward().x) > Abs(TheCamera.GetForward().y)){
		// looking west/east

		ymin = Max(iy - 10, 0);
		ymax = Min(iy + 10, NUMSECTORS_Y - 1);
		assert(ymin <= ymax);

		// Delete a block of sectors that we know is behind the camera
		if(TheCamera.GetForward().x > 0.0f){
			// looking east
			xmax = Max(ix - 2, 0);
			xmin = Max(ix - 10, 0);
			inc = 1;
		}else{
			// looking west
			xmax = Min(ix + 2, NUMSECTORS_X - 1);
			xmin = Min(ix + 10, NUMSECTORS_X - 1);
			inc = -1;
		}
		for(y = ymin; y <= ymax; y++){
			for(x = xmin; x != xmax; x += inc){
				sect = CWorld::GetSector(x, y);
				if(DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], mem))
					return;
			}
		}

		
		while(RemoveLoadedZoneModel())
			if(ms_memoryUsed < mem)
				return;

		// Now a block that intersects with the camera's frustum
		if(TheCamera.GetForward().x > 0.0f){
			// looking east
			xmax = Max(ix + 10, 0);
			xmin = Max(ix - 2, 0);
			inc = 1;
		}else{
			// looking west
			xmax = Min(ix - 10, NUMSECTORS_X - 1);
			xmin = Min(ix + 2, NUMSECTORS_X - 1);
			inc = -1;
		}
		for(y = ymin; y <= ymax; y++){
			for(x = xmin; x != xmax; x += inc){
				sect = CWorld::GetSector(x, y);
				if(DeleteRwObjectsNotInFrustumInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], mem) ||
				   DeleteRwObjectsNotInFrustumInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], mem) ||
				   DeleteRwObjectsNotInFrustumInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], mem))
					return;
			}
		}

		// As last resort, delete objects from the last step more aggressively
		for(y = ymin; y <= ymax; y++){
			for(x = xmax; x != xmin; x -= inc){
				sect = CWorld::GetSector(x, y);
				if(DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], mem))
					return;
			}
		}
	}else{
		// looking north/south

		xmin = Max(ix - 10, 0);
		xmax = Min(ix + 10, NUMSECTORS_X - 1);
		assert(xmin <= xmax);

		// Delete a block of sectors that we know is behind the camera
		if(TheCamera.GetForward().y > 0.0f){
			// looking north
			ymax = Max(iy - 2, 0);
			ymin = Max(iy - 10, 0);
			inc = 1;
		}else{
			// looking south
			ymax = Min(iy + 2, NUMSECTORS_Y - 1);
			ymin = Min(iy + 10, NUMSECTORS_Y - 1);
			inc = -1;
		}
		for(x = xmin; x <= xmax; x++){
			for(y = ymin; y != ymax; y += inc){
				sect = CWorld::GetSector(x, y);
				if(DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], mem))
					return;
			}
		}

		while(RemoveLoadedZoneModel())
			if(ms_memoryUsed < mem)
				return;

		// Now a block that intersects with the camera's frustum
		if(TheCamera.GetForward().y > 0.0f){
			// looking north
			ymax = Max(iy + 10, 0);
			ymin = Max(iy - 2, 0);
			inc = 1;
		}else{
			// looking south
			ymax = Min(iy - 10, NUMSECTORS_Y - 1);
			ymin = Min(iy + 2, NUMSECTORS_Y - 1);
			inc = -1;
		}
		for(x = xmin; x <= xmax; x++){
			for(y = ymin; y != ymax; y += inc){
				sect = CWorld::GetSector(x, y);
				if(DeleteRwObjectsNotInFrustumInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], mem) ||
				   DeleteRwObjectsNotInFrustumInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], mem) ||
				   DeleteRwObjectsNotInFrustumInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], mem))
					return;
			}
		}

// this is gone in mobile together with RemoveReferencedTxds
//		if(RemoveReferencedTxds(mem))
//			return;

		// As last resort, delete objects from the last step more aggressively
		for(x = xmin; x <= xmax; x++){
			for(y = ymax; y != ymin; y -= inc){
				sect = CWorld::GetSector(x, y);
				if(DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], mem))
					return;
			}
		}
	}

	while(ms_memoryUsed >= mem && RemoveLeastUsedModel(0));
}

void
CStreaming::DeleteRwObjectsInSectorList(CPtrList &list)
{
	CPtrNode *node;
	CEntity *e;

	for(node = list.first; node; node = node->next){
		e = (CEntity*)node->item;
		if(!e->bStreamingDontDelete && !e->bImBeingRendered)
			e->DeleteRwObject();
	}
}

void
CStreaming::DeleteRwObjectsInOverlapSectorList(CPtrList &list, int32 x, int32 y)
{
	CPtrNode *node;
	CEntity *e;

	for(node = list.first; node; node = node->next){
		e = (CEntity*)node->item;
		if(e->m_rwObject && !e->bStreamingDontDelete && !e->bImBeingRendered){
			// Now this is pretty weird...
			if(Abs(CWorld::GetSectorIndexX(e->GetPosition().x) - x) >= 1.6f)
//			{
				e->DeleteRwObject();
//				return;		// BUG?
//			}
			else	// FIX?
			if(Abs(CWorld::GetSectorIndexY(e->GetPosition().y) - y) >= 1.6f)
				e->DeleteRwObject();
		}
	}
}

bool
CStreaming::DeleteRwObjectsBehindCameraInSectorList(CPtrList &list, size_t mem)
{
	CPtrNode *node;
	CEntity *e;

	for(node = list.first; node; node = node->next){
		e = (CEntity*)node->item;
		if(!e->bStreamingDontDelete && !e->bImBeingRendered &&
#ifdef GTA_OGC
		   !(ms_aInfoForModel[e->GetModelIndex()].m_flags & STREAMFLAGS_20) &&
#endif
		   e->m_rwObject && ms_aInfoForModel[e->GetModelIndex()].m_next &&
		   FindPlayerPed()->m_pCurSurface != e){
			e->DeleteRwObject();
			if (CModelInfo::GetModelInfo(e->GetModelIndex())->GetNumRefs() == 0
#ifdef GTA_OGC
			    && CanRemoveModel(e->GetModelIndex())   // B137: RemoveModel on a QUEUED script-owned model cancels its request (b136: specialchar 110 'state 0 req 0' forever)
#endif
			    ) {
#ifdef GTA_OGC
				TraceEvict(e->GetModelIndex());
#endif
				RemoveModel(e->GetModelIndex());
				if(ms_memoryUsed < mem)
					return true;
			}
		}
	}
	return false;
}

bool
CStreaming::DeleteRwObjectsNotInFrustumInSectorList(CPtrList &list, size_t mem)
{
	CPtrNode *node;
	CEntity *e;

	for(node = list.first; node; node = node->next){
		e = (CEntity*)node->item;
		if(!e->bStreamingDontDelete && !e->bImBeingRendered &&
#ifdef GTA_OGC
		   !(ms_aInfoForModel[e->GetModelIndex()].m_flags & STREAMFLAGS_20) &&
#endif
		   e->m_rwObject && (!e->IsVisible() || e->bOffscreen) && ms_aInfoForModel[e->GetModelIndex()].m_next){
			e->DeleteRwObject();
			if (CModelInfo::GetModelInfo(e->GetModelIndex())->GetNumRefs() == 0
#ifdef GTA_OGC
			    && CanRemoveModel(e->GetModelIndex())   // B137: RemoveModel on a QUEUED script-owned model cancels its request (b136: specialchar 110 'state 0 req 0' forever)
#endif
			    ) {
#ifdef GTA_OGC
				TraceEvict(e->GetModelIndex());
#endif
				RemoveModel(e->GetModelIndex());
				if(ms_memoryUsed < mem)
					return true;
			}
		}
	}
	return false;
}

void
CStreaming::MakeSpaceFor(int32 size)
{
#ifdef FIX_BUGS
#define MB (1024 * 1024)
	if(ms_memoryAvailable == 0) {
		extern size_t _dwMemAvailPhys;
		ms_memoryAvailable = (_dwMemAvailPhys - 10 * MB) / 2;
		if(ms_memoryAvailable < 65 * MB) ms_memoryAvailable = 65 * MB;
	}
#undef MB
#endif
	int32 want = size;
#ifdef GTA_OGC
	// B107: blocking loads (LoadScene, LoadAllRequestedModels at New Game)
	// used to run 12 s against Init2's boot value; track the heap they fill.
	// Every 4th call: mallinfo walks the heap.
	{ static uint32 n; if((n++ & 3) == 0) HonestBudget(); }
	// Free a slice of headroom beyond what this load needs, so the next few
	// loads fit without evicting anything.
	//
	// Stock behaviour leaves exactly `size` free, which is fine on PC where
	// the budget is 65MB and never binds. Here the working set sits right at
	// the budget (measured 11470K resident against 11524K available), so
	// every single request evicted a model, and the model evicted was
	// promptly requested again. Standing perfectly still was enough to see
	// it: the detailed building drops out, SetupBigBuildingVisibility falls
	// back to the far LOD because the detail is no longer resident, the
	// detail streams back, and it flips again. That is the "flickering
	// between near and far LOD while stationary".
	//
	// ponytail: a fixed slice, not a ratio — the upgrade is real resident-byte
	// accounting, at which point the budget itself becomes trustworthy.
	// 1.5MB, not 512K (B37): at 512K the streamer sat one model above the
	// line and evicted ~55 times a second — every load cost a reload, and the
	// disc traffic starved the audio stream thread (crackle). A wider slice
	// evicts in batches and leaves the disc to the streams between them.
	// B53: no slice. Under the honest budget it demanded 512K the heap never had,
	// so every conversion evicted; the fresh TXDs went first and reloaded at 2/s.
#endif
	while(ms_memoryUsed >= ms_memoryAvailable - want){
		size_t before = ms_memoryUsed;
#ifdef GTA_OGC
		// Counted so "the streamer is thrashing" stops being a guess. Against
		// the loads completed over the same interval this separates the two
		// failure modes that look identical on screen: evictions ~= loads is
		// churn (the budget is too small and every load costs a model), while
		// evictions ~= 0 with loads still trickling means capacity is fine and
		// the backlog is a rate limit somewhere else.
		gStrEvict++;
#endif
		if(!RemoveLeastUsedModel(STREAMFLAGS_20)){
			DeleteRwObjectsBehindCamera(ms_memoryAvailable - size);
			return;
		}
		// Stop if an eviction freed nothing measurable. Without this the loop
		// keeps evicting while the accounting stands still and strips the
		// world bare instead of freeing what the load asked for.
		if(ms_memoryUsed >= before)
			return;
	}
}

void
CStreaming::LoadScene(const CVector &pos)
{
#ifdef GTA_OGC
	HonestBudget();   // B107: not Init2's boot number
	printf("STREAM loadscene budget %uK used %uK\n", (unsigned)(ms_memoryAvailable/1024), (unsigned)(ms_memoryUsed/1024));
#endif
	CStreamingInfo *si, *prev;
	eLevelName level;

	level = CTheZones::GetLevelFromPosition(&pos);
	debug("Start load scene\n");
	for(si = ms_endRequestedList.m_prev; si != &ms_startRequestedList; si = prev){
		prev = si->m_prev;
		if((si->m_flags & (STREAMFLAGS_KEEP_IN_MEMORY|STREAMFLAGS_PRIORITY)) == 0)
			RemoveModel(si - ms_aInfoForModel);
	}
	CRenderer::m_loadingPriority = false;
	DeleteAllRwObjects();
	if(level == LEVEL_GENERIC)
		level = CGame::currLevel;
	CGame::currLevel = level;
	RemoveUnusedBigBuildings(level);
	RequestBigBuildings(level, pos);
	RequestBigBuildings(LEVEL_GENERIC, pos);
	RemoveIslandsNotUsed(level);
	LoadAllRequestedModels(false);
	InstanceBigBuildings(level, pos);
	InstanceBigBuildings(LEVEL_GENERIC, pos);
	AddModelsToRequestList(pos, STREAMFLAGS_20);
	CRadar::StreamRadarSections(pos);

	if (!CGame::IsInInterior()) {
		for (int i = 0; i < 5; i++) {
			CZoneInfo zone;
			CTheZones::GetZoneInfoForTimeOfDay(&pos, &zone);
			int32 model = CCarCtrl::ChooseCarModelToLoad(CCarCtrl::ChooseCarRating(&zone));
			CStreaming::RequestModel(model, STREAMFLAGS_DEPENDENCY);
		}
	}
	LoadAllRequestedModels(false);
	InstanceLoadedModels(pos);

	for(int i = 0; i < NUMSTREAMINFO; i++)
		ms_aInfoForModel[i].m_flags &= ~STREAMFLAGS_20;
	debug("End load scene\n");
}

void
CStreaming::LoadSceneCollision(const CVector &pos)
{
	CColStore::LoadCollision(pos);
	CStreaming::LoadAllRequestedModels(false);
}

void
CStreaming::MemoryCardSave(uint8 *buf, uint32 *size)
{
	int i;

	*size = NUM_DEFAULT_MODELS;
	for(i = 0; i < NUM_DEFAULT_MODELS; i++)
		if(ms_aInfoForModel[i].m_loadState == STREAMSTATE_LOADED)
			buf[i] = ms_aInfoForModel[i].m_flags;
		else
			buf[i] = 0xFF;
}

void 
CStreaming::MemoryCardLoad(uint8 *buf, uint32 size)
{
	uint32 i;

	assert(size == NUM_DEFAULT_MODELS);
	for(i = 0; i < size; i++)
		if(ms_aInfoForModel[i].m_loadState == STREAMSTATE_LOADED)
			if(buf[i] != 0xFF)
				ms_aInfoForModel[i].m_flags = buf[i];
}

void
CStreaming::UpdateForAnimViewer(void)
{
	if (CStreaming::ms_channelError == -1) {
		CStreaming::AddModelsToRequestList(CVector(0.0f, 0.0f, 0.0f), 0);
		CStreaming::LoadRequestedModels();
		// original modifier was %d
		sprintf(gString, "Requested %d, memory size %zuK\n", CStreaming::ms_numModelsRequested, 2 * CStreaming::ms_memoryUsed);
	}
	else {
		CStreaming::RetryLoadFile(CStreaming::ms_channelError);
	}
}


void
CStreaming::PrintStreamingBufferState()
{
	char str[128];
	wchar wstr[128];
	uint32 offset, size;

	CTimer::Stop();
	int i = 0;
	while (i < NUMSTREAMINFO) {
		while (true) {
			int j = 0;
			DoRWStuffStartOfFrame(50, 50, 50, 0, 0, 0, 255);
			CPad::UpdatePads();
			CSprite2d::InitPerFrame();
			CFont::InitPerFrame();
			DefinedState();

			CRect unusedRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
			CRGBA unusedColor(255, 255, 255, 255);
			CFont::SetFontStyle(FONT_BANK);
			CFont::SetBackgroundOff();
			CFont::SetWrapx(DEFAULT_SCREEN_WIDTH);
			CFont::SetScale(0.5f, 0.75f);
			CFont::SetCentreOff();
			CFont::SetCentreSize(DEFAULT_SCREEN_WIDTH);
			CFont::SetJustifyOff();
			CFont::SetColor(CRGBA(200, 200, 200, 200));
			CFont::SetBackGroundOnlyTextOff();
			int modelIndex = i;
			if (modelIndex < NUMSTREAMINFO) {
				int y = 24;
				for ( ; j < 34 && modelIndex < NUMSTREAMINFO; modelIndex++) {
					CStreamingInfo *streamingInfo = &ms_aInfoForModel[modelIndex];
					CBaseModelInfo *modelInfo = CModelInfo::GetModelInfo(modelIndex);
					if (streamingInfo->m_loadState != STREAMSTATE_LOADED || !streamingInfo->GetCdPosnAndSize(offset, size))
						continue;

					if (modelIndex >= STREAM_OFFSET_TXD)
						sprintf(str, "txd %s, refs %d, size %dK, flags 0x%x", CTxdStore::GetTxdName(modelIndex - STREAM_OFFSET_TXD),
						        CTxdStore::GetNumRefs(modelIndex - STREAM_OFFSET_TXD), 2 * size, streamingInfo->m_flags);
					else
						sprintf(str, "model %d,%s, refs%d, size%dK, flags%x", modelIndex, modelInfo->GetModelName(), modelInfo->GetNumRefs(), 2 * size,
						        streamingInfo->m_flags);
					AsciiToUnicode(str, wstr);
					CFont::PrintString(24.0f, y, wstr);
					y += 12;
					j++;
				}
			}

			if (CPad::GetPad(1)->GetCrossJustDown())
				i = modelIndex;

			if (!CPad::GetPad(1)->GetTriangleJustDown())
				break;

			i = 0;
			CFont::DrawFonts();
			DoRWStuffEndOfFrame();
		}
		CFont::DrawFonts();
		DoRWStuffEndOfFrame();
	}
	CTimer::Update();
}
