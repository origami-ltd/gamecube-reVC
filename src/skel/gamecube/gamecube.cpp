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

#include <gccore.h>
#include <ogc/machine/processor.h>
#include <ogc/usbgecko.h>
#include <ogc/dvd.h>
#include <iso9660.h>
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
	// bounded retries: never hangs without a gecko, and the alive-probe
	// false-negatives on Dolphin's emulated gecko, so don't gate on it
	usb_sendbuffer_safe_ex(EXI_CHANNEL_1, msg, strlen(msg), 1000);
	usb_sendbuffer_safe_ex(EXI_CHANNEL_1, "\n", 1, 1000);
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
static void
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
	FILE *f = fopen("dvd:/crash.log", "a");
	if(f){
		fprintf(f, "---- %s ---- build %s %s\n%s%s\nSTACK:%s\n",
		    tag, __DATE__, __TIME__, msg, heap, stack);
		fclose(f);
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
	FrontEndMenuManager.m_PrefsUseWideScreen = false;
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
	RwCameraShowRaster(camera, nil, 0);
	VIDEO_WaitVSync();
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
		// ponytail: the DVD probe blocks forever on an empty/absent drive
		// (startup() and isInserted() both block), so it is opt-in. It is only
		// a fallback anyway: the 1.5GB asset set cannot fit a 1.35GB disc, so
		// SD Gecko is the real medium. Hold B at boot to try a disc.
		if(!fileSystemReady){
			PAD_ScanPads();
			if(PAD_ButtonsHeld(0) & PAD_BUTTON_B){
				printf("mount: probing DVD (ISO9660)...\n");
				if(ISO9660_Mount("dvd", &__io_gcdvd)){
					fileSystemReady = TRUE;
					fileSystemIsFat = false;
				}
			}else
				printf("mount: skipping DVD (hold B at boot to try disc)\n");
		}
		if(!fileSystemReady)
			return FALSE;
	}
	if(chdir("dvd:/") == 0)
		return TRUE;
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
	PAD_ScanPads();
	if(PAD_ButtonsDown(0) & PAD_BUTTON_START)
		RsGlobal.quit = TRUE;
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
	gcFlushCrashLog();

	// Boot self-check: the game opens assets with Windows-style backslash
	// paths, so verify both the raw path and the normalized one before the
	// engine starts and a failure turns into an unrelated crash. Results go to
	// the card as well as the console, since emulator logs are not reliable.
	{
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

	while(SYS_MainLoop() && !RsGlobal.quit){
		HandleExit();

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
