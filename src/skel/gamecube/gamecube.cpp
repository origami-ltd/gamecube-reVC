#include "common.h"
#include "crossplatform.h"
#include "platform.h"
#include "Pad.h"
#include "skeleton.h"
#include "main.h"
#include "Game.h"
#include "Timer.h"
#include "Frontend.h"
#include "ControllerConfig.h"
#include "FileMgr.h"
#include "CdStream.h"
#ifdef EXTENDED_PIPELINES
#include "custompipes.h"
#endif

#include <gccore.h>
#include <ogc/machine/processor.h>
#include <ogc/usbgecko.h>
#include <ogc/dvd.h>
#ifndef HW_RVL
extern "C" int GcCardMountDevice(void);
#endif
namespace rw { namespace gx { int8_t gxReadEfbPref(void); } }
#include <iso9660.h>
extern "C" bool ISO9660_MountDbg(const char *name, const DISC_INTERFACE *disc_interface);
#include <fat.h>
#ifdef HW_RVL
#include <sdcard/wiisd_io.h>
#else
#include <sdcard/gcsd.h>
#endif
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
} }

long _dwOperatingSystemVersion = OS_WINXP;
size_t _dwMemAvailPhys;
RwUInt32 gGameState;

// Freeze watchdog. Every hang in this port so far has presented identically —
// frozen image, no exception, no crash.log, and near-zero CPU because the main
// thread is blocked rather than spinning — which makes them indistinguishable
// from each other and costs a boot per guess. A separate thread that only
// watches a counter can still run when the main thread cannot, so it is the
// one place that can say where the game stopped.
void GeckoLog(const char *msg);   // defined below
volatile uint32 gFrameTick;
const char *gPhase = "boot";
static lwp_t watchdogThread = LWP_THREAD_NULL;
static volatile bool watchdogStop;

// Boot-smash bracketing. current_mallinfo.arena IS newlib's sbrked_mem
// accumulator; the corruptor blasts it to ~300MB within the first ~50 frames.
// Called from every LoadingScreen step (and anywhere else worth a marker),
// the first caller that sees poison names the init step that did the deed.
extern "C" { extern struct mallinfo __malloc_current_mallinfo; }
void
gcHeapGuardCheck(const char *marker)
{
	static bool hit;
	if(hit)
		return;
	// arena alone trips LATE: it only goes insane on the next sbrk AFTER the
	// top-chunk header is smashed. mallinfo() walks every free chunk and the
	// top, so a smashed header shows up at the first marker after the write.
	struct mallinfo gmi = mallinfo();
	// Memory map: one line per marker where used moved >256K since the last
	// line. One boot = the whole boot's consumption curve, the number the
	// budget graveyard in Streaming.cpp never had.
	{
		static uint32 lastUsed;
		uint32 used = (uint32)gmi.uordblks;
		uint32 delta = used > lastUsed ? used - lastUsed : lastUsed - used;
		// audio-* markers always log: they're the requested MEM1 audit and
		// their deltas sit under the 256K noise gate.
		if(delta > 256u*1024 ||
		   (marker && marker[0]=='a' && marker[1]=='u' && marker[2]=='d')){
			lastUsed = used;
			if(CdStreamFsTryLock()){
				FILE *mf = fopen("dvd:/memmap.log", "a");
				if(mf){
					fprintf(mf, "MEM %s used=%uK free=%uK\n",
					    marker ? marker : "-", used/1024,
					    (unsigned)((uint32)gmi.fordblks/1024));
					fclose(mf);
				}
				CdStreamFsUnlock();
			}
		}
	}
	if((uint32)gmi.arena <= 20u*1024*1024 &&
	   (uint32)gmi.fordblks <= 20u*1024*1024 &&
	   (uint32)gmi.uordblks <= 20u*1024*1024 &&
	   (uint32)gmi.keepcost <= 20u*1024*1024)
		return;
	hit = true;
	char l[128];
	snprintf(l, sizeof(l), "SMASH-AT %s arena=%uK ford=%uK uord=%uK keep=%uK",
	    marker ? marker : "-",
	    (unsigned)((uint32)gmi.arena/1024), (unsigned)((uint32)gmi.fordblks/1024),
	    (unsigned)((uint32)gmi.uordblks/1024), (unsigned)((uint32)gmi.keepcost/1024));
	GeckoLog(l);
	if(CdStreamFsTryLock()){
		FILE *f = fopen("dvd:/crash.log", "a");
		if(f){ fprintf(f, "%s\n", l); fclose(f); }
		CdStreamFsUnlock();
	}
}

// ROOT CAUSE of the boot heap smash: libogc's _sbrk_r silently FALLS BACK TO
// MEM2 when Arena1 runs out — the brk jumps ~300MB (the impossible
// mallinfo.arena), newlib's top spans the unmapped 0x8180-0x8FFF gap, and
// MEM2's Arena2Lo is the same bump pointer the audio bank owns, so the two
// overwrite each other. This override keeps malloc in MEM1 and makes
// exhaustion an honest nil instead of a corrupted heap.
extern "C" void*
_sbrk_r(struct _reent *r, ptrdiff_t incr)
{
	u32 level;
	_CPU_ISR_Disable(level);
	u8 *lo = (u8*)SYS_GetArena1Lo();
	u8 *hi = (u8*)SYS_GetArena1Hi();
	if(lo + incr <= hi && lo + incr >= (u8*)0x80003100){
		SYS_SetArena1Lo(lo + incr);
		_CPU_ISR_Restore(level);
		return lo;
	}
	// NO MEM2 fallback: tried on 08-20 — newlib's dlmalloc treats the
	// discontiguous brk as one span, mallinfo read 3.9GB free and the carve
	// from the gap-top aborted the game mid-cutscene. MEM2 relief is done
	// with a dedicated lwp_heap for texture buffers (gxraster) instead.
	_CPU_ISR_Restore(level);
	r->_errno = ENOMEM;
	return (void*)-1;
}

void *gcLifeboat;
void
gcNewHandler(void)
{
	if(gcLifeboat){
		free(gcLifeboat);
		gcLifeboat = nil;
		GeckoLog("LIFEBOAT released");
		return;   // operator new retries
	}
	std::set_new_handler(nil);   // second failure: fail loud as before
}

static void*
watchdogMain(void*)
{
	uint32 last = 0;
	int stuck = 0;
	bool reported = false;
	while(!watchdogStop){
		usleep(1000*1000);
		// Re-arm the spawn lifeboat: released boats sink into the dust of
		// small allocations within seconds; a re-armed one means the next
		// OOM burst is also survivable. Quiet failure — tries again next
		// second.
		if(gcLifeboat == nil){
			gcLifeboat = malloc(2560*1024);
			if(gcLifeboat){
				extern void gcNewHandler(void);
				std::set_new_handler(gcNewHandler);
				GeckoLog("LIFEBOAT re-armed");
			}
		}
		// Heap-smash tripwire. mallinfo walks every chunk under the malloc
		// lock; a corrupted chunk header makes its sums impossible (measured:
		// "used 308266K" in a 16MB arena, twice, two builds). First trip
		// stamps the second and the phase — the corruptor's time of death.
		{
			static bool tripped;
			struct mallinfo wmi = mallinfo();
			if(!tripped && ((uint32)wmi.uordblks > 20u*1024*1024 ||
			                (uint32)wmi.fordblks > 20u*1024*1024)){
				tripped = true;
				char hl[96];
				snprintf(hl, sizeof(hl),
				    "HEAPSMASH t=%u phase=%s used=%uK free=%uK",
				    (unsigned)gFrameTick, gPhase ? gPhase : "-",
				    (unsigned)((uint32)wmi.uordblks/1024),
				    (unsigned)((uint32)wmi.fordblks/1024));
				GeckoLog(hl);
				if(CdStreamFsTryLock()){
					FILE *hf = fopen("dvd:/crash.log", "a");
					if(hf){ fprintf(hf, "%s\n", hl); fclose(hf); }
					CdStreamFsUnlock();
				}
			}
		}
		if(gFrameTick != last){
			last = gFrameTick;
			stuck = 0;
			reported = false;
			continue;
		}
		// Only the states whose loop iteration IS a frame. GS_INIT_ONCE and
		// GS_INIT_PLAYING_GAME each do a whole game load inside a single
		// iteration, so a stalled counter there is the normal case, not a
		// hang — the first version of this watchdog cried wolf at tick=3 in
		// GS_INIT_PLAYING_GAME and reported nothing about the real freeze.
		if(gGameState != GS_PLAYING_GAME && gGameState != GS_FRONTEND){
			stuck = 0;
			continue;
		}
		// Re-report every eight seconds it stays stalled, rather than once and
		// then silence. One line cannot tell a genuinely stopped game from a
		// single slow step that recovered, and this project has spent whole
		// sessions on that ambiguity. A repeating line is a stopped game; a
		// lone one was a slow load.
		if(++stuck < 8)
			continue;
		stuck = 0;
		(void)reported;
		extern unsigned gxWaitRetrace, gxCamW, gxCamH;
		extern const char *gxLastPath;
		char line[256];
		snprintf(line, sizeof(line),
		    "HANG phase=%s tick=%u state=%u menu=%d vsync=%u "
		    "gxpath=%s cam=%ux%u",
		    gPhase ? gPhase : "-", (unsigned)gFrameTick,
		    (unsigned)gGameState, (int)FrontEndMenuManager.m_bMenuActive,
		    gxWaitRetrace, gxLastPath ? gxLastPath : "-",
		    gxCamW, gxCamH);
		// Gecko only, deliberately. The first version wrote this to dvd:/ and
		// got nothing: when the game is stuck the main thread is stuck holding
		// libfat, so the watchdog's own fopen blocks behind it and the one
		// report that matters never lands. A hang reporter cannot depend on
		// the subsystem the hang might be in. Gecko is EXI and independent, so
		// it still gets through — it just truncates past a couple of dozen
		// characters, hence one field per line rather than one long line.
		// ONE line, and a short one. Five separate GeckoLog calls lost all but
		// the first: Dolphin's emulated Gecko truncates past a couple of dozen
		// characters and drops what it cannot drain, so a multi-line report is
		// a report that does not arrive. Everything is abbreviated to fit:
		// H <phase-initial> s<state> m<menu> <gxpath> v<vsync> r<rdIdle>c<cmdIdle>
		char part[48];
		// Ask the GP directly, from a thread that is not blocked on it. The
		// bounded wait in showRaster turned out to be too late to catch this:
		// when the GP stalls, the FIFO fills, and the CPU parks in whichever
		// GX call runs next — GX_CopyDisp, well before GX_DrawDone. So the
		// draw-sync deadline never gets a chance to fire and no GPSTALL is
		// written even though the GP is the thing that stopped.
		// rd/cmd idle both 0 with the frame counter frozen IS a stalled GP.
		{
			u8 overhi = 0, underlow = 0, rdIdle = 0, cmdIdle = 0, brkpt = 0;
			GX_GetGPStatus(&overhi, &underlow, &rdIdle, &cmdIdle, &brkpt);
			// The phase name in full, on its own short line. Sending only its
			// first letter made every hang read as "endofframe", because that
			// was the last marker Idle set — the markers were opened and never
			// closed, so the report named the last phase ENTERED rather than
			// where the thread actually was. They close now, and the name is
			// worth the extra line.
			// GP status FIRST and short. Gecko drops what it cannot drain, so
			// the second line has been lost every time — and it was the line
			// carrying the one fact that decides the whole question: r1c1 is
			// an idle GP with the CPU stuck elsewhere, r0c0 is a stalled GP.
			// The phase FIRST, inside the first line. Splitting it onto a
			// second line was the whole reason it never arrived: Gecko drops
			// what it cannot drain, and the second line has been lost on
			// every single hang in this project — the last capture ends
			// literally at "HANG r1", mid-word. Whatever survives truncation
			// is now the part worth having.
			snprintf(part, sizeof(part), "HANG %s r%uc%u",
			    gPhase ? gPhase : "-", rdIdle, cmdIdle);
			GeckoLog(part);
			snprintf(part, sizeof(part), "s%u", (unsigned)gGameState);
			GeckoLog(part);
			snprintf(part, sizeof(part), "H s%u m%d %s v%u r%uc%u",
			    (unsigned)gGameState,
			    (int)FrontEndMenuManager.m_bMenuActive,
			    gxLastPath ? gxLastPath : "-", gxWaitRetrace,
			    rdIdle, cmdIdle);
			GeckoLog(part);

			// And to the SD, via TRY-lock. A blocking guard made the first
			// version silent (main thread wedged holding libfat = the report
			// never landed), but writing with NO lock was worse: the watchdog
			// false-fires on slow loads (frames stall while the worker is
			// mid-read inside libfat), and an unguarded fopen racing that is
			// heap corruption in a run that was NOT over. Try-lock keeps both
			// properties: fs wedged → busy → skip (gecko already has the
			// line, and hang.log-empty-means-fs-wedged stays a signal); GX/GP
			// hang with fs idle → lock free → the full report lands.
			snprintf(line, sizeof(line),
			    "HANG phase=%s tick=%u state=%u menu=%d vsync=%u gx=%s "
			    "cam=%ux%u gp rd=%u cmd=%u over=%u under=%u brk=%u",
			    gPhase ? gPhase : "-", (unsigned)gFrameTick,
			    (unsigned)gGameState, (int)FrontEndMenuManager.m_bMenuActive,
			    gxWaitRetrace, gxLastPath ? gxLastPath : "-", gxCamW, gxCamH,
			    rdIdle, cmdIdle, overhi, underlow, brkpt);
			if(CdStreamFsTryLock()){
				FILE *hf = fopen("dvd:/hang.log", "a");
				if(hf){
					fprintf(hf, "%s\n", line);
					fclose(hf);
				}
				CdStreamFsUnlock();
			}
		}
	}
	return nil;
}

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
 * the red screen stays until the emulator/console is reset, and it also saves
 * the dump into a memory cookie that the next boot appends to dvd:/crash.log
 * (readable from the host by mounting the SD image).
 */

// ponytail: fixed high-MEM1 scratch, same reserved region libogc uses for its
// panic framebuffer (0xC1700000); survives a DOL reload, not a cold boot.
struct CrashCookie {
	u32 magic;
	u32 length;
	char text[8184];
};
#define CRASH_COOKIE ((CrashCookie*)0xC17C0000)
#define CRASH_MAGIC  0x43525348 // 'CRSH'

extern "C" {
void VIDEO_SetFramebuffer(void *fb);
void __VIClearFramebuffer(void *fb, u32 size, u32 color);
void __console_init(void *fb, int xstart, int ystart, int xres, int yres, int stride);
}

// Live host-side log line over Dolphin's emulated USB Gecko (EXI slot B,
// TCP localhost:55020). Dolphin surfaces no libogc console output any other
// way. Cheap no-op when no gecko is attached.
void
GeckoLog(const char *msg)
{
	// "Cheap no-op when no gecko is attached" was wrong, and it cost this
	// project real time. 1000 retries is 1000 EXI transactions on the MAIN
	// thread, per line, whenever nobody is draining the other end — and the
	// reader drops all the time (Dolphin's listener takes one connection, so a
	// dead `nc` never comes back). Loading logs one line per animation block,
	// so a dropped reader turns a 40-second load into a twenty-minute one that
	// looks exactly like a freeze: no output, and the CPU at 5% because the
	// thread is parked in EXI rather than spinning.
	//
	// So: few retries, and give up on the transport entirely once it has
	// clearly stopped being read. Any successful line brings it back, which is
	// what makes a mid-run reconnect still work.
	static int deadStreak;
	if(deadStreak >= 8)
		return;
	// A SHORT write is not a failure: Dolphin partial-sends constantly, which
	// is why the log has always looked chewed. Only nothing-at-all counts.
	if(usb_sendbuffer_safe_ex(EXI_CHANNEL_1, msg, (int)strlen(msg), 16) <= 0){
		deadStreak++;
		return;
	}
	deadStreak = 0;
	usb_sendbuffer_safe_ex(EXI_CHANNEL_1, "\n", 1, 16);
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
	usb_sendbuffer_safe_ex(EXI_CHANNEL_1, line, strlen(line), 1000);

	CrashCookie *ck = CRASH_COOKIE;
	size_t n = strlen(line);
	if(ck->length + n < sizeof(ck->text)){
		memcpy(ck->text + ck->length, line, n);
		ck->length += n;
		ck->text[ck->length] = '\0';
	}
}

static void
gcPanic(unsigned exid, PPCContext *ctx)
{
	static volatile bool inPanic;
	if(inPanic)
		for(;;)
			;
	inPanic = true;

	// Stop the world FIRST. With interrupts live the decrementer keeps
	// scheduling other threads, and the next game frame flips the
	// framebuffer back — the red screen becomes a one-frame flash.
	u32 level;
	_CPU_ISR_Disable(level);
	(void)level;

	static const char *const names[] = {
		"Reset", "MachineCheck", "DSI", "ISI", "Interrupt", "Alignment",
		"Program", "FPU", "Decrementer", "Syscall", "Trace", "Performance",
		"IABR"
	};

	GX_AbortFrame();
	void *xfb = (void*)0xC1700000;
	VIDEO_SetFramebuffer(xfb);
	__VIClearFramebuffer(xfb, 640*480*VI_DISPLAY_PIX_SZ, COLOR_MAROON);
	__console_init(xfb, 48, 48, 640-96, 480-96, 2*640);

	CrashCookie *ck = CRASH_COOKIE;
	ck->magic = CRASH_MAGIC;
	ck->length = 0;
	ck->text[0] = '\0';

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
	// No SD write here: fat goes through IPC and sleeps the crashed context,
	// which hands the CPU back to the game — that's how the red screen was
	// getting painted over. The cookie flushes to crash.log on the next boot.

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
void
gcFatalPark(const char *tag, const char *msg)
{
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

	struct mallinfo mi = mallinfo();
	char heap[64];
	snprintf(heap, sizeof(heap), "heap used %uK free %uK",
	    (unsigned)mi.uordblks/1024, (unsigned)mi.fordblks/1024);

	// Stamp the build. crash.log lives on the SD card and is appended to
	// across every boot, so without this the entries from a dozen different
	// binaries are indistinguishable — and resolving an old stack against the
	// current ELF produces confident nonsense (lodepng frames inside
	// CCullZones::Update, in one real case). Match this string against the
	// build before trusting any address in the stack below it.
	// The streaming worker may be inside libfat right now — fail-loud parks
	// fire during normal play. A healthy worker releases the lock in ms, so
	// wait briefly; if it never comes, skip the file. The maroon screen and
	// gecko carry the report either way, and writing through contended
	// libfat is how heap corruption starts — a poor way to report one.
	bool fsLocked = false;
	for(int i = 0; i < 200 && !(fsLocked = CdStreamFsTryLock() != 0); i++)
		usleep(1000);
	if(fsLocked){
		FILE *f = fopen("dvd:/crash.log", "a");
		if(f){
			fprintf(f, "---- %s ---- build %s %s\n%s%s\nSTACK:%s\n",
			    tag, __DATE__, __TIME__, msg, heap, stack);
			fclose(f);
		}
		CdStreamFsUnlock();
	}

	u32 level;
	_CPU_ISR_Disable(level);
	(void)level;

	void *xfb = (void*)0xC1700000;
	VIDEO_SetFramebuffer(xfb);
	__VIClearFramebuffer(xfb, 640*480*VI_DISPLAY_PIX_SZ, COLOR_MAROON);
	__console_init(xfb, 48, 48, 640-96, 480-96, 2*640);
	printf("reVC %s\n%s%s\nSTACK:%s\n", tag, msg, heap, stack);
	{
		char line[512];
		snprintf(line, sizeof(line), "reVC %s\n%s%s\nSTACK:%s", tag, msg, heap, stack);
		GeckoLog(line);
	}
	// Dump delivered (crash.log + gecko + this screen). Power off so a
	// batch-mode (-b) Dolphin closes itself instead of parking forever —
	// user directive: the log and the exit, not the museum piece.
	_CPU_ISR_Restore(level);
	SYS_ResetSystem(SYS_POWEROFF, 0, 0);
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

// Called once the filesystem is up: persist any dump left by a crash.
static void
gcFlushCrashLog(void)
{
	CrashCookie *ck = CRASH_COOKIE;
	if(ck->magic != CRASH_MAGIC || ck->length == 0 ||
	   ck->length >= sizeof(ck->text))
		return;

	DVD_FS_GUARD;
	FILE *f = fopen("dvd:/crash.log", "a");
	if(f){
		fputs("---- crash ----\n", f);
		fwrite(ck->text, 1, ck->length, f);
		fclose(f);
		printf("crash.log: saved dump from previous run\n");
	}
	ck->magic = 0;
	ck->length = 0;
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
			ISO9660_Unmount("dvd");
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

RwMemoryFunctions *
psGetMemoryFunctions(void)
{
	return nil;
}

RwBool
psInstallFileSystem(void)
{
	if(!fileSystemReady){
#ifdef HW_RVL
		static const DISC_INTERFACE *const sdSlots[] = { &__io_wiisd };
		static const char *const sdNames[] = { "front SD" };
#else
		static const DISC_INTERFACE *const sdSlots[] = { &__io_gcsda, &__io_gcsdb };
		static const char *const sdNames[] = { "SD Gecko slot A", "SD Gecko slot B" };
#endif
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
		// ponytail: on real hardware with an EMPTY drive this probe blocks
		// forever (startup() and isInserted() both block); if that setup ever
		// matters, gate it on DVD_GetCoverStatus first.
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
		if(!fileSystemReady)
			return FALSE;
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
		ISO9660_Unmount("dvd");
	fileSystemReady = FALSE;
	return FALSE;
}

const char *
_psGetUserFilesFolder(void)
{
#ifndef HW_RVL
	// Userfiles (settings dump + story saves) live on the memory card, each
	// as its own CARD file — options and progress separated, and nothing
	// depends on the disc being writable.
	if(GcCardMountDevice()){
		static const char mc[] = "mc:";
		return mc;
	}
#endif
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
#define GC_SUBSTICK_RANGE 56

static inline int32
gcStickScale(int v, int range)
{
	int r = v * 128 / range;
	return r > 127 ? 127 : r < -127 ? -127 : r;
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
	// The game scales its stick axes against +-128 (see CPad, which multiplies
	// normalised input by 128), but a GameCube stick only reaches about +-72
	// at the physical gate and the C-stick about +-56 — PAD_StickX is
	// calibrated, not full-scale. Passing those straight through meant a fully
	// deflected stick asked for barely half speed and the player never ran at
	// maximum on analog alone. Scale to the range the game expects, clamp, and
	// keep the calibration knob visible: real pads vary and a worn stick reads
	// lower still.
	state.LeftStickX  =  gcStickScale(PAD_StickX(padID),    GC_STICK_RANGE);
	state.LeftStickY  = -gcStickScale(PAD_StickY(padID),    GC_STICK_RANGE);
	state.RightStickX =  gcStickScale(PAD_SubStickX(padID), GC_SUBSTICK_RANGE);
	state.RightStickY = -gcStickScale(PAD_SubStickY(padID), GC_SUBSTICK_RANGE);
	state.LeftShoulder2 = PAD_TriggerL(padID);
	state.RightShoulder2 = PAD_TriggerR(padID);
	state.LeftShoulder1 = (buttons & PAD_TRIGGER_L) ? 255 : 0;
	state.RightShoulder1 = (buttons & PAD_TRIGGER_R) ? 255 : 0;
	state.DPadUp = (buttons & PAD_BUTTON_UP) ? 255 : 0;
	state.DPadDown = (buttons & PAD_BUTTON_DOWN) ? 255 : 0;
	state.DPadLeft = (buttons & PAD_BUTTON_LEFT) ? 255 : 0;
	state.DPadRight = (buttons & PAD_BUTTON_RIGHT) ? 255 : 0;
	state.Start = (buttons & PAD_BUTTON_START) ? 255 : 0;
	state.Select = (buttons & PAD_TRIGGER_Z) ? 255 : 0;
	state.Cross = (buttons & PAD_BUTTON_A) ? 255 : 0;
	state.Circle = (buttons & PAD_BUTTON_B) ? 255 : 0;
	state.Square = (buttons & PAD_BUTTON_X) ? 255 : 0;
	state.Triangle = (buttons & PAD_BUTTON_Y) ? 255 : 0;
}

int
main(int, char *[])
{
	// Mirror stdout to OSReport so boot output is readable in an emulator log
	// (and over USB Gecko on hardware), not just on the framebuffer console.
	SYS_STDIO_Report(TRUE);
	gcInstallPanicHandler();

	// Spawn lifeboat: during the save-load refill nothing is evictable, so
	// the player-creation `new`s can meet a heap ground to dust. Hold 1.5MB
	// from boot; the new_handler releases it on the first failed `new` and
	// the allocation retries into the freed block. Second failure surfaces
	// as the normal fail-loud abort. std::set_new_handler is the stdlib's
	// own emergency-pool hook — no call-site knowledge needed.
	{
		extern void *gcLifeboat;
		extern void gcNewHandler(void);
		gcLifeboat = malloc(2560*1024);
		std::set_new_handler(gcNewHandler);
	}

	// ROOT CAUSE of the boot heap smash (08-20): newlib's free() runs
	// malloc_trim past a 128K top and trims via sbrk(-X) — but libogc's sbrk
	// is a bump pointer that was never meant to run backwards, and trim's
	// resync path then recomputes arena = sbrk(0)-sbrk_base as ~302MB and
	// hands out blocks overlapping late .bss (__sf, current_mallinfo,
	// temp_cwd — every observed victim). The streaming probe-gate
	// (malloc big + free immediately, MakeSpaceFor) fires exactly that
	// pattern constantly. Trim buys nothing on a console: disable it.
	mallopt(M_TRIM_THRESHOLD, 0x7fffffff);

	psInitConsole();
	PAD_Init();
	printf("reVC GameCube booting...\n");

	// The target is a GameCube: 24MB of MEM1 and nothing else. On Wii (used
	// only as a boot vehicle, since Dolphin emulates its SD) MEM2 exists but
	// is deliberately left alone, so the budget stays GameCube-sized.
	{
		size_t arena1 = (size_t)SYS_GetArena1Size();
		printf("mem: MEM1 arena %u KiB (%u MiB budget)\n",
		    (unsigned)(arena1/1024), (unsigned)(arena1/(1024*1024)));
#ifdef HW_RVL
		printf("mem: MEM2 present but unused (GameCube budget enforced)\n");
#endif
	}

	if(!psInstallFileSystem()){
		printf("FATAL: no filesystem (SD Gecko slot A/B or DVD)\n");
		psHalt();
	}
	printf("reVC GameCube: filesystem mounted (%s)\n", fileSystemIsFat ? "SD" : "DVD");
#ifndef HW_RVL
	// Memory-card write proof, one-shot, before any game code runs: a real
	// CARD file through the mc:/ devoptab. The artifact is an mcprobe GCI on
	// card A, checkable from the host even when the disc bring-up stalls
	// later. Removed once a full settings save is demonstrable in-game.
	if(GcCardMountDevice()){
		FILE *mf = fopen("mc:/mcprobe.bin", "wb");
		if(mf){
			fputs("reVC mc probe", mf);
			fclose(mf);
			printf("mc: probe written\n");
		}else
			printf("mc: probe open FAILED\n");
	}
#endif
	gcFlushCrashLog();

	// Boot self-check: the game opens assets with Windows-style backslash
	// paths, so verify both the raw path and the normalized one before the
	// engine starts and a failure turns into an unrelated crash. Results go to
	// the card as well as the console, since emulator logs are not reliable.
	{
		DVD_FS_GUARD;
		FILE *log = fopen("dvd:/revc_boot.log", "w");
		#define SELFTEST_LOG(...) do { \
			printf(__VA_ARGS__); \
			if(log) fprintf(log, __VA_ARGS__); \
		} while(0)

		char cwd[256];
		SELFTEST_LOG("selftest: cwd '%s'\n",
		    getcwd(cwd, sizeof(cwd)) ? cwd : "(getcwd failed)");

		bool ok = false;

		// List the card root: distinguishes "wrong path" from "empty/absent card".
		DIR *d = opendir("dvd:/");
		if(d == nil)
			SELFTEST_LOG("selftest: opendir root -> FAIL\n");
		else{
			int n = 0;
			struct dirent *e;
			while((e = readdir(d)) != nil && n < 6){
				SELFTEST_LOG("selftest: root[%d] '%s'\n", n, e->d_name);
				n++;
			}
			if(n == 0)
				SELFTEST_LOG("selftest: root is EMPTY\n");
			closedir(d);
		}

		DVD_FS_GUARD;
		FILE *f = fopen("dvd:/models/coll/peds.col", "rb");
		SELFTEST_LOG("selftest: raw open -> %s\n", f ? "OK" : "FAIL");
		if(f) fclose(f);

		char *norm = casepath("models\\coll\\peds.col");
		SELFTEST_LOG("selftest: normalized '%s'\n", norm ? norm : "(null)");
		if(norm){
			FILE *g = fopen(norm, "rb");
			SELFTEST_LOG("selftest: backslash open -> %s\n", g ? "OK" : "FAIL");
			if(g){ ok = true; fclose(g); }
			free(norm);
		}

		#undef SELFTEST_LOG
		if(log) fclose(log);

		// Report only. The game owns the boot flow from here; DoRWStuff now
		// tolerates a nil camera, so a bad read surfaces as the game's own
		// loading/version screen instead of a pre-boot halt.
		if(!ok)
			printf("WARN: assets not readable yet; continuing to game boot\n");
	}

	if(RsEventHandler(rsINITIALIZE, nil) == rsEVENTERROR){
		printf("FATAL: rsINITIALIZE failed\n");
		psTerminate();
		psHalt();
	}

	ControlsManager.MakeControllerActionsBlank();
	ControlsManager.InitDefaultControlConfiguration();

	// ponytail: RsRwInitialize only reads this as displayID; the console has none.
	if(RsEventHandler(rsRWINITIALIZE, nil) == rsEVENTERROR){
		printf("FATAL: rsRWINITIALIZE failed\n");
		RsEventHandler(rsTERMINATE, nil);
		psTerminate();
		psHalt();
	}

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

	// Lowest priority: it must never take time from the game, and it only has
	// to run when the game has stopped running.
	LWP_CreateThread(&watchdogThread, watchdogMain, nil, nil, 16*1024, 127);

	while(SYS_MainLoop() && !RsGlobal.quit){
		// Named, because it sits between the frame's last marker and "loop":
		// a stall in here used to report as "endofframe" and send the search
		// into the present path.
		gPhase = "handleexit";
		HandleExit();

		// gFrameTick is bumped in DoRWStuffEndOfFrame now, not here: frames
		// presented is the honest measure, and the menu renders two of them
		// from outside this loop.
		gPhase = "loop";

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
			printf("GS_INIT_ONCE: CGame::InitialiseOnceAfterRW\n");
			LoadingScreen(nil, nil, "loadsc0");
			if(!CGame::InitialiseOnceAfterRW()){
				printf("FATAL: InitialiseOnceAfterRW failed\n");
				RsGlobal.quit = TRUE;
				break;
			}
			// Debug: dvd:/autostart.txt skips the frontend so crashes in world
			// load reproduce with no controller input.
			DVD_FS_GUARD;
			FILE *as = fopen("dvd:/autostart.txt", "r");
			if(as){
				fclose(as);
				printf("autostart.txt: skipping frontend\n");
				gGameState = GS_INIT_PLAYING_GAME;
			}else
				gGameState = GS_INIT_FRONTEND;
			break;
		}

		case GS_INIT_FRONTEND:
			LoadingScreen(nil, nil, "loadsc0");
			FrontEndMenuManager.m_bGameNotLoaded = true;
			FrontEndMenuManager.m_bStartUpFrontEndRequested = true;
			gGameState = GS_FRONTEND;
#ifndef HW_RVL
			// One-shot memory-card write proof: the full settings dump goes
			// through mc:/ the moment the frontend exists. The artifact is a
			// gta_vc.set file on card A — checkable from the host.
			FrontEndMenuManager.SaveSettings();
			printf("mc: settings save attempted\n");
#endif
			break;

		case GS_FRONTEND:
			RsEventHandler(rsFRONTENDIDLE, nil);
			if(!FrontEndMenuManager.m_bMenuActive || FrontEndMenuManager.m_bWantToLoad)
				gGameState = GS_INIT_PLAYING_GAME;
			break;

		case GS_INIT_PLAYING_GAME:
			printf("GS_INIT_PLAYING_GAME\n");
			InitialiseGame();
			FrontEndMenuManager.m_bGameNotLoaded = false;
			gGameState = GS_PLAYING_GAME;
			BootLog("entering game loop");
			break;

		case GS_PLAYING_GAME:
			RsEventHandler(rsIDLE, (void *)TRUE);
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
