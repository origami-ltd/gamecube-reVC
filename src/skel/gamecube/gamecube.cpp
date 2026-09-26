#include "common.h"
#include "crossplatform.h"
#include "platform.h"
#include "Pad.h"
#include "skeleton.h"
#include "main.h"
#include "Game.h"
#include "Timer.h"
#include "Frontend.h"
#include "Camera.h"
#include "DMAudio.h"
#include "GenericGameStorage.h"
#include "ControllerConfig.h"
#include "FileMgr.h"
#include "CdStream.h"
#include "Streaming.h"
#include "TxdStore.h"
#include "PlayerPed.h"
#include "PlayerInfo.h"
#include "Pools.h"
#include "CarCtrl.h"
#include "CutsceneMgr.h"
#include "Wanted.h"
#include "Font.h"
#include "Sprite2d.h"
#include "gcmovie.h"
#ifdef EXTENDED_PIPELINES
#include "custompipes.h"
#endif

#include <gccore.h>
#include <aesndlib.h>
#include <ogc/machine/processor.h>
#include <ogc/usbgecko.h>
#include <ogc/dvd.h>
extern "C" int GcCardMountDevice(void);
namespace rw { namespace gx { int8_t gxReadEfbPref(void); } }
#include <iso9660.h>
extern "C" bool ISO9660_MountDbg(const char *name, const DISC_INTERFACE *disc_interface);
extern "C" void ISO9660_UnmountDbg(const char *name);
#include <fat.h>
#include <sdcard/gcsd.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/color.h>
#include <tuxedo/ppc/exception.h>
#include <unistd.h>
#include <dirent.h>
#include <stdarg.h>
#include <malloc.h>

namespace rw { namespace gx {
extern bool32 gxRimEnable;
extern uint32 gxCopyFilterLevel;
extern bool32 gxGlossEnable;
extern float gxGlossMult;
extern bool32 gxLightmapEnable;
extern float gxLightmapBlend;
extern uint32 gxDlBytes;
} }

// libogc enables external interrupts before the compiler-generated __eabi
// call runs the C++ global constructors. A normal homebrew loader leaves the
// hardware quiescent, but a disc apploader can hand us a pending interrupt;
// servicing it halfway through the constructor table produced a nested ISI
// before main() could install diagnostics. The linker wraps only __eabi, so
// normal interrupt delivery resumes before the first user statement.
extern "C" void __real___eabi(void);
// Heap emergency reserve (B73). The heap dies of fragmentation, not size: a 17K
// must-allocate failed with 1MB free in holes. Keep a 512K block aside; when an
// allocation fails, hand it back, retry, and tell the streamer to shed hard
// until the heap has real room again (Streaming.cpp re-arms it).
extern "C" void *__real_malloc(size_t);
extern "C" void *__real_memalign(size_t, size_t);
extern "C" void *__real_calloc(size_t, size_t);
extern "C" void *__real_realloc(void *, size_t);
extern "C" void __real_free(void *);
extern "C" { void *gHeapReserve; volatile unsigned gHeapEmergency; lwp_t gMainLwp; }
extern "C" int gcStreamEmergencyShed(unsigned need);   // Streaming.cpp (B77)
extern "C" void *gcBigAlloc(size_t sz);   // below (B79)
// B78: the emergency store is a private BSS arena, never handed back to the
// heap. B77's malloc'd reserve was released on the first failure and could not
// be re-armed: a fragmented heap has no 256K hole even with 1.8MB free, so the
// second 37K skin failed and mustmalloc exited. Blocks carved here carry their
// size; the arena resets when everything carved from it has been freed.
enum { HEAP_ARENA_BYTES = 256*1024 };
static uint8 gHeapArena[HEAP_ARENA_BYTES] __attribute__((aligned(32)));
static uint32 gArenaUsed, gArenaLive, gArenaCarves;
static inline bool gcInArena(const void *p){ return (const uint8*)p >= gHeapArena && (const uint8*)p < gHeapArena + HEAP_ARENA_BYTES; }

// DIAG B179: b178/b179 corrupted newlib's heap silently (malloc returned
// 0x83f637a0 with '1115581K free', then an ISI through a garbage vtable) — a
// failed native geometry load freeing an uninitialised attribBase (B180). With
// dvd:/heapcheck.txt the chunk chain is walked every frame and after every
// streamed model; the first bad header is printed once.
extern "C" { extern char *__malloc_sbrk_base; extern void *__malloc_av_[]; }
extern "C" void __malloc_lock(struct _reent *); extern "C" void __malloc_unlock(struct _reent *);
extern "C" { int gHeapCheckOn; }
static void gcStackLine(char *stack, int cap);
extern "C" void gcHeapCheck(u32 tag, u32 arg)
{
	static int bad;
	if(!gHeapCheckOn || bad) return;
	__malloc_lock(_REENT);
	u8 *top = (u8*)__malloc_av_[2], *p = nil;
	u32 size = 0, chunks = 0;
	bool ok = true;
	if(__malloc_sbrk_base != nil && __malloc_sbrk_base != (char*)-1 && top != nil){
		p = (u8*)(((u32)__malloc_sbrk_base + 7) & ~7u);
		while(p < top){
			size = *(u32*)(p + 4) & ~3u;
			if(size < 16 || (size & 7) || size > (u32)(top - p) || ++chunks > 400000){ ok = false; break; }
			p += size;
		}
		if(ok && p != top) ok = false;
	}
	__malloc_unlock(_REENT);
	if(ok) return;
	bad = 1;
	char stack[100]; gcStackLine(stack, sizeof(stack));
	printf("HEAPCHECK BAD after %c%c%c%c %u: chunk %p size %08x (#%u) top %p base %p at%s\n",
	    tag>>24, tag>>16, tag>>8, tag, (unsigned)arg, p, (unsigned)size, (unsigned)chunks, top, __malloc_sbrk_base, stack);
}
// B120: who asks — the b118 tour failed 2587 mallocs of 37508/61044 bytes on
// the main thread at ~1400/s with nothing in the log naming the caller.
static void gcStackLine(char *stack, int cap)
{
	int n = 0;
	u32 sp = (u32)(uintptr_t)__builtin_frame_address(0);
	for(int i = 0; i < 8 && sp && (sp & 3) == 0 && sp >= 0x80000000u && sp < 0x81800000u; i++){
		u32 *frame = (u32*)sp;
		n += snprintf(stack + n, cap - n, " %08X", frame[1]);
		if(n >= cap - 10) break;
		sp = frame[0];
	}
}
static void *gcArenaCarve(size_t sz, size_t al, const char *who)
{
	// B129: small blocks only. b118-b128 carved 53-101K model blocks here
	// ('used 238K/256K live 2' for the rest of the run) and the sub-1K RW
	// allocation that ended b128 ('OOM need 0K') found the arena full.
	extern volatile int gcEssentialLoad;   // Streaming.cpp: set while converting a SCRIPTOWNED model (B150)
	if(sz > 4096 && !gcEssentialLoad) return NULL;   // B150: the script's own props/specials may take the whole arena (b149: prop 295, 81K, 24 misses with 1.4MB of crumbs)
	if(al < 32) al = 32;
	u32 level; _CPU_ISR_Disable(level);
	uint32 start = (gArenaUsed + 32 + al - 1) & ~(al - 1);   // 32 bytes of header room before the block
	void *p = NULL;
	if(start + sz <= HEAP_ARENA_BYTES){
		*(uint32*)(gHeapArena + start - 4) = (uint32)sz;
		p = gHeapArena + start; gArenaUsed = start + sz; gArenaLive++; gArenaCarves++;
	}
	_CPU_ISR_Restore(level);
	gHeapEmergency++;
	static u64 lastSaid; static unsigned muted;
	if(ticks_to_millisecs(gettime() - lastSaid) > 1000){
		char stack[100]; gcStackLine(stack, sizeof(stack));
		printf("HEAP: %s(%u) failed, arena %s (used %uK/%uK live %u, %u muted) at%s\n", who, (unsigned)sz, p ? "carved" : "FULL",
		    gArenaUsed/1024, HEAP_ARENA_BYTES/1024, gArenaLive, muted, stack); lastSaid = gettime(); muted = 0;
	}else muted++;
	return p;
}
static int gcBigReleaseEmpty(void);   // below, with the chunk allocator
extern "C" int gcBootDone(void);   // below (B152)
// B177: set around allocations whose owner can do without (the post-effect
// frame grab): a failure returns NULL instead of shedding the world for them.
// b177e shed 18 models, LODs among them, for one 600K grab during the intro.
extern "C" { volatile int gcOptionalAlloc; }
static void *gcHeapFail(size_t sz, size_t al, const char *who)
{
	if(sz == 0 || gcOptionalAlloc) return NULL;
	// B133: the chunks first, any size. b132 booted in 6.7 minutes: 369
	// 1.6-3.6K StageCollisionRecord mallocs failed in LoadLevel while
	// chunk 1 (2MB, arena) sat empty — B110 keeps <4K blocks out of the
	// chunks while booting, which is right as a preference, not as a rule
	// when the general heap is already gone.
	if(al <= 32 && gcBootDone()){ extern void *gcBigAllocAny(size_t); void *p = gcBigAllocAny(sz); if(p) return p; }   // B134 after boot only — B152: during LoadLevel this filled the chunks with small blocks and starved the general heap (b151: 3 collision zones lost at boot; b130 without it booted clean)
	// B109: the general heap starved at boot with 5.3MB free INSIDE the
	// permanent chunks (B106): 64-byte mallocs failed through the whole of
	// CGame::Initialise. Before B106 the empty chunks went back to the heap
	// on their own; now they go back exactly when the heap needs them.
	if(gcBigReleaseEmpty()){
		void *p = al > 8 ? __real_memalign(al, sz) : __real_malloc(sz);
		if(p) return p;
	}
	if(gcStreamEmergencyShed(sz)){   // main thread: drop models, then one more try in the real heap
		void *p = al > 8 ? __real_memalign(al, sz) : __real_malloc(sz);
		if(p) return p;
		p = gcBigAlloc(sz); if(p) return p;   // B114/B115: any size — the shed freed chunk room as well, and a chunk beats the arena or death

	}else if(LWP_GetSelf() != gMainLwp){
		// B94: another thread (audio decode) cannot shed; flag the emergency and wait
		// for the main loop's cadence to free room rather than crash on a NULL
		// (B93: Tremor's failed 8K/16K buffers ended in wild MMIO writes).
		gHeapEmergency++;
		for(int k = 0; k < 40; k++){
			usleep(5000);
			void *p = al > 8 ? __real_memalign(al, sz) : __real_malloc(sz);
			if(p) return p;
			if(sz >= 4096){ p = gcBigAlloc(sz); if(p) return p; }
		}
	}
	return gcArenaCarve(sz, al, who);
}
extern "C" int gcBigContains(const void *p);
extern "C" void gcBigFree(void *p);
extern "C" size_t gcBigSizeOf(const void *p);

// B155: heap census by call site. Every live block goes into a side hash
// (pointer -> size, site); a site is the four return addresses above the
// allocator's entry point and keeps its live bytes. gcHeapCensusDump prints
// the top sites for addr2line. The 9-10MB of MEM1 that are neither streaming,
// pools nor textures had no instrument at all until now. DIAGNOSTIC: the
// tables cost 526K of MEM1 — set GC_HEAP_CENSUS to 0 for a release DOL.
// B177: off again, as in b172. b176's 16384 entries cannot hold the 17-20K live
// blocks b155 measured: a full table makes hcDel's backward shift spin forever
// with interrupts off (b177c froze on the loading screen). Needs >= 32768.
#define GC_HEAP_CENSUS 0
#if GC_HEAP_CENSUS
enum { HC_ENTRIES = 16384, HC_SITES = 512, HC_TOP = 48 };
struct HcEnt { u32 p, szsite; };                     // szsite = size<<9 | site
struct HcSite { u32 pc[4]; u32 live, count, used; };
static HcEnt *gHc; static HcSite *gHcSite; static u32 gHcN, gHcLost, gHcLive, gHcInit;
static void hcInit(void)
{
	gHcInit = 1;
	gHc = (HcEnt*)__real_malloc(sizeof(HcEnt)*HC_ENTRIES);
	gHcSite = (HcSite*)__real_malloc(sizeof(HcSite)*HC_SITES);
	if(gHc == NULL || gHcSite == NULL){ gHc = NULL; return; }
	memset(gHc, 0, sizeof(HcEnt)*HC_ENTRIES);
	memset(gHcSite, 0, sizeof(HcSite)*HC_SITES);
}
// B177: b176 cut the table from 65536 to 16384 entries and kept '>> 16', so the
// first probe landed up to 384K past the table: the dvdfs 'INDEX CORRUPTED'
// scribbler of the first b177 run. Mask to the table.
static inline u32 hcHash(u32 p){ return (((p >> 3) * 2654435761u) >> 16) & (HC_ENTRIES-1); }
// Frame 0 is this function, 1 is hcAdd, 2 is the allocator entry
// (__wrap_malloc, gcBigAlloc); its saved LR is the first address that matters.
static __attribute__((noinline)) u32 hcSiteOf(void)
{
	u32 pcs[4] = {0, 0, 0, 0};
	u32 sp = (u32)(uintptr_t)__builtin_frame_address(0);
	for(int i = 0, n = 0; i < 12 && n < 4 && sp && (sp & 3) == 0 && sp >= 0x80000000u && sp < 0x81800000u; i++){
		u32 *frame = (u32*)sp;
		if(i >= 2) pcs[n++] = frame[1];
		sp = frame[0];
	}
	u32 h = (pcs[0]*31u + pcs[1]*17u + pcs[2]*7u + pcs[3]) >> 2;
	for(u32 k = 0; k < HC_SITES; k++){
		u32 i = (h + k) & (HC_SITES-1);
		HcSite *s = &gHcSite[i];
		if(!s->used){ s->used = 1; memcpy(s->pc, pcs, sizeof(pcs)); return i; }
		if(memcmp(s->pc, pcs, sizeof(pcs)) == 0) return i;
	}
	return HC_SITES-1;   // table full: everything else lands in the last slot
}
static __attribute__((noinline)) void hcAdd(void *p, size_t sz)
{
	if(!gHcInit) hcInit();
	if(gHc == NULL || p == NULL) return;
	if(sz > 0x7FFFFF) sz = 0x7FFFFF;
	u32 level; _CPU_ISR_Disable(level);
	u32 site = hcSiteOf();
	u32 key = (u32)(uintptr_t)p, i = hcHash(key), k;
	for(k = 0; k < HC_ENTRIES; k++, i = (i + 1) & (HC_ENTRIES-1)){
		if(gHc[i].p == key){   // a free this census never saw: replace
			u32 os = gHc[i].szsite & 511, osz = gHc[i].szsite >> 9;
			gHcSite[os].live -= osz; gHcSite[os].count--; gHcLive -= osz; gHcN--;
			gHc[i].p = 0;
		}
		if(gHc[i].p == 0){
			gHc[i].p = key; gHc[i].szsite = ((u32)sz << 9) | site;
			gHcSite[site].live += sz; gHcSite[site].count++; gHcLive += sz; gHcN++;
			break;
		}
	}
	if(k == HC_ENTRIES) gHcLost++;
	_CPU_ISR_Restore(level);
}
static __attribute__((noinline)) void hcDel(void *p)
{
	if(gHc == NULL || p == NULL) return;
	u32 level; _CPU_ISR_Disable(level);
	u32 key = (u32)(uintptr_t)p, i = hcHash(key);
	for(u32 k = 0; k < HC_ENTRIES && gHc[i].p != 0; k++, i = (i + 1) & (HC_ENTRIES-1)){
		if(gHc[i].p != key) continue;
		u32 site = gHc[i].szsite & 511, sz = gHc[i].szsite >> 9;
		gHcSite[site].live -= sz; gHcSite[site].count--; gHcLive -= sz; gHcN--;
		// Backward-shift deletion: no tombstones, probe chains stay intact.
		u32 hole = i, j = (i + 1) & (HC_ENTRIES-1);
		while(gHc[j].p != 0){
			u32 h = hcHash(gHc[j].p);
			bool between = hole <= j ? (h > hole && h <= j) : (h > hole || h <= j);
			if(!between){ gHc[hole] = gHc[j]; hole = j; }
			j = (j + 1) & (HC_ENTRIES-1);
		}
		gHc[hole].p = 0; gHc[hole].szsite = 0;
		break;
	}
	_CPU_ISR_Restore(level);
}
extern "C" void gcHeapCensusDump(const char *why)
{
	if(gHc == NULL) return;
	printf("HCENSUS %s: live %uK in %u blocks (lost %u) | top sites: liveK count pc0 pc1 pc2 pc3\n",
	    why, gHcLive/1024, gHcN, gHcLost);
	u8 done[HC_SITES]; memset(done, 0, sizeof(done));
	for(int t = 0; t < HC_TOP; t++){
		int best = -1;
		for(int i = 0; i < HC_SITES; i++)
			if(!done[i] && gHcSite[i].used && (best < 0 || gHcSite[i].live > gHcSite[best].live)) best = i;
		if(best < 0 || gHcSite[best].live < 4096) break;
		done[best] = 1;
		printf("HC %6uK %6u %08X %08X %08X %08X\n", gHcSite[best].live/1024, gHcSite[best].count,
		    gHcSite[best].pc[0], gHcSite[best].pc[1], gHcSite[best].pc[2], gHcSite[best].pc[3]);
	}
}
#else
static inline void hcAdd(void *, size_t) {}
static inline void hcDel(void *) {}
extern "C" void gcHeapCensusDump(const char *) {}
#endif

// B155: frame profile — microseconds per phase of the game loop, accumulated
// by Idle (main.cpp) and CGame::Process (Game.cpp), printed with the census
// as avg/max ms. The police-chase collapse (ft 17 -> 109 ms) had no breakdown.
enum { PROF_N = 15 };
static const char *gProfName[PROF_N] = {"stream","script","world","pop","game","audio","cnstr","prerender","render","fx","2d","present","effects","droplets","blur"};
static u64 gProfAcc[PROF_N], gProfMax[PROF_N];
static u32 gProfFrame[PROF_N];   // B187: this frame's phases, for the SPIKE line
extern unsigned gxPageIns;       // librw texel store: ARAM → MEM1 window copies
extern "C" unsigned long long gcNowUs(void){ return ticks_to_microsecs(gettime()); }
extern "C" { extern uint32 gStrLoad, gLoadAllN; }   // Streaming.cpp: conversions, blocking LoadAllRequestedModels calls
extern "C" { extern volatile const char *gMainWhere; }   // defined below
extern "C" void gcProfAdd(int id, unsigned long long us)
{
	if(id < 0 || id >= PROF_N) return;
	gProfAcc[id] += us;
	gProfFrame[id] += (u32)us;
	if(us > gProfMax[id]) gProfMax[id] = us;
	// DIAG b176: name the phase behind every >100 ms frame, with what the
	// streamer did inside it. The PROF maxima said "game 352" and nothing else.
	static uint32 lastLoad, lastAll;
	if(us >= 100000)
		printf("SLOW %s %ums where=%s loads=%u loadall=%u\n", gProfName[id], (unsigned)(us/1000),
		    (const char*)gMainWhere, gStrLoad - lastLoad, gLoadAllN - lastAll);
	lastLoad = gStrLoad; lastAll = gLoadAllN;
}
static void gcProfLine(char *out, size_t n, u32 frames)
{
	size_t k = 0;
	for(int i = 0; i < PROF_N && k < n; i++){
		k += snprintf(out + k, n - k, "%s %u/%u ", gProfName[i],
		    (unsigned)(frames ? gProfAcc[i]/frames/1000 : 0), (unsigned)(gProfMax[i]/1000));
		gProfAcc[i] = gProfMax[i] = 0;
	}
}

// B187: the user sees 20 fps in rain, crashes and police chaos, and the
// 300-frame PROF maxima cannot say which phases blew up in the SAME frame.
// One line per >50 ms gameplay frame (at most one a second: OSReport is slow).
static void gcSpikeLine(u32 t)
{
	static u32 lastPage, lastLoad; static u64 lastSaid;
	u32 page = gxPageIns - lastPage, load = gStrLoad - lastLoad;
	lastPage = gxPageIns; lastLoad = gStrLoad;
	if(t > 50000 && gGameState == GS_PLAYING_GAME && !FrontEndMenuManager.m_bMenuActive && !CCutsceneMgr::IsRunning() &&
	   ticks_to_millisecs(gettime() - lastSaid) >= 1000){
		lastSaid = gettime();
		char line[200]; size_t k = snprintf(line, sizeof(line), "SPIKE %ums pagein %u loads %u |", (unsigned)(t/1000), page, load);
		for(int i = 0; i < PROF_N && k < sizeof(line); i++)
			if(gProfFrame[i] >= 3000)
				k += snprintf(line + k, sizeof(line) - k, " %s %u", gProfName[i], (unsigned)(gProfFrame[i]/1000));
		printf("%s\n", line);
	}
	memset(gProfFrame, 0, sizeof(gProfFrame));
}

static void gcFrameSample(u64 start)
{
	gcSpikeLine(ticks_to_microsecs(gettime() - start));
	if(gGameState != GS_PLAYING_GAME) return;
	static u32 samples[300], work[300], count, lastPhase;
	int cap = FrontEndMenuManager.m_PrefsFrameLimiter == CMenuManager::FRAMELIMIT_30 ? 30 :
	    FrontEndMenuManager.m_PrefsFrameLimiter ? 60 : 0;
	u32 phase = cap | (FrontEndMenuManager.m_bMenuActive << 8) | (CCutsceneMgr::IsRunning() << 9);
	if(phase != lastPhase){ count = 0; lastPhase = phase; }
	samples[count++] = ticks_to_microsecs(gettime() - start);
	if(count != ARRAY_SIZE(samples)) return;
	u64 sum = 0;
	u32 slow60 = 0, slow30 = 0;
	for(u32 i = 0; i < count; i++){
		u32 t = samples[i]; sum += t;
		slow60 += t > 17167; slow30 += t > 33833;
		u32 j = i;
		while(j && work[j-1] > t){ work[j] = work[j-1]; j--; }
		work[j] = t;
	}
	printf("FRAMEPERF n=%u cap=%d menu=%d cut=%d avg_us=%u p50=%u p95=%u p99=%u max=%u late60=%u late30=%u\n",
	    count, cap, (phase>>8)&1, (phase>>9)&1, (unsigned)(sum/count),
	    work[149], work[284], work[296], work[299], slow60, slow30);
	count = 0;
}

extern "C" void *__wrap_malloc(size_t sz)
{
	void *p = __real_malloc(sz);
	if(p == NULL) p = gcHeapFail(sz, 8, "malloc");
	if(p && !gcBigContains(p)) hcAdd(p, sz);   // chunk blocks are counted by gcBigAlloc itself
	return p;
}
extern "C" void *__wrap_memalign(size_t al, size_t sz)
{
	void *p = __real_memalign(al, sz);
	if(p == NULL) p = gcHeapFail(sz, al, "memalign");
	if(p && !gcBigContains(p)) hcAdd(p, sz);
	return p;
}
extern "C" void *__wrap_calloc(size_t n, size_t sz)
{
	void *p = __real_calloc(n, sz);
	if(p == NULL && n*sz){ p = gcHeapFail(n*sz, 8, "calloc"); if(p) memset(p, 0, n*sz); }
	if(p && !gcBigContains(p)) hcAdd(p, n*sz);
	return p;
}
// B179: newlib's free() trusts the header in front of the pointer. A garbage
// or already-free pointer makes it merge a fake chunk into top: b178b/b179c
// ended with top = p-8, malloc handing out 0x83f637a0 and the game frozen
// (b179c: Geometry::destroy freeing the uninitialised attribBase of a failed
// native load, fixed in librw B180).
// Check the header first; a bad free is logged with its caller and leaked.
static bool
gcNewlibOwns(void *p, const char *what)
{
	if(__malloc_sbrk_base == nil || __malloc_sbrk_base == (char*)-1)
		return true;
	u8 *c = (u8*)p - 8, *top = (u8*)__malloc_av_[2];
	u8 *base = (u8*)(((u32)__malloc_sbrk_base + 7) & ~7u);
	const char *why = nil;
	if(((u32)p & 7) || c < base || c >= top)
		why = "outside the heap";
	else{
		u32 size = *(u32*)(c + 4) & ~3u;
		if(size < 16 || (size & 7) || size > (u32)(top - c))
			why = "bad size";
		else if(!(*(u32*)(c + size + 4) & 1))
			why = "already free";
	}
	if(why == nil)
		return true;
	static u32 said;
	if(said++ < 20){
		char stack[100]; gcStackLine(stack, sizeof(stack));
		printf("BADFREE %s %p (%s) at%s\n", what, p, why, stack);
	}
	return false;
}
extern "C" void __wrap_free(void *p)
{
	if(p == NULL) return;
	if(gcBigContains(p)){ gcBigFree(p); return; }   // B95: a plain free() on a chunk block corrupted newlib's heap (warped geometry)
	hcDel(p);
	if(gcInArena(p)){
		u32 level; _CPU_ISR_Disable(level);
		if(gArenaLive && --gArenaLive == 0) gArenaUsed = 0;   // everything carved is gone: start over
		_CPU_ISR_Restore(level);
		return;
	}
	if(!gcNewlibOwns(p, "free"))
		return;
	__real_free(p);
}
extern "C" void *__wrap_realloc(void *p, size_t sz)
{
	if(p == NULL) return __wrap_malloc(sz);
	if(sz == 0){ __wrap_free(p); return NULL; }
	if(gcBigContains(p)){   // B95: chunk block through the plain realloc() path
		size_t old = gcBigSizeOf(p);
		void *q = __wrap_malloc(sz);
		if(q){ memcpy(q, p, old < sz ? old : sz); gcBigFree(p); }
		return q;
	}
	if(!gcInArena(p)){
		if(!gcNewlibOwns(p, "realloc"))
			return NULL;
		void *q = __real_realloc(p, sz);
		if(q){ hcDel(p); hcAdd(q, sz); return q; }
		size_t old = malloc_usable_size(p);
		q = gcHeapFail(sz, 8, "realloc");
		if(q){ memcpy(q, p, old < sz ? old : sz); hcDel(p); __real_free(p); if(!gcBigContains(q)) hcAdd(q, sz); }
		return q;
	}
	uint32 old = *(uint32*)((uint8*)p - 4);
	void *q = __wrap_malloc(sz);
	if(q){ memcpy(q, p, old < sz ? old : sz); __wrap_free(p); }
	return q;
}
extern "C" void gcHeapReserveArm(void) { }   // B78: the arena replaced the malloc'd reserve

// B79: big-block heap. B76-B78 died in a fragmented single heap: 1.4MB free in
// holes with no 37K one. Every RenderWare allocation of 8K or more (geometry,
// skins, TXD dictionaries, window spills) now lives in 1MB chunks with
// first-fit + coalescing; the general heap keeps the small long-lived objects
// that were splitting the holes. A chunk is taken from the general heap when
// needed and handed back when empty, so nothing has to be sized at boot; when
// no chunk can be had the caller falls back to the general heap as before.
enum { BIG_CHUNK = 2048*1024, BIG_CHUNKS = 7, BIG_SPANS = 1024, BIG_MIN = 1024 };   // B106: spans 512->1024 for 1K routing (MemoryMgr.cpp now routes >= BIG_MIN); 12 bytes each   // B100: 2MB chunks (a 700K mesh needs one hole), 1K+ routed (B98 died on 3.6K mallocs with 1.6MB free in chunks)
struct BigSpan { uint32 addr, size; uint8 used; };
struct BigChunk { uint8 *base; BigSpan sp[BIG_SPANS]; int32 n; uint32 used; uint32 size; };   // B114: size per chunk (2MB, or 1MB when no 2MB hole exists after boot)
static BigChunk gBig[BIG_CHUNKS];
static int32 gBigChunks;
// B110: 0 while booting — small blocks stay in the general heap and empty
// chunks go back to it, exactly the pre-B106 boot that fit. 1 from the
// first GS_PLAYING_GAME frame on — the three chunks are kept and 1-4K RW
// blocks live in the third one. B108/B109 tried permanence from the start
// and starved CGame::Initialise of general heap twice.
static int gBigKeep;
extern "C" int gcBootDone(void) { return gBigKeep; }   // B144/B151: 1 once LoadLevel is done — the streaming floor, admission control and loader bound wait for it
static int gcBigAddChunk(void);
extern "C" void gcBigReport(void);
// B151: called at the end of CGame::Initialise's LoadLevel (Game.cpp). The main
// script's first LOAD_SCENE used to run before the GS_PLAYING_GAME frame set
// this, so the initial world set filled the heap with no floor and the
// hotel's collision zones failed at "boot" (b146/b150: car under the map).
extern "C" void gcBootLevelLoaded(void)
{
	if(gBigKeep) return;
	gBigKeep = 1;
	while(gBigChunks < 3 && gcBigAddChunk()) ;
	printf("HEAP: chunks kept from now: %d\n", (int)gBigChunks);
	gcBigReport();
	extern unsigned gxViTVMode, gxHaveComponent, gxXfbHeight;
	printf("VIDEO mode %u (%s) component cable %u xfb %u\n", gxViTVMode,
	    (gxViTVMode & 3) == 2 ? "progressive" : "interlaced", gxHaveComponent, gxXfbHeight);
}
static uint32 gBigUsed, gBigFails;
static void *gcBigAllocIn(BigChunk *c, uint32 size, int32 begin = 0)
{
	for(int32 i = begin; i < c->n; i++){
		BigSpan *s = &c->sp[i];
		if(s->used || s->size < size) continue;
		if(s->size > size && c->n < BIG_SPANS){
			memmove(s+2, s+1, sizeof(BigSpan)*(c->n-i-1));
			s[1].addr = s->addr + size; s[1].size = s->size - size; s[1].used = 0;
			s->size = size; c->n++;
		}
		s->used = 1; c->used += s->size; gBigUsed += s->size;
		return c->base + s->addr;
	}
	return NULL;
}
static int gcBigAddChunk(void)   // interrupts ON: newlib's lock must be free to block
{
	uint32 size = BIG_CHUNK;
	// B131: the three permanent chunks come straight from the arena, not
	// from newlib. b129/b130 proved a second memalign(2MB) fails at every
	// boot moment (only [2048K,1024K,1024K] ever came back); the arena
	// itself has the room. They are never freed (gcBigFree keeps k < 3).
	// B135: newlib only. B131-B134 carved chunks from the arena
	// (SYS_SetArenaLo) and every run since had impossible heap states
	// (boot starving with chunks empty, 100-byte texture allocs failing):
	// libogc's sbrk evidently keeps its own heap end, so those chunks
	// overlapped the heap. The b126-b130 layout [2MB,1MB,1MB] is the one
	// that boots in 35 s and plays.
	uint8 *base = (uint8*)__real_memalign(32, size);
	if(base == NULL){   // B114: after boot there is no 2MB hole, but a 1MB one usually exists — play ran on ONE chunk ("chunks kept from now: 1")
		size = BIG_CHUNK/2;
		base = (uint8*)__real_memalign(32, size);
	}
	if(base == NULL){
		static int said; if(said++ < 3) printf("HEAP: big chunk %d: no 1MB hole (free %uK)\n", gBigChunks, (unsigned)(mallinfo().fordblks/1024));
		return 0;
	}
	u32 level; _CPU_ISR_Disable(level);
	BigChunk *c = &gBig[gBigChunks++];
	c->base = base; c->size = size; c->n = 1; c->used = 0; c->sp[0].addr = 0; c->sp[0].size = size; c->sp[0].used = 0;
	_CPU_ISR_Restore(level);
	printf("HEAP: chunk %d = %uK at %p (heap free %uK)\n", (int)gBigChunks, (unsigned)(size/1024), base, (unsigned)(mallinfo().fordblks/1024));
	return 1;
}
// B127: carve the three chunks FIRST THING in main, while the heap is one
// hole. b125/b126 census read 'big 3/2417K/654K': the lazy priming (first RW
// block, after console, filesystem and RW init) only found 1MB holes, so 3MB
// of chunks held a 6-8MB streaming set and the rest sliced the general heap
// into crumbs — OOM at the docks (b125), 2622 failed loads in 18 min (b126).
static uint32 gPrimeFreeK, gPrimeArenaK;
extern "C" void gcBigPrime(void)
{
	struct mallinfo mi = mallinfo(); gPrimeFreeK = mi.fordblks/1024; gPrimeArenaK = mi.arena/1024;
	while(gBigChunks < 3 && gcBigAddChunk()) ;
}
// B130: at main start newlib's arena is 618K (sbrk grows lazily) — b129's
// prime got one 2MB chunk and two 1MB fallbacks. Called again once the
// arena has grown (after rsINITIALIZE, and at GS_INIT_ONCE): empty
// sub-2MB chunks go back and 2MB ones are carved in their place.
extern "C" void gcBigReprime(void)
{
	for(int32 k = gBigChunks - 1; k >= 0; k--){
		BigChunk *c = &gBig[k];
		if(c->size < BIG_CHUNK && c->used == 0){
			u32 level; _CPU_ISR_Disable(level);
			__real_free(c->base);
			*c = gBig[--gBigChunks];
			_CPU_ISR_Restore(level);
		}
	}
	while(gBigChunks < 3 && gcBigAddChunk()) ;
}
// The boot console eats printf until the game loop; this line reaches the log.
extern "C" void gcBigReport(void)
{
	struct mallinfo mi = mallinfo();
	printf("HEAP: prime saw free %uK arena %uK; now arena %uK free %uK; chunks:", (unsigned)gPrimeFreeK, (unsigned)gPrimeArenaK, (unsigned)(mi.arena/1024), (unsigned)(mi.fordblks/1024));
	for(int32 k = 0; k < gBigChunks; k++) printf(" [%d %uK used %uK]", (int)k, (unsigned)(gBig[k].size/1024), (unsigned)(gBig[k].used/1024));
	printf("\n");
}
// B134: the general heap is gone — any chunk, any size, no boot preference.
void *gcBigAllocAny(size_t sz)
{
	uint32 size = ((uint32)sz + 31) & ~31u;
	if(size > BIG_CHUNK - 64) return NULL;
	u32 level; _CPU_ISR_Disable(level);
	void *p = NULL;
	for(int32 k = 0; k < gBigChunks && p == NULL; k++) p = gcBigAllocIn(&gBig[k], size);
	_CPU_ISR_Restore(level);
	if(p) hcAdd(p, sz);
	return p;
}
extern "C" void *gcBigAlloc(size_t sz)
{
	uint32 size = ((uint32)sz + 31) & ~31u;
	if(size > BIG_CHUNK - 64) return NULL;   // B98: anything that fits a chunk (B97 died on an ~800K mesh block with 873K free in a chunk)
	static int primed;
	if(!primed){   // B82: take the first chunks while the heap is still one big hole; B81 got exactly one
		primed = 1;
		while(gBigChunks < 3 && gcBigAddChunk()) ;   // B85: 5 starved the general heap at boot (3K mallocs failing)
	}
	for(int attempt = 0; attempt < 2; attempt++){
		u32 level; _CPU_ISR_Disable(level);
		void *p = NULL;
		// B108: blocks under 4K live only in the third primed chunk, so the
		// first two keep their big spans. B106 let 1-4K blocks fill every
		// chunk from the start of LoadLevel; the first large RW block then
		// found no span, the general heap could not serve it either, and
		// gcHeapFail ran before streaming existed (see gcStreamEmergencyShed).
		// Once that chunk is full, small blocks fall back to the general heap.
		// B115: small blocks prefer the LAST chunk (whatever its index — b114 ran
		// on two chunks and the "third chunk only" rule refused every 1-4K block:
		// 'OOM need 1K' with 840K free inside the chunks), then any chunk.
		if(size < 4096){
			if(gBigKeep && gBigChunks >= 1) p = gcBigAllocIn(&gBig[gBigChunks-1], size);
			if(gBigKeep) for(int32 k = 0; k < gBigChunks && p == NULL; k++) p = gcBigAllocIn(&gBig[k], size);
		}
		else for(int32 k = 0; k < gBigChunks && p == NULL; k++) p = gcBigAllocIn(&gBig[k], size);
		_CPU_ISR_Restore(level);
		if(p){ hcAdd(p, sz); return p; }
		// B147: while booting, three chunks at most. LoadLevel otherwise grows
		// them to seven (2MB + 6x1MB) out of the general heap it is still
		// filling, and its 140-378 B collision records then fail (b145: four
		// COL zones dropped at boot -> the car under the map later).
		if(gBigChunks >= BIG_CHUNKS || !gcBigAddChunk()) break;   // B152: B147's boot cap dropped — b130's dynamics (extras come and go while booting) booted with zero failures
	}
	gBigFails++;
	return NULL;
}
// B109: hand one EMPTY chunk back to the general heap; called only from a
// failed general-heap allocation (gcHeapFail). Empty chunks are the boot
// case; in play every chunk holds RW blocks and this finds nothing.
static int gcBigReleaseEmpty(void)
{
	u32 level; _CPU_ISR_Disable(level);
	int released = 0;
	for(int32 k = gBigChunks - 1; k >= 1; k--){   // B136: chunk 0 stays; empty others feed a starving boot (B109/B110)
		if(gBig[k].used == 0){
			uint8 *base = gBig[k].base;
			gBig[k] = gBig[--gBigChunks];
			_CPU_ISR_Restore(level);
			__real_free(base);
			printf("HEAP: empty chunk released to the general heap (chunks now %d)\n", (int)gBigChunks);
			released = 1;
			break;
		}
	}
	if(!released) _CPU_ISR_Restore(level);
	return released;
}
static BigChunk *gcBigChunkOf(const void *p)
{
	for(int32 k = 0; k < gBigChunks; k++)
		if((const uint8*)p >= gBig[k].base && (const uint8*)p < gBig[k].base + gBig[k].size) return &gBig[k];
	return NULL;
}
extern "C" int gcBigContains(const void *p) { return gcBigChunkOf(p) != NULL; }
extern "C" size_t gcBigSizeOf(const void *p)
{
	BigChunk *c = gcBigChunkOf(p); if(c == NULL) return 0;
	uint32 addr = (uint32)((const uint8*)p - c->base);
	for(int32 i = 0; i < c->n; i++) if(c->sp[i].addr == addr && c->sp[i].used) return c->sp[i].size;
	return 0;
}
extern "C" int gcBigResize(void *p, size_t bytes)
{
	u32 level; _CPU_ISR_Disable(level);
	BigChunk *c = gcBigChunkOf(p);
	uint32 size = ((uint32)bytes + 31) & ~31u;
	if(c && size){
		uint32 addr = (uint32)((uint8*)p - c->base);
		for(int32 i = 0; i < c->n; i++){
			BigSpan *s = &c->sp[i];
			if(s->addr != addr || !s->used || size > s->size) continue;
			uint32 tail = s->size - size;
			if(tail && i+1 < c->n && !s[1].used){
				s[1].addr -= tail;
				s[1].size += tail;
			}else if(tail && c->n < BIG_SPANS){
				memmove(s+2, s+1, sizeof(BigSpan)*(c->n-i-1));
				s[1].addr = addr + size; s[1].size = tail; s[1].used = 0;
				c->n++;
			}else if(tail){
				_CPU_ISR_Restore(level);
				return 1;
			}
			s->size = size; c->used -= tail; gBigUsed -= tail;
			_CPU_ISR_Restore(level);
			return 1;
		}
	}
	_CPU_ISR_Restore(level);
	return 0;
}

extern "C" void *gcBigMove(void *p)
{
	u32 level; _CPU_ISR_Disable(level);
	BigChunk *c = gcBigChunkOf(p);
	if(c){
		uint32 addr = (uint32)((uint8*)p - c->base);
		for(int32 i = 0; i < c->n; i++){
			BigSpan *s = &c->sp[i];
			if(s->addr != addr || !s->used) continue;
			if(i == 0 || s[-1].used){
				if(i+1 == c->n || s[1].used) break;
				uint32 size = s->size, limit = size + s[1].size;
				uint32 best = size + size/8 + 32;
				if(best > limit) best = limit;
				BigChunk *target = NULL;
				for(int32 k = 0; k < gBigChunks; k++)
					for(int32 j = 0; j < gBig[k].n; j++){
						BigSpan *hole = &gBig[k].sp[j];
						if(!hole->used && hole->size >= size && hole->size < best){
							target = &gBig[k]; best = hole->size;
						}
					}
				if(target){
					// Allocate the best fitting hole, leaving the large tail intact.
					int32 bestIndex = -1;
					for(int32 j = 0; j < target->n; j++)
						if(!target->sp[j].used && target->sp[j].size == best){ bestIndex = j; break; }
					void *moved = gcBigAllocIn(target, size, bestIndex);
					memcpy(moved, p, size);
					gcBigFree(p);
					_CPU_ISR_Restore(level);
					return moved;
				}
				break;
			}
			uint32 size = s->size, gap = s[-1].size, start = s[-1].addr;
			void *moved = c->base + start;
			memmove(moved, p, size);
			s[-1].size = size; s[-1].used = 1;
			s->addr = start + size; s->size = gap; s->used = 0;
			if(i+1 < c->n && !s[1].used){
				s->size += s[1].size;
				memmove(s+1, s+2, sizeof(BigSpan)*(c->n-i-2)); c->n--;
			}
			_CPU_ISR_Restore(level);
			return moved;
		}
	}
	_CPU_ISR_Restore(level);
	return p;
}

extern "C" void gcBigFree(void *p)
{
	hcDel(p);
	u32 level; _CPU_ISR_Disable(level);
	BigChunk *c = gcBigChunkOf(p);
	if(c){
		uint32 addr = (uint32)((uint8*)p - c->base);
		for(int32 i = 0; i < c->n; i++){
			BigSpan *s = &c->sp[i];
			if(s->addr != addr || !s->used) continue;
			s->used = 0; c->used -= s->size; gBigUsed -= s->size;
			if(i+1 < c->n && !s[1].used){ s->size += s[1].size; memmove(s+1, s+2, sizeof(BigSpan)*(c->n-i-2)); c->n--; }
			if(i > 0 && !s[-1].used){ s[-1].size += s->size; memmove(s, s+1, sizeof(BigSpan)*(c->n-i-1)); c->n--; }
			break;
		}
		// B106: the three primed chunks are permanent. Released once empty after
		// boot they could never come back (no contiguous 2MB hole exists later),
		// so play ran on ONE chunk ('big 1' in every census) and RW blocks spilled
		// into the fragmented general heap. Extras beyond the primed three still go.
		// B128: the three primed chunks are NEVER released. B110 let them go
		// while booting (gBigKeep 0): the splash TXD's block was the first
		// thing in chunk 1, its free emptied the chunk, the 2MB hole was eaten
		// by boot allocations and every later chunk was a 1MB one (b127 log:
		// 'chunk 3 = 1024K' x15, census 'big 3/3045K'). Extras beyond three
		// still go back when empty.
		if(c->used == 0 && (c - gBig) >= 1 && !(gBigKeep && gBigChunks <= 3)){   // B136: chunk 0 (the 2MB one) stays; the others follow B110 (back to the heap while booting)
			__real_free(c->base);
			*c = gBig[--gBigChunks];
		}
	}
	_CPU_ISR_Restore(level);
}
// total free the streamer may count on: general holes plus what the chunks still hold
extern "C" size_t gcHeapFreeTotal(void)
{
	size_t f = mallinfo().fordblks;
	for(int32 k = 0; k < gBigChunks; k++) f += gBig[k].size - gBig[k].used;
	return f;
}
extern "C" void gcBigStats(unsigned *chunks, unsigned *usedK, unsigned *freeK, unsigned *largestK, unsigned *fails)
{
	uint32 largest = 0;
	for(int32 k = 0; k < gBigChunks; k++)
		for(int32 i = 0; i < gBig[k].n; i++)
			if(!gBig[k].sp[i].used && gBig[k].sp[i].size > largest) largest = gBig[k].sp[i].size;
	uint32 total = 0; for(int32 k = 0; k < gBigChunks; k++) total += gBig[k].size;
	*chunks = gBigChunks; *usedK = gBigUsed/1024; *freeK = (total - gBigUsed)/1024; *largestK = largest/1024; *fails = gBigFails;
}
extern "C" void
__wrap___eabi(void)
{
	u32 level;
	_CPU_ISR_Disable(level);
	__real___eabi();
	_CPU_ISR_Restore(level);
}

long _dwOperatingSystemVersion = OS_WINXP;
size_t _dwMemAvailPhys;
RwUInt32 gGameState;




static void *framebuffer;
static GXRModeObj *videoMode;
static psGlobalType platformState;
static RwBool fileSystemReady;
// ponytail: assets (1.5GB) exceed a 1.35GB GameCube disc, so SD Gecko is the
// only medium that fits them whole. Mounted as "dvd" so every existing
// "dvd:/" path resolves unchanged; real DVD stays as fallback.
static bool fileSystemIsFat;

double
psTimer(void)
{
	return ticks_to_millisecs(gettime());
}

/*
 * Crash handling. libogc's default panic screen scans the pad and treats a
 * held A as "Reset" and Z as "Reload" — so crashing right after a menu
 * selection instantly wipes the dump. This replacement never reads input, so
 * the red screen stays until the emulator/console is reset.
 */

extern "C" {
void VIDEO_SetFramebuffer(void *fb);
void __VIClearFramebuffer(void *fb, u32 size, u32 color);
void __console_init(void *fb, int xstart, int ystart, int xres, int yres, int stride);
}


static void
panicPrintf(const char *fmt, ...)
{
	char line[192];
	va_list va;
	va_start(va, fmt);
	vsnprintf(line, sizeof(line), fmt, va);
	va_end(va);

	fputs(line, stdout);
}

// Crash record for Dolphin's MemoryWatcher: the console below is invisible
// under Dolphin's XFB emulation and eats stdout, so a crash looked like a freeze.
extern "C" { volatile unsigned gCrashId, gCrashPc, gCrashLr, gCrashSp, gCrashStack[8]; }
extern "C" { extern volatile const char *gMainWhere; }
static void
gcPanic(unsigned exid, PPCContext *ctx)
{
	static volatile bool inPanic;
	if(inPanic)
		for(;;)
			;
	inPanic = true;
	gCrashId = exid; gCrashPc = ctx->pc; gCrashLr = ctx->lr; gCrashSp = ctx->gpr[1];
	{
		u32 sp = ctx->gpr[1];
		for(int i = 0; i < 8 && sp && sp != 0xFFFFFFFF && (sp & 3) == 0 && sp >= 0x80000000u && sp < 0x81800000u; i++){
			u32 *frame = (u32*)sp;
			gCrashStack[i] = frame[1];
			sp = frame[0];
		}
	}
	fprintf(stderr, "CRASH exid %u pc %08X lr %08X sp %08X where %s stack %08X %08X %08X %08X %08X %08X\n",
	    exid, ctx->pc, ctx->lr, ctx->gpr[1], (const char*)gMainWhere,
	    gCrashStack[0], gCrashStack[1], gCrashStack[2], gCrashStack[3], gCrashStack[4], gCrashStack[5]);

	// Stop the world FIRST. With interrupts live the decrementer keeps
	// scheduling other threads, and the next game frame flips the
	// framebuffer back — the red screen becomes a one-frame flash.
	u32 level;
	_CPU_ISR_Disable(level);
	(void)level;

	// Tuxedo exception IDs are sparse and start at one. Keeping this table
	// indexed by the raw ID matters: the old compact table reported ISI (4)
	// as "Interrupt", sending diagnosis toward the wrong hardware subsystem.
	static const char *const names[] = {
		"Unknown", "Reset", "MachineCheck", "DSI", "ISI", "Interrupt",
		"Alignment", "Program", "FPU", "Decrementer", "Unknown", "Unknown",
		"Syscall", "Trace", "Unknown", "Performance", "Unknown", "Unknown",
		"Unknown", "IABR"
	};

	GX_AbortFrame();
	void *xfb = (void*)0xC1700000;
	VIDEO_SetFramebuffer(xfb);
	__VIClearFramebuffer(xfb, 640*480*VI_DISPLAY_PIX_SZ, COLOR_MAROON);
	__console_init(xfb, 48, 48, 640-96, 480-96, 2*640);

	panicPrintf("reVC crash: %s exception\n",
	    exid < sizeof(names)/sizeof(names[0]) ? names[exid] : "?");
	for(unsigned i = 0; i < 8; i++)
		panicPrintf("GPR%02u %08X GPR%02u %08X GPR%02u %08X GPR%02u %08X\n",
		    i, ctx->gpr[i], i+8, ctx->gpr[i+8],
		    i+16, ctx->gpr[i+16], i+24, ctx->gpr[i+24]);
	panicPrintf("PC %08X LR %08X CTR %08X CR %08X\n",
	    ctx->pc, ctx->lr, ctx->ctr, ctx->cr);

	panicPrintf("STACK:");
	u32 sp = ctx->gpr[1];
	for(int i = 0; i < 12 && sp && sp != 0xFFFFFFFF && (sp & 3) == 0 &&
	    sp >= 0x80000000u && sp < 0x81800000u; i++){
		u32 *frame = (u32*)sp;
		panicPrintf(" %08X", frame[1]);
		sp = frame[0];
	}

	for(;;)
		;
}

static void
gcInstallPanicHandler(void)
{
	PPCExcptCurPanicFn = gcPanic;
}

// Fatal-but-not-exception ends (assert, abort, exit) return to the loader,
// which in Dolphin batch mode just quits the emulator and the message is
// never seen. Park on a readable screen instead. These run in normal thread
// context, so unlike gcPanic they can write crash.log before stopping the
// world. The stack walk names the caller (symbolize with addr2line).
// Non-static: sampman's fail-loud audio path parks through here too.

// Freeze watchdog (B48). The main loop bumps gMainTick every iteration and
// stamps gMainWhere at the stages that can block (streaming, CD sync, texture
// load, page-in, voice arm, audio service, present). The VI retrace interrupt
// watches: three seconds without a tick prints the last checkpoint. B42/B45/
// B47 stopped both threads at once with the CPU idle — this says where, and
// if this line itself stops, interrupts are off.
extern "C" { volatile const char *gMainWhere = "boot"; }
volatile uint32 gMainTick;
static uint64 gFtSum;
static uint32 gFtMax, gFtN;
static float gMeasuredFps;
static uint64 gFpsSum;
static uint32 gFpsN;
extern "C" { volatile unsigned gVblTick; }
static void
gcRetraceWatch(u32 rc)
{
	(void)rc;
	// No printf here: stdio locks in interrupt context. Dolphin's MemoryWatcher
	// samples these counters from outside the emulated CPU (scratch memwatch.py).
	gVblTick++;
}

// One line of heap truth, shared by the death screen, the OOM exit and the
// in-game tick. str = what CStreaming counts (the budget only governs this),
// tex = tiled texels resident in MEM1 (dca3 keeps these in VRAM), dl =
// recorded display lists. Whatever is left of "used" after those three is
// engine + pools + paths + everything loaded outside the streamer's
// accounting — the fixed set the budget knob cannot touch.
// Bytes a CPool holds: its slot array plus one flag byte a slot. What the
// dca3 pool profile in config.h actually costs, measured instead of assumed.
template<class T, class U> static size_t
poolBytes(CPool<T,U> *p)
{
	return p ? (size_t)p->GetSize()*(sizeof(U) + 1) : 0;
}

extern "C" unsigned gIsoRdN, gIsoRdJumps;   // dvdfs.c: sector reads issued, and how many were seeks
extern "C" void fsDiscStatsPrint(void);
extern "C" int gcVoiceCensusLine(char *out, int cap);   // sampman_gamecube.cpp (B155)
extern unsigned gxColorBytes;   // gxraster.cpp
extern unsigned gcMemoryMoves, gcMemoryMovedBytes;
extern "C" void gcStreamClassCensus(unsigned out[6]);   // B88
static void
gcHeapLine(char *out, size_t n)
{
	unsigned bigC, bigU, bigF, bigL, bigX; gcBigStats(&bigC, &bigU, &bigF, &bigL, &bigX);
	unsigned cls[6]; gcStreamClassCensus(cls);
	struct mallinfo mi = mallinfo();
	extern unsigned gxTiledBytes;
	size_t pools = poolBytes(CPools::GetPtrNodePool()) + poolBytes(CPools::GetEntryInfoNodePool()) +
	    poolBytes(CPools::GetPedPool()) + poolBytes(CPools::GetVehiclePool()) +
	    poolBytes(CPools::GetBuildingPool()) + poolBytes(CPools::GetTreadablePool()) +
	    poolBytes(CPools::GetObjectPool()) + poolBytes(CPools::GetDummyPool()) +
	    poolBytes(CPools::GetAudioScriptObjectPool()) + poolBytes(CPools::GetColModelPool());
	extern unsigned gxAramBytes, gxWsBytes, gxWsPeak, gxPageIns, gxWsStarved, gxWsForced, gxSpills, gxWsFrameBytes, gxWsFramePeak, gxShareBytes, gxWsShared;
	extern unsigned gxPageLevels[4], gxWsRetired;
	extern uint32 gStrEvict;
	extern unsigned gStreamStarvedTotal, gStreamDecPumps, gVoiceStarvedTotal, gVoicePumps;
	snprintf(out, n, "heap used %uK free %uK big %u/%uK/%uK/%uK/%u | str %uK (b%u c%u v%u p%u t%u o%u) tex %uK dl %uK col %uK pools %uK | aram %uK share %uK ws %uK/%uK wsframe %uK/%uK pagein %u/%u lvl %u/%u/%u/%u ret %u starve %u/%u spill %u evict %u | snd starved %u pumps %u voices starved %u pumps %u | dvd reads %u seeks %u | ft %u/%ums",
	    (unsigned)(mi.arena - mi.fordblks)/1024, (unsigned)mi.fordblks/1024, bigC, bigU, bigF, bigL, bigX,
	    (unsigned)(CStreaming::ms_memoryUsed/1024), cls[0]/1024, cls[1]/1024, cls[2]/1024, cls[3]/1024, cls[4]/1024, cls[5]/1024, gxTiledBytes/1024,
	    (unsigned)(rw::gx::gxDlBytes/1024), (unsigned)(gxColorBytes/1024), (unsigned)(pools/1024),
	    gxAramBytes/1024, gxShareBytes/1024, gxWsBytes/1024, gxWsPeak/1024, gxWsFrameBytes/1024, gxWsFramePeak/1024, gxPageIns, gxWsShared, gxPageLevels[0], gxPageLevels[1], gxPageLevels[2], gxPageLevels[3], gxWsRetired, gxWsStarved, gxWsForced, gxSpills, (unsigned)gStrEvict, gStreamStarvedTotal, gStreamDecPumps, gVoiceStarvedTotal, gVoicePumps, gIsoRdN, gIsoRdJumps, gFtN ? (unsigned)(gFtSum/gFtN/1000) : 0, gFtMax/1000);
	gFtSum = gFtMax = gFtN = 0;
}

// B97: the census on screen (Graphics > Stats HUD). Refreshed every 30 frames.
extern "C" { signed char gcStatsHud; }
extern "C" float gcFramesPerSecond(void) { return gMeasuredFps; }
extern "C" unsigned gcFrameMsAvg(void) { return gFtN ? (unsigned)(gFtSum/gFtN/1000) : 0; }
extern "C" unsigned gcFrameMsMax(void) { return gFtMax/1000; }
extern "C" const char *gcStatsText(void)
{
	static char line[520]; static uint32 last;
	if(line[0] == 0 || gMainTick - last >= 30){ gcHeapLine(line, sizeof(line)); last = gMainTick; }
	return line;
}

// librw's must-allocate calls this before it exits: this OSReport line is
// the only record of what the heap looked like when a required allocation
// failed.
extern "C" unsigned gcRasterTiledBytes(void *raster);   // gxraster.cpp

extern "C" void
gcOomReport(size_t need)
{
	char heap[560];
	gcHeapLine(heap, sizeof(heap));
	printf("OOM need %uK | %s\n", (unsigned)(need/1024), heap);
	// The bill behind "tex": every resident dictionary, its refs and the
	// tiled bytes its textures hold. GetSlot is nil on a free slot.
	for(int i = 0; i < TXDSTORESIZE; i++){
		TxdDef *def = CTxdStore::GetSlot(i);
		if(def == nil || def->texDict == nil)
			continue;
		unsigned bytes = 0; int count = 0;
		FORLIST(lnk, def->texDict->textures){
			bytes += gcRasterTiledBytes(rw::Texture::fromDict(lnk)->raster);
			count++;
		}
		if(bytes)
			printf("  txd %-20s refs %d tex %d %uK\n", def->name, def->refCount, count, bytes/1024);
	}
}
void
gcFatalPark(const char *tag, const char *msg)
{
	// OSReport first: the park screen is only pixels, and log silence has
	// been misread as a freeze more than once. This line lets host-side
	// monitors catch every park without a screenshot.
	fprintf(stderr, "FATAL %s %s\n", tag, msg);
	char stack[220];
	int n = 0;
	u32 sp = (u32)(uintptr_t)__builtin_frame_address(0);
	for(int i = 0; i < 10 && sp && (sp & 3) == 0 &&
	    sp >= 0x80000000u && sp < 0x81800000u; i++){
		u32 *frame = (u32*)sp;
		n += snprintf(stack + n, sizeof(stack) - n, " %08X", frame[1]);
		if(n >= (int)sizeof(stack) - 10)
			break;
		sp = frame[0];
	}

	char heap[560];
	gcHeapLine(heap, sizeof(heap));

	u32 level;
	_CPU_ISR_Disable(level);
	(void)level;

	void *xfb = (void*)0xC1700000;
	VIDEO_SetFramebuffer(xfb);
	__VIClearFramebuffer(xfb, 640*480*VI_DISPLAY_PIX_SZ, COLOR_MAROON);
	__console_init(xfb, 48, 48, 640-96, 480-96, 2*640);
	printf("reVC %s\n%s%s\nSTACK:%s\n", tag, msg, heap, stack);
	// PARK, and never power off.
	//
	// This used to call SYS_ResetSystem(SYS_POWEROFF) so a batch-mode Dolphin
	// would close itself instead of sitting on the screen forever. On a real
	// console that turns it OFF - the user watched it happen at the end of a
	// load - and it takes the one thing worth having with it: this screen
	// names the failure, and nobody can read it after the power goes. The
	// dump is already in crash.log by now either way, but the screen is what
	// gets read first.
	for(;;)
		;
}

extern "C" void
__assert_func(const char *file, int line, const char *func, const char *failedexpr)
{
	char msg[256];
	snprintf(msg, sizeof(msg), "%s:%d\n%s\n%s\n",
	    file, line, func ? func : "?", failedexpr ? failedexpr : "?");
	gcFatalPark("assert", msg);
}

extern "C" void
abort(void)
{
	gcFatalPark("abort", "");
}

extern "C" void
exit(int status)
{
	char msg[32];
	snprintf(msg, sizeof(msg), "status %d\n", status);
	gcFatalPark("exit", msg);
}


// ponytail: returning from main() runs the static destructors, and those free
// through RenderWare's allocator — null unless RwEngineInit ran, so it faults
// with PC=0. A console has nothing to return to, so park here instead.
static void
psHalt(void)
{
	printf("Halted. Press RESET to reboot.\n");
	for(;;)
		VIDEO_WaitVSync();
}

// ponytail: idempotent so main() can print before the engine starts;
// psInitialize calls it again harmlessly.
void
psInitConsole(void)
{
	if(framebuffer != nil)
		return;

	VIDEO_Init();
	VIDEO_SetPostRetraceCallback(gcRetraceWatch);

	videoMode = VIDEO_GetPreferredMode(NULL);
	framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(videoMode));
	// Offer it to the GX backend, which would otherwise allocate a third
	// framebuffer and leave this one stranded. SYS_AllocateFramebuffer takes
	// from the arena permanently, so a stranded buffer is 614KB of MEM1 gone
	// for the whole run — real streaming budget.
	{
		extern void *gxAdoptXfb;
		extern unsigned gxAdoptXfbSize;
		gxAdoptXfb = framebuffer;
		gxAdoptXfbSize = VIDEO_GetFrameBufferSize(videoMode);
	}
	console_init(framebuffer, 20, 20, videoMode->fbWidth, videoMode->xfbHeight,
	    videoMode->fbWidth * VI_DISPLAY_PIX_SZ);

	VIDEO_Configure(videoMode);
	VIDEO_SetNextFramebuffer(framebuffer);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
}

// The Gekko has no fsqrt, and newlib's sqrtf is a bit-by-bit integer loop
// behind an errno wrapper — every Magnitude() in the game, librw's length()
// and all 159 call sites paid for it. The frsqrte estimate (1/32 accurate)
// and three Newton steps in double precision give the float result; dca3
// does the same with the SH4's fsrra. This definition replaces libm's for
// the whole link. Zero, negatives, NaN, infinity and denormals take the
// exact path.
extern "C" float __ieee754_sqrtf(float);
extern "C" float
sqrtf(float x)
{
	if(!(x >= 1.17549435e-38f && x <= 3.40282347e38f))
		return __ieee754_sqrtf(x);
	double y, h = 0.5*(double)x;
	__asm__("frsqrte %0,%1" : "=f"(y) : "f"((double)x));
	y = y*(1.5 - h*y*y);
	y = y*(1.5 - h*y*y);
	y = y*(1.5 - h*y*y);
	return (float)((double)x*y);
}

// Boot check for the sqrtf above: the worst error against libm's exact root
// over a sweep of magnitudes, in float ulps. Expect 0 or 1.
static void
gcSqrtCheck(void)
{
	uint32 worst = 0; float at = 0.0f;
	for(float x = 1.0e-30f; x < 1.0e30f; x *= 1.0137f){
		float a = sqrtf(x), b = __ieee754_sqrtf(x);
		int32 ia, ib;
		memcpy(&ia, &a, 4); memcpy(&ib, &b, 4);
		uint32 d = (uint32)(ia > ib ? ia - ib : ib - ia);
		if(d > worst){ worst = d; at = x; }
	}
	printf("SQRT check: worst %u ulp at %g\n", (unsigned)worst, (double)at);
}

RwBool
psInitialize(void)
{
	psInitConsole();
	PAD_Init();
	RsGlobal.ps = &platformState;

	// GameCube output is fixed 640x480 4:3; keep maximumWidth/Height in sync
	// because camera creation and CameraSize read those, not width/height.
	RsGlobal.width = 640;
	RsGlobal.height = 480;
	RsGlobal.maximumWidth = 640;
	RsGlobal.maximumHeight = 480;
	// skeleton.cpp defaults this to 30, which would make the frame limiter
	// cap at half the retrace rate the moment vsync is off.
	RsGlobal.maxFPS = 60;
	FrontEndMenuManager.m_PrefsUseWideScreen = false;
	// GC defaults; reVC.ini still overrides these when present.
	FrontEndMenuManager.m_nPrefsMSAALevel = 1;
	FrontEndMenuManager.m_nDisplayMSAALevel = 1;
	_dwMemAvailPhys = (size_t)SYS_GetArena1Size();
	gGameState = GS_START_UP;
	return TRUE;
}

void
psTerminate(void)
{
	if(fileSystemReady){
		if(fileSystemIsFat)
			fatUnmount("dvd");
		else
			ISO9660_UnmountDbg("dvd");
		fileSystemReady = FALSE;
	}
}

RwBool
psCameraBeginUpdate(RwCamera *camera)
{
	return RwCameraBeginUpdate(camera) != nil;
}

void
psCameraShowRaster(RwCamera *camera)
{
	// No VIDEO_WaitVSync() here. gx::showRaster already ends with one, and a
	// second wait is a whole field of pure idle: measured, DoRWStuffEndOfFrame
	// ran 28.5ms of which showRaster was 11.9ms, and the 16.6ms remainder is
	// exactly one 59.94Hz field — RwCameraEndUpdate on this backend is a no-op,
	// so there was nothing else it could be. That pushed ~21ms of real work over
	// the 33.3ms budget and every frame fell to the third retrace: 1507 of 1534
	// logged frames were 50.0ms, i.e. a locked 20fps rather than a stutter.
	//
	// This is the consumer the single-retrace change in showRaster missed.
	RwCameraShowRaster(camera, nil, 0);
}

RwImage *
psGrabScreen(RwCamera *)
{
	return nil;
}

void
psMouseSetPos(RwV2d *)
{
}

RwBool
psSelectDevice(void)
{
	return TRUE;
}

extern RwMemoryFunctions memFuncs;   // src/rw/MemoryMgr.cpp
RwMemoryFunctions *
psGetMemoryFunctions(void)
{
	return &memFuncs;   // B80: nil left librw on plain malloc; the B79 big-block routing lives in MemoryMgr
}

RwBool
psInstallFileSystem(void)
{
	if(!fileSystemReady){
		static const DISC_INTERFACE *const sdSlots[] = { &__io_gcsda, &__io_gcsdb };
		static const char *const sdNames[] = { "SD Gecko slot A", "SD Gecko slot B" };
		for(size_t i = 0; i < sizeof(sdSlots)/sizeof(sdSlots[0]); i++){
			printf("mount: probing %s...\n", sdNames[i]);
			// 1MB sector cache (64 pages x 32 sectors); the default is tiny
			// and the whole game streams through this mount.
			if(fatMount("dvd", sdSlots[i], 0, 64, 32)){
				fileSystemReady = TRUE;
				fileSystemIsFat = true;
				break;
			}
		}
		// The disc is the real GameCube medium (the asset set fits a 1.46GB
		// mini-DVD). Probed after SD so the dev card still wins when present.
		// On real hardware with an empty drive this probe blocks forever -
		// startup() and isInserted() both block.
		if(!fileSystemReady){
			printf("mount: probing DVD (ISO9660)...\n");
			{
				extern int FindDevice(const char*);
				printf("mount: FindDevice(dvd:)=%d\n", FindDevice("dvd:"));
			}
			if(ISO9660_MountDbg("dvd", &__io_gcdvd)){
				printf("mount: DVD ISO9660 mounted\n");
				fileSystemReady = TRUE;
				fileSystemIsFat = false;
			}else{
				printf("mount: DVD ISO9660 mount FAILED\n");
				;
			}
		}
		if(!fileSystemReady){
			printf("mount: no storage found. Put the game files in the ROOT of\n");
			printf("       the SD card or a USB drive, formatted FAT32.\n");
			return FALSE;
		}
	}
	if(chdir("dvd:/") == 0){
		// RESOLUTION pref: read for the menu row's sake only. The 528-line
		// EFB experiment is a measured dead end (DispCopyYScale cannot
		// downsample — magenta frame; see startGX), so RsGlobal stays 480
		// whatever the pref says until a real supersample path exists.
		rw::gx::gxReadEfbPref();
		return TRUE;
	}
	printf("mount: chdir dvd:/ FAILED errno=%d\n", errno);
	if(fileSystemIsFat)
		fatUnmount("dvd");
	else
		ISO9660_UnmountDbg("dvd");
	fileSystemReady = FALSE;
	return FALSE;
}

const char *
_psGetUserFilesFolder(void)
{
	// Userfiles (settings dump + story saves) live on the memory card, each
	// as its own CARD file — options and progress separated, and nothing
	// depends on the disc being writable.
	if(GcCardMountDevice()){
		static const char mc[] = "mc:";
		return mc;
	}
	static const char path[] = "dvd:/userfiles";
	return path;
}

RwBool
psNativeTextureSupport(void)
{
	return FALSE;
}

void
_InputTranslateShiftKeyUpDown(RsKeyCodes *)
{
}

long
_InputInitialiseMouse(bool)
{
	return 0;
}

void
_InputShutdownMouse(void)
{
}

bool
_InputMouseNeedsExclusive(void)
{
	return false;
}

void
_InputInitialiseJoys(void)
{
}

void
HandleExit(void)
{
	// Nothing, deliberately.
	//
	// This used to be PAD_ScanPads() followed by "START quits", inherited from
	// the desktop skels where Escape closes the window. On a GameCube START is
	// the pause button, so every press both opened the pause menu and set
	// RsGlobal.quit — the main loop then left on its next condition check and
	// the port sat in shutdown with the menu still on screen. Frozen image, no
	// exception, no crash.log, GP idle, CPU at 5% because the thread was no
	// longer presenting anything. That is the pause-menu freeze this project
	// spent its whole life chasing, and it is why the game's own Save menu
	// never froze: that one is entered by walking into the save marker, not by
	// pressing START.
	//
	// The second PAD_ScanPads was harmful on its own too. It recomputes the
	// down/up edges against the previous scan, so calling it here consumed the
	// press before CapturePad's own scan could see it.
	//
	// There is no "exit" on this console — Dolphin has a stop button and the
	// hardware has reset. If a quit combo is ever wanted it needs a chord that
	// is not a game button, and it must not scan the pads a second time.
}

void
_psSelectScreenVM(RwInt32 videoModeIndex)
{
	_psSetVideoMode(0, videoModeIndex);
}

void
InitialiseLanguage(void)
{
}

RwBool
_psSetVideoMode(RwInt32, RwInt32 videoModeIndex)
{
	return videoModeIndex == 0;
}

RwChar **
_psGetVideoModeList(void)
{
	static RwChar mode[] = "640 X 480 X 32";
	static RwChar *modes[] = { mode };
	return modes;
}

RwInt32
_psGetNumVideModes(void)
{
	return 1;
}

RwBool
IsForegroundApp(void)
{
	return TRUE;
}

// Physical gate of a GameCube stick, in PAD_Stick units. Tune per pad if a
// worn stick still cannot reach full deflection.
#define GC_STICK_RANGE    72
#define GC_SUBSTICK_RANGE 59
#define GC_STICK_DEAD     15

// libogc's PAD_Clamp squeezes the vector into Nintendo's octagon (cardinal
// 72 but only 40 per axis at the diagonal), so a full diagonal came out at
// 78% magnitude — under the game's run threshold. Full speed existed only
// on the four cardinals. Radial deadzone + radial rescale instead: the
// direction is preserved exactly and every heading can reach the full 128.
static inline void
gcStickRadial(int x, int y, int range, int16 *outX, int16 *outY)
{
	float mag = sqrtf((float)(x*x + y*y));
	if(mag <= (float)GC_STICK_DEAD){
		*outX = 0;
		*outY = 0;
		return;
	}
	float scale = (mag - GC_STICK_DEAD) * 128.0f /
	    ((range - GC_STICK_DEAD) * mag);
	float ox = x * scale, oy = y * scale;
	float m2 = ox*ox + oy*oy;
	if(m2 > 127.0f*127.0f){
		float s = 127.0f / sqrtf(m2);
		ox *= s;
		oy *= s;
	}
	*outX = (int16)ox;
	*outY = (int16)oy;
}

void
CapturePad(RwInt32 padID)
{
	CPad *pad = CPad::GetPad(padID);
	CControllerState &state = pad->PCTempJoyState;
	state.Clear();

	if(padID < 0 || padID >= PAD_CHANMAX)
		return;

	PAD_ScanPads();
	u16 buttons = PAD_ButtonsHeld(padID);
	// No PAD_Clamp: its octagon correction is what capped the diagonals
	// (see gcStickRadial). Raw stick, radial deadzone, radial rescale.
	int16 lx, ly, rx, ry;
	gcStickRadial(PAD_StickX(padID), PAD_StickY(padID),
	    GC_STICK_RANGE, &lx, &ly);
	gcStickRadial(PAD_SubStickX(padID), PAD_SubStickY(padID),
	    GC_SUBSTICK_RANGE, &rx, &ry);
	state.LeftStickX  =  lx;
	state.LeftStickY  = -ly;
	state.RightStickX =  rx;
	state.RightStickY = -ry;
	state.LeftShoulder2 = PAD_TriggerL(padID);
	state.RightShoulder2 = PAD_TriggerR(padID);
	state.LeftShoulder1 = (buttons & PAD_TRIGGER_L) ? 255 : 0;
	state.RightShoulder1 = (buttons & PAD_TRIGGER_R) ? 255 : 0;
	state.DPadUp = (buttons & PAD_BUTTON_UP) ? 255 : 0;
	state.DPadDown = (buttons & PAD_BUTTON_DOWN) ? 255 : 0;
	// main.scm polls RightShock (pad button 19, 19 call sites) to toggle
	// taxi/vigilante submissions; nothing else reads RightShock under
	// GTA_OGC, and DPad-down is otherwise idle in a vehicle — this is the
	// "Direcional: Missão" the settings card already promises.
	state.RightShock = state.DPadDown;
	state.DPadLeft = (buttons & PAD_BUTTON_LEFT) ? 255 : 0;
	state.DPadRight = (buttons & PAD_BUTTON_RIGHT) ? 255 : 0;
	state.Start = (buttons & PAD_BUTTON_START) ? 255 : 0;
	state.Select = (buttons & PAD_TRIGGER_Z) ? 255 : 0;
	state.Cross = (buttons & PAD_BUTTON_A) ? 255 : 0;
	state.Circle = (buttons & PAD_BUTTON_B) ? 255 : 0;
	state.Square = (buttons & PAD_BUTTON_X) ? 255 : 0;
	state.Triangle = (buttons & PAD_BUTTON_Y) ? 255 : 0;
}

static void
showPortCredit(void)
{
	static wchar line1[24], line2[24], line3[32];
	AsciiToUnicode("a port by", line1);
	AsciiToUnicode("ERASMO BELLUMAT", line2);
	AsciiToUnicode("for Nintendo GameCube", line3);

	// Five seconds at 60 Hz: 0.5s in, 4s held, 0.5s out. A button starts a
	// short fade instead of cutting to the next frame.
	int skipFrames = 0, skipAlpha = 0;
	for(int i = 0; i < 300; i++){
		int alpha = i < 30 ? i*255/29 :
		    i >= 270 ? (299-i)*255/29 : 255;
		PAD_ScanPads();
		bool skip = false;
		for(int pad = 0; pad < PAD_CHANMAX; pad++)
			skip |= PAD_ButtonsDown(pad) != 0;
		if(skip && skipFrames == 0){
			skipFrames = 15;
			skipAlpha = alpha;
		}
		if(skipFrames)
			alpha = skipAlpha*(skipFrames-1)/14;

		if(DoRWStuffStartOfFrame(0, 0, 0, 0, 0, 0, 255)){
			CSprite2d::SetRecipNearClip();
			CSprite2d::InitPerFrame();
			CFont::InitPerFrame();
			DefinedState();
			CSprite2d::DrawRect(CRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT),
			    CRGBA(0, 0, 0, 255));

			CFont::SetBackgroundOff();
			CFont::SetJustifyOff();
			CFont::SetRightJustifyOff();
			CFont::SetCentreOn();
			CFont::SetCentreSize(SCREEN_WIDTH);
			CFont::SetPropOn();
			CFont::SetDropShadowPosition(0);
			CFont::SetAlphaFade(255.0f);

			// font2 is Rage Italic; FONT_HEADING selects Pricedown in font1.
			CFont::SetFontStyle(FONT_BANK);
			CFont::SetScale(SCREEN_SCALE_X(1.0f), SCREEN_SCALE_Y(1.45f));
			CFont::SetColor(CRGBA(207, 134, 204, alpha));
			CFont::PrintString(SCREEN_WIDTH * 0.5f, SCREEN_SCALE_Y(188.0f), line1);

			CFont::SetFontStyle(FONT_HEADING);
			CFont::SetScale(SCREEN_SCALE_X(1.15f), SCREEN_SCALE_Y(1.65f));
			CFont::SetColor(CRGBA(60, 144, 248, alpha));
			CFont::PrintString(SCREEN_WIDTH * 0.5f, SCREEN_SCALE_Y(215.0f), line2);

			CFont::SetFontStyle(FONT_BANK);
			CFont::SetScale(SCREEN_SCALE_X(1.0f), SCREEN_SCALE_Y(1.45f));
			CFont::SetColor(CRGBA(207, 134, 204, alpha));
			CFont::PrintString(SCREEN_WIDTH * 0.5f, SCREEN_SCALE_Y(242.0f), line3);
			CFont::DrawFonts();
			DoRWStuffEndOfFrame();
		}
		VIDEO_WaitVSync();
		if(skipFrames && --skipFrames == 0)
			break;
	}
	// Restore CFont::Initialise's defaults so this one-off card cannot leak
	// layout state into the splash or frontend.
	CFont::SetScale(1.0f, 1.0f);
	CFont::SetSlantRefPoint(SCREEN_WIDTH, 0.0f);
	CFont::SetSlant(0.0f);
	CFont::SetColor(CRGBA(255, 255, 255, 0));
	CFont::SetJustifyOff();
	CFont::SetCentreOff();
	CFont::SetWrapx(SCREEN_WIDTH);
	CFont::SetCentreSize(SCREEN_WIDTH);
	CFont::SetBackgroundOff();
	CFont::SetBackGroundOnlyTextOff();
	CFont::SetPropOn();
	CFont::SetFontStyle(FONT_BANK);
	CFont::SetRightJustifyWrap(0.0f);
	CFont::SetAlphaFade(255.0f);
	CFont::SetDropShadowPosition(0);
}

static bool autoCarTestEnabled, autoCarProbed;
bool gcAutoSkipCutscenes;   // autocar.txt runs are hands-free: CutsceneMgr ends each scene after 1 s
static CVehicle *autoCarTestVehicle;
static void
autoCarTestTick(void)
{
	if(!autoCarProbed){
		autoCarProbed = true;
		FILE *f = fopen("dvd:/autocar.txt", "r");
		if(f){ fclose(f); autoCarTestEnabled = gcAutoSkipCutscenes = true; printf("autocar.txt: traffic-AI drive enabled, cutscenes skipped\n"); }
	}
	if(!autoCarTestEnabled) return;
	uint32 now = CTimer::GetTimeInMillisecondsPauseMode();
	// Every 2 min: pause menu → Gamepad Settings for 3 s → back to the game,
	// the page whose native pad load was where b179b caught the heap
	// corruption. The page logs 'FRONTEND3D: ...'.
	static uint32 padAt; static int padStep;
	if(padAt == 0) padAt = now;
	if(padStep == 0 && now - padAt > 120000 && !FrontEndMenuManager.m_bMenuActive &&
	   !CCutsceneMgr::IsRunning() && !CCutsceneMgr::IsCutsceneProcessing()){
		FrontEndMenuManager.RequestFrontEndStartUp(); padStep = 1; padAt = now;
	}else if(padStep == 1 && now - padAt > 1500 && FrontEndMenuManager.m_bMenuActive){
		printf("PADTEST open Gamepad Settings\n");
		FrontEndMenuManager.SwitchToNewScreen(MENUPAGE_CONTROLLER_SETTINGS); padStep = 2; padAt = now;
	}else if(padStep == 2 && now - padAt > 3000){
		FrontEndMenuManager.RequestFrontEndShutDown(); padStep = 0; padAt = now;
	}
	if(padStep) return;
	if(now < 45000 || CCutsceneMgr::IsCutsceneProcessing() || CCutsceneMgr::IsRunning() ||
	   TheCamera.m_WideScreenOn || CPad::GetPad(0)->ArePlayerControlsDisabled()){
		autoCarTestVehicle = nil;
		return;
	}
	CPlayerPed *player = FindPlayerPed();
	if(player == nil) return;
	if(player->m_pWanted && player->m_pWanted->GetWantedLevel() != 0)
		player->SetWantedLevel(0);
	if(player->m_fHealth <= 0.0f){
		autoCarTestVehicle = nil;
		return;
	}
	CVehicle *car = player->bInVehicle ? player->m_pMyVehicle : nil;
	if(car == nil){
		float best = 150.0f*150.0f;
		for(int i = 0; i < CPools::GetVehiclePool()->GetSize(); i++){
			CVehicle *candidate = CPools::GetVehiclePool()->GetSlot(i);
			if(candidate == nil || !candidate->IsCar() || candidate->IsBike() ||
			   candidate->VehicleCreatedBy != RANDOM_VEHICLE ||
			   candidate->pDriver == nil || candidate->m_fHealth <= 250.0f)
				continue;
			bool seat = false;
			for(int s = 0; s < candidate->m_nNumMaxPassengers; s++)
				seat |= candidate->pPassengers[s] == nil;
			if(!seat) continue;
			float d = (candidate->GetPosition() - player->GetPosition()).MagnitudeSqr();
			if(d < best){ best = d; car = candidate; }
		}
		if(car == nil){
			static uint32 lastWait;
			if(now - lastWait >= 5000){
				lastWait = now;
				printf("AUTOCAR waiting x=%.1f y=%.1f vehicles=%d\n",
				    player->GetPosition().x, player->GetPosition().y,
				    CPools::GetVehiclePool()->GetNoOfUsedSpaces());
			}
			return;
		}
		player->SetObjective(OBJECTIVE_ENTER_CAR_AS_PASSENGER, car);
		player->WarpPedIntoCar(car);
	}
	if(car->m_fHealth <= 0.0f) return;
	if(car != autoCarTestVehicle){
		autoCarTestVehicle = car;
		CCarCtrl::JoinCarWithRoadSystem(car);
		car->AutoPilot.m_nCarMission = MISSION_CRUISE;
		car->AutoPilot.m_nTempAction = TEMPACT_NONE;
		car->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_STOP_FOR_CARS;
		car->AutoPilot.m_nAntiReverseTimer = CTimer::GetTimeInMilliseconds();
		if(car->AutoPilot.m_nCruiseSpeed == 0)
			car->AutoPilot.m_nCruiseSpeed = 12;
		car->AutoPilot.m_fMaxTrafficSpeed = car->AutoPilot.m_nCruiseSpeed;
		car->bEngineOn = true;
		car->SetStatus(STATUS_PHYSICS);
	}
	static uint32 lastLog;
	if(now - lastLog > 5000){
		lastLog = now;
		printf("AUTOCAR t=%u x=%.1f y=%.1f speed=%.2f mission=%d temp=%d status=%d wanted=%d\n",
		    (unsigned)now, car->GetPosition().x, car->GetPosition().y,
		    car->m_vecMoveSpeed.Magnitude(), (int)car->AutoPilot.m_nCarMission,
		    (int)car->AutoPilot.m_nTempAction, (int)car->GetStatus(),
		    player->m_pWanted ? (int)player->m_pWanted->GetWantedLevel() : -1);
	}
}

int
main(int, char *[])
{
	gMainLwp = LWP_GetSelf();
	// Mirror stdout to OSReport so boot output is readable in an emulator log
	// (and over USB Gecko on hardware), not just on the framebuffer console.
	SYS_STDIO_Report(TRUE);
	gcInstallPanicHandler();


	psInitConsole();
	PAD_Init();
	gcBigPrime();   // B127: three 2MB streaming chunks before anything else touches the heap


	if(!psInstallFileSystem()){
		gcFatalPark("FILESYSTEM", "SD/USB/DVD mount failed\n");
	}


	if(RsEventHandler(rsINITIALIZE, nil) == rsEVENTERROR){
		gcFatalPark("INITIALIZE", "rsINITIALIZE failed\n");
	}
	// B136: no re-prime. b135 starved at the first LoadLevel malloc: once the
	// arena had grown, the GS_INIT_ONCE re-prime got real 2MB chunks and the
	// boot (B109) had nowhere to live. The boot keeps B110's rule: empty
	// chunks other than chunk 0 go back while gBigKeep == 0.

	ControlsManager.MakeControllerActionsBlank();
	ControlsManager.InitDefaultControlConfiguration();

	// Load the saved options. Every other skeleton does this (glfw.cpp:530,
	// win.cpp) and this one never did: settings were written on every menu
	// change and silently discarded at the next boot, so the goal - "the
	// options are saved and the game loads them when it opens" - only ever
	// had its write half. The file is gta_vc.set, which is the OPTIONS file
	// and is separate from the story slots (GTAVCsf*.b).
	FrontEndMenuManager.LoadSettings();
	{
		FILE *f = fopen("dvd:/benchmark.txt", "r");
		if(f){
			int fps = 0;
			if(fscanf(f, "%d", &fps) == 1 && (fps == 30 || fps == 60)){
				FrontEndMenuManager.m_PrefsFrameLimiter = fps == 30 ?
				    CMenuManager::FRAMELIMIT_30 : CMenuManager::FRAMELIMIT_60;
				printf("BENCHMARK cap=%d\n", fps);
			}
			fclose(f);
		}
	}

	// ponytail: RsRwInitialize only reads this as displayID; the console has none.
	if(RsEventHandler(rsRWINITIALIZE, nil) == rsEVENTERROR){
		gcFatalPark("RENDERWARE", "rsRWINITIALIZE failed\n");
	}
	// psInitConsole's console_init re-attached stdout to the boot XFB, and the
	// FMVs present into that same buffer — every printf during a movie drew
	// console glyphs over the picture. Re-route to OSReport now that RW owns
	// the screen; the log and USB Gecko get the text instead of the frames.
	SYS_STDIO_Report(TRUE);

	{
		RwRect r;
		r.x = 0;
		r.y = 0;
		r.w = RsGlobal.maximumWidth;
		r.h = RsGlobal.maximumHeight;
		RsEventHandler(rsCAMERASIZE, &r);
	}

	CPad::GetPad(0)->Clear(true);
	CPad::GetPad(1)->Clear(true);


	{ FILE *hc = fopen("dvd:/heapcheck.txt", "r"); if(hc){ fclose(hc); gHeapCheckOn = 1; printf("heapcheck.txt: heap walk every frame and conversion\n"); } }   // DIAG B179
	while(SYS_MainLoop() && !RsGlobal.quit){
		u64 frameStart = gettime();
		gMainTick++;
		gcHeapCheck(0x4652414D, gMainTick);   // DIAG B179 'FRAM'
		{
			static u64 tPrev;
			u64 t = gettime();
			if(tPrev){
				u32 us = ticks_to_microsecs(t - tPrev);
				gFtSum += us;
				if(us > gFtMax) gFtMax = us;
				gFtN++;
				gFpsSum += us;
				gFpsN++;
				if(gFpsSum >= 500000){
					gMeasuredFps = 1000000.0f * gFpsN / gFpsSum;
					gFpsSum = 0;
					gFpsN = 0;
				}
			}
			tPrev = t;
		}
		// Named, because it sits between the frame's last marker and "loop":
		// a stall in here used to report as "endofframe" and send the search
		// into the present path.
		HandleExit();


		// The GC path always waited for the retrace inside gx::showRaster;
		// m_PrefsVsync and m_PrefsFrameLimiter were read only by the
		// glfw/win/sdl2 skels and never by this one.
		//
		// Deliberately NOT following those skels in also forcing the wait
		// while a menu is up. Doing that makes the retrace wait switch on at
		// the exact moment the menu opens, and the menu opening is when this
		// port freezes — so the first build that wired it that way could not
		// tell a menu bug from a vsync-transition bug. One variable at a time:
		// the preference alone decides, and it does not change under us.
		// Options bridge into the GX backend, copied every frame so the menu
		// toggles are live: rim light from the neo switch, the AA level into
		// the copy-filter. librw stays ignorant of the menu manager.
		{
#ifdef EXTENDED_PIPELINES
			rw::gx::gxRimEnable = CustomPipes::RimlightEnable;
			rw::gx::gxGlossEnable = CustomPipes::GlossEnable;
			rw::gx::gxGlossMult = CustomPipes::GlossMult;
			rw::gx::gxLightmapEnable = CustomPipes::LightmapEnable;
			rw::gx::gxLightmapBlend =
			    CustomPipes::WorldLightmapBlend.Get()*CustomPipes::LightmapMult;
#endif
#ifdef MULTISAMPLING
			rw::gx::gxCopyFilterLevel = FrontEndMenuManager.m_nPrefsMSAALevel > 0;
#endif
			{
				// The osc probe's card dump holds DVD_FS_GUARD; unguarded
				// 5s card writes are the documented main-thread killer and
				// they starve the vorbis decode thread the same way. Only
				// measurement sessions (dvd:/autolog.txt) may write.
			}
		}
		extern unsigned gxWaitRetrace;
		// m_PrefsVsyncDisp, not m_PrefsVsync: the menu toggles Disp, and the
		// Disp->real copy only runs when starting a new game (and only under
		// LEGACY_MENU_OPTIONS at that). Reading the real one meant the Frame
		// Sync option did nothing until the next New Game.
		gxWaitRetrace = FrontEndMenuManager.m_PrefsVsyncDisp;

		switch(gGameState){
		case GS_START_UP:
			gGameState = GS_INIT_ONCE;
			break;

		case GS_INIT_ONCE: {
			// Port credit first, then both movies, then normal game init and
			// the existing splash. One AESND lifetime across both movies:
			// resetting the DSP between consecutive streams left the second
			// voice in a broken high-pitch state. Reset once before the game's
			// audio backend starts.
			showPortCredit();
			{
				bool titlesPresent, openingPresent;
				{
					DVD_FS_GUARD;
					FILE *titles = fopen("dvd:/movies/titles.ogv", "rb");
					titlesPresent = titles != nil;
					if(titles)
						fclose(titles);
					FILE *opening = fopen("dvd:/movies/opening.ogv", "rb");
					openingPresent = opening != nil;
					if(opening)
						fclose(opening);
				}
				if(titlesPresent){
					AESND_Init();
					AESND_Pause(false);
					// titles.ogv is the Rockstar logo reel; opening.ogv follows
					// with the Vice City montage when present.
					bool titlesPlayed = PlayGameCubeMovie("dvd:/movies/titles.ogv",
					    framebuffer, videoMode->fbWidth, videoMode->xfbHeight,
					    VIDEO_GetFrameBufferSize(videoMode));
					bool openingPlayed = !openingPresent || (titlesPlayed &&
					    PlayGameCubeMovie("dvd:/movies/opening.ogv", framebuffer,
					        videoMode->fbWidth, videoMode->xfbHeight,
					        VIDEO_GetFrameBufferSize(videoMode)));
					AESND_Pause(true);
					AESND_Reset();
					if(!titlesPlayed)
						gcFatalPark("FMV", "titles.ogv read/decode failed; check OSReport\n");
					if(!openingPlayed)
						gcFatalPark("FMV", "opening.ogv read/decode failed; check OSReport\n");
				}else
					printf("FMV: image has no movies; continuing to splash\n");
			}
			printf("GS_INIT_ONCE: CGame::InitialiseOnceAfterRW\n");
			// B136: re-prime removed (see main)
			LoadingScreen(nil, nil, "loadsc0");
			if(!CGame::InitialiseOnceAfterRW()){
				gcFatalPark("GAME-INIT", "InitialiseOnceAfterRW failed\n");
			}
			gGameState = GS_INIT_FRONTEND;
			break;
		}

		case GS_INIT_FRONTEND:
			LoadingScreen(nil, nil, "loadsc0");
			FrontEndMenuManager.m_bGameNotLoaded = true;
			FrontEndMenuManager.m_bStartUpFrontEndRequested = true;
			// CMenuManager::Process refuses to do anything while the camera
			// reports a fade in progress, and on a cold boot nothing can ever
			// end that fade: DoFade only reaches ProcessFade inside
			// if(StillToFadeOut), which only a save load sets, and it returns
			// outright while the timer is paused. A fade left standing by the
			// loading screen is therefore permanent, and the menu never opens.
			// Hand the frontend a clean fade instead of trusting the state it
			// inherits. Fade() with a zero timeout resolves fully in the one
			// ProcessFade below, so this is exact, not a nudge.
			TheCamera.SetFadeColour(0, 0, 0);
			TheCamera.Fade(0.0f, FADE_IN);
			TheCamera.ProcessFade();
			gGameState = GS_FRONTEND;
			break;

		case GS_FRONTEND:
			RsEventHandler(rsFRONTENDIDLE, nil);
			// m_bMenuActive is still false on the first iteration - Process()
			// has to run once to raise it. Leaving on "not active" alone means
			// any frame where Process() bails early (see the fade above) drops
			// the frontend entirely and boots into the game with the menu
			// never opened, which is exactly what the hang dump showed:
			// state=9 GS_PLAYING_GAME, menu=0, running under an opaque splash.
			// The pending request is the signal that the menu is still owed a
			// chance; SwitchMenuOnAndOff clears it once it has acted.
			if((!FrontEndMenuManager.m_bMenuActive && !FrontEndMenuManager.m_bStartUpFrontEndRequested)
			    || FrontEndMenuManager.m_bWantToLoad)
				gGameState = GS_INIT_PLAYING_GAME;
			break;

		case GS_INIT_PLAYING_GAME:
			{ extern bool gIntroHold; gIntroHold = true; }   // B99
			// B113 (user): silent from the first pixel of the loading bar. The
			// frontend track was still playing into the load, and
			// CGame::Initialise ends by restoring both fades to 127.
			DMAudio.SetEffectsFadeVol(0);
			DMAudio.SetMusicFadeVol(0);
			printf("GS_INIT_PLAYING_GAME\n");
			InitialiseGame();
			DMAudio.SetEffectsFadeVol(0);
			DMAudio.SetMusicFadeVol(0);
			FrontEndMenuManager.m_bGameNotLoaded = false;
			gGameState = GS_PLAYING_GAME;
			break;

		case GS_PLAYING_GAME:
			gcBootLevelLoaded();   // B151: normally already done at the end of LoadLevel; harmless twice
			gMainWhere = "idle";
			RsEventHandler(rsIDLE, (void *)TRUE);
			autoCarTestTick();
			gMainWhere = "post-idle";
			{
				// One heap line every ten seconds: the log's memory instrument.
				static u32 censusTick;
				censusTick++;
				if(censusTick % 300 == 0){
					u32 frames = gFtN;   // gcHeapLine resets the frame counters
					char heap[560];
					gcHeapLine(heap, sizeof(heap));
					printf("CENSUS %s\n", heap);
					{ static bool sqrtChecked; if(!sqrtChecked){ sqrtChecked = true; gcSqrtCheck(); } }
					fsDiscStatsPrint();
					if(censusTick % 1800 == 0) gcHeapCensusDump("periodic");   // DIAG b176: top live sites once a minute
					printf("STREAM compact %u moves %uK\n", gcMemoryMoves, gcMemoryMovedBytes/1024);
					{ char prof[420]; gcProfLine(prof, sizeof(prof), frames); printf("PROF %u frames avg/max ms: %s\n", (unsigned)frames, prof); }   // B155
					if(censusTick % 1800 == 0) gcHeapCensusDump("play");   // B155: every minute
					// The tier's boot-time lines go to the screen console; say once here.
					static bool saidTier;
					if(!saidTier){
						saidTier = true;
						extern unsigned gxAramBytes;
						printf("ARAM tier: %s\n", gxAramBytes ? "armed" : "NOT armed");
					}
				}
				// B155: what the mixer plays under a cutscene (user: city noise under Marco's Bistro).
				if(censusTick % 150 == 0 && CCutsceneMgr::IsRunning()){
					char v[360];
					gcVoiceCensusLine(v, sizeof(v));
					printf("CUTSFX t=%u%s\n", (unsigned)CTimer::GetTimeInMilliseconds(), v);
				}
			}
			// Service the restart request. Idle() returns before ANY rendering
			// while one is pending (main.cpp, right after DMAudio.Service) and
			// expects the platform's game loop to act on it - win.cpp,
			// glfw.cpp and sdl2.cpp all do, and this skeleton never did.
			//
			// DoSettingsBeforeStartingAGame raises m_bWantToRestart the moment
			// you choose Start New Game, so from that frame on the loop ran
			// audio, script and cutscenes but presented nothing: the screen
			// kept whatever was last drawn - the loading splash - while the
			// game played underneath it, which is exactly how this was
			// reported. dvd:/autostart.txt hid it completely by jumping
			// straight to GS_INIT_PLAYING_GAME, so the frontend never ran and
			// the flag was never raised. That is why it reproduced on other
			// people's cards and never on the one card that had the file.
			if(FrontEndMenuManager.m_bWantToRestart || b_FoundRecentSavedGameWantToLoad){
				if(b_FoundRecentSavedGameWantToLoad){
					FrontEndMenuManager.m_bWantToRestart = true;
					FrontEndMenuManager.m_bWantToLoad = true;
				}
				printf("restart requested: reinitialising game\n");
				gcHeapCensusDump("restart");   // B155: the fixed set as the frontend leaves it
				// B102: silent and black from here. The menu path muted in
				// DoSettingsBeforeStartingAGame; autostart and Load Game did
				// not, and the 12 s hotel LOAD_SCENE inside the reinit runs
				// before Idle's hold can act.
				{ extern bool gIntroHold; gIntroHold = true; }
				DMAudio.SetEffectsFadeVol(0);
				DMAudio.SetMusicFadeVol(0);
				CPad::ResetCheats();
				CPad::StopPadsShaking();
				DMAudio.ChangeMusicMode(MUSICMODE_DISABLE);
				CGame::ShutDownForRestart();
				CTimer::Stop();
				if(!CGame::InitialiseWhenRestarting()){
					gcFatalPark("GAME-RESTART", "InitialiseWhenRestarting failed\n");
				}
				DMAudio.ChangeMusicMode(MUSICMODE_GAME);
				FrontEndMenuManager.m_bWantToRestart = false;
				b_FoundRecentSavedGameWantToLoad = false;
			}
			break;
		}

		// Frame limiter. With the retrace wait off nothing else paces the
		// loop, so without this it becomes a busy spin that starves the DVD
		// and audio threads. Absolute deadlines rather than sleep(period), so
		// a frame that runs long is absorbed instead of pushing every
		// subsequent frame late.
		// No "&& !gxWaitRetrace" any more. That gate meant Frame Sync ON — the
		// default — skipped the limiter entirely, so OFF/30/60 all behaved
		// identically and the option looked dead. The two are not exclusive:
		// the retrace wait quantises to a whole field, and the limiter then
		// holds the rest of the deadline. With sync on and 30 selected, that
		// is what actually produces a steady 30 instead of a 60/30 swing.
		if(FrontEndMenuManager.m_PrefsFrameLimiter){
			static u64 tNext;
			// The Options entry is OFF / 60 / 30 now, so the cap comes from
			// the preference rather than from RsGlobal alone. 30 is the useful
			// one on this console: a scene that cannot hold 60 reads far
			// steadier locked at 30 than oscillating between the two.
			int fps = FrontEndMenuManager.m_PrefsFrameLimiter ==
			    CMenuManager::FRAMELIMIT_30 ? 30 : 60;
			u64 period = millisecs_to_ticks(1000)/fps;
			u64 now = gettime();
			if(tNext > now)
				usleep(ticks_to_microsecs(tNext - now));
			tNext = (tNext > now ? tNext : now) + period;
		}
		gcFrameSample(frameStart);
	}

	if(gGameState == GS_PLAYING_GAME)
		CGame::ShutDown();
	CTimer::Stop();

	RsEventHandler(rsRWTERMINATE, nil);
	RsEventHandler(rsTERMINATE, nil);
	psTerminate();
	psHalt();
	return 0;
}
