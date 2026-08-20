# reVC on GameCube — next session

TWO Claude sessions worked this tree in parallel on 08-19/20 (github-3f =
audio/ISO/droplets/menu; the goal session = artifacts/anim/streaming). They
coordinated live via SendMessage; everything below is merged and current.
The tree is ALL uncommitted; be surgical.

## 08-20 night — current state, all committed. READ FIRST.

Branches: `gamecube` (current, good), `floor-08-20` (the user's named PISO),
`wip-08-20` (everything the day produced incl. the discarded MEM2
experiments - cherry-pick only). Tarball:
/Users/ebellumat/revc-backup-0820-1730/worktree-full.tar.gz

### THE RULE THIS DAY COST: MEM2 DOES NOT EXIST

GameCube = 24MB MEM1 + 16MB ARAM (DMA-only, not CPU/GX addressable). MEM2 is
Wii-only. The Wii dev target is a valid proxy ONLY while MEM2 is untouched,
because Wii MEM1 == GameCube MEM1. MEM2 may stand in for ARAM alone, capped
at 16MB, DMA discipline only (gcBankAlloc enforces it). Everything else that
went to MEM2 this day was reverted.

### Root causes found and fixed, each with its evidence

1. **Stutter (goal item 1) - MEASURED before/after.** Not the codec: the
   radio's `ov_pcm_seek` bisects a 70MB station and then decodes forward to
   an exact sample. Over 278 beats of live play the audio column peaked at
   **148.2ms** and the worst frame at **71.9ms**. `ov_pcm_seek_page` does the
   same job to within a page (inaudible on radio). After, over 261 beats:
   audio max **12.4ms**, worst frame max **21.6ms**, and stream opens are now
   logged directly (`open=Nms` in audio.log) at 0-8ms.
2. **Echo / "mono desynchronised between both ears".** AUDIO_REFLECTIONS
   spawns delayed copies on purpose; they are meant to be panned apart and
   attenuated. In ProcessActiveQueues' start path `k = (j + m_nChannelOffset)
   % m_nActiveSamples`, the sound is started on **k** but volume and pan were
   computed into and applied to **j**. Measured with a bounded StartChannel
   log: 300/300 starts carried the default pan and 166 were the same sample
   twice within 300ms. Only builds without EXTERNAL_3D_SOUND compile that
   branch, which is why the PC never saw it. AudioManager.cpp, four `j`->`k`.
3. **Metallic quality.** AESND's ucode resamples by repeating samples. Each
   channel is now interpolated once, on the way in, to the DSP's own rate -
   and that rate is `DSP_DEFAULT_FREQ` = 54MHz/1124 = **48042.7Hz** on
   GameCube, not a flat 48000, so the ucode's resampler runs at ratio 1.0 and
   never engages. 48kHz 16-bit stereo is the hardware ceiling. The old 48kHz
   *bank* (46MB against 14.3MB of source) never fit ARAM and is gone.
   `tools/gamecube/test_resample.py` proves the in-place conversion is safe
   for every rate in the game's own sfx.sdt and within 1 LSB of float.
4. **Legs twist when reversing (goal item 2).** Not the blend layer - that
   fix targets compressed anims and ANIM_COMPRESSION is off. The port had
   widened the IFP bone-tag read from stock's `== 44` to `>= 44`. In the real
   ped.ifp, 4322 ANIM chunks are 44 bytes and 709 are 48, and offset 40 means
   different things: at 44 it is the bone tag (Root=0 Pelvis=1 Neck=4), at 48
   it is a hierarchy tree link (Root=4 Pelvis=5, sibling at offset 44).
   Reading the link as a tag bound all 709 to the wrong bone - Root drove the
   Neck - and CAR_LB, the reverse look-behind, is a 48-byte anim that
   animates thigh, calf and foot. AnimManager.cpp back to `== 44`.
5. **Options never loaded (goal item 3).** glfw and win both call
   `FrontEndMenuManager.LoadSettings()` at startup; the GameCube skeleton
   never did, so settings were written on every menu change and discarded at
   the next boot. Wired at the same point. The options file is `gta_vc.set`
   (1783 bytes on the card, volumes 49/49), separate from the story slots
   `GTAVCsf*.b`. NOTE: the older handoff claim that memcard was "PROVEN BOTH
   DIRECTIONS" was wrong - `mc:` is still rejected by
   crossplatform.cpp's path normaliser, so the proven round trip is the dev
   target's SD. The GC memory card needs `mc:` past `GameCubePath.h` plus a
   `chdir_r` in carddev.c, and is gated behind the GC-mode render crash.
6. **Debug HUD hang.** `CFont::GetNumberLines` and `GetTextRect` wrap by
   resetting x and advancing y WITHOUT advancing the string, so a word wider
   than the wrap box spins forever. PrintString already guarded this with
   `!first`; the two measuring passes did not, and they only run when a
   background box is on - which the HUD turns on, with a ~24-unit box. The
   user's hang.log named it: `phase=2dafterfade, gp rd=1 cmd=1`.
   `tools/gamecube/test_font_wrap.py` covers it.
7. **Voice is native.** Mission speech ships as the game's own IMA ADPCM
   .wav (39MB, mono, mostly 22050Hz, 512-byte blocks) - no Vorbis, no Tremor
   decode state. Decoder is bit-exact vs ffmpeg
   (`tools/gamecube/test_adpcm.py`). One bug worth remembering: a block
   decodes to 2034 bytes and the stream chunk is 16128, which do not divide,
   and zero-filling the remainder punched ~43ms of silence into every 323ms
   of dialogue. The leftover is carried across chunks now.

### Audio architecture, to the user's spec

Music (radio) is the ONLY thing that gets Vorbis, at its own native 44.1kHz
stereo (measured from the .adf: 44100/2ch/128kbps). Mission .mp3 transcodes
at its source rate (32000 stereo), ambience at 22050 mono, voice ships
native ADPCM, SFX stay native in ARAM. A staged conversion is in
`/Users/ebellumat/revc-audio-native` (convert_audio.py --music-rate 44100
--radio-quality 4); copy onto the card with Dolphin CLOSED, then fsck.

### Open

- TEMP diagnostic to remove at bring-up close: the bounded StartChannel log
  (`dvd:/chan.log`) in sampman_gamecube.cpp. It samples the FIRST 300 starts,
  which are frontend/intro and legitimately 2D (pan 63); to check positional
  pan, make it skip the first few hundred and sample in-world.
- MEM1 remains tight on a 24MB machine. Levers if allocation failures return:
  streaming budget, the 1.3MB of frontend textures never freed
  (Frontend.cpp UnloadTextures), ped slots. NOT texture quality (user rule),
  NOT MEM2.
- Two findings from an audit, verified in code but not yet acted on:
  `AR_Init(nil, 0)` at CdStream_gamecube.cpp:71 (the sampman side already has
  the 300-entry array; whoever initialises first wins) and the write-buffering
  block sitting in `myfread` instead of `myfwrite` in FileMgr.cpp.
- ISO budget vs the 1.46GB mini-DVD: re-measure after the native re-encode.

## 08-20 afternoon session (audio deep-dive) — READ FIRST, supersedes older audio notes

User directive of record: "fix ALL audio, best possible quality, fail-loud".

1. **THE sfx mute root cause, fixed**: sfx.sdt (tSample index) was read raw on
   big-endian Gekko — every offset/size/frequency byteswapped garbage since the
   backend was written. Bank 0 sized itself 1.58GB→alloc fail→no bank→every
   channel silent (menu beeps, engine, footsteps — the "mute reports").
   LE→BE swap at InitialiseSampleBanks. AHB now shows ch=27..29/29 playing.
2. **Bank range fix**: bank 0 spanned all of sfx.raw (340MB); ends at
   SAMPLEBANK_PED_START now (real 14.3MB; ped region streams per-sample).
3. **Wii bank alloc fixed**: LoadSampleBank used raw AR_Alloc (no ARAM on Wii,
   HW_RVL MEM2 shim existed but was never wired). gcBankAlloc everywhere.
4. **Quality (HW_RVL)**: bank linear-resampled to 48kHz at load into MEM2
   (~46MB, arena2 fits w/ 5.8MB spare) — AESND ucode is ZOH, native-rate
   samples aliased ("qualidade uma merda"). Channels now POINT into the MEM2
   bank (no per-channel MEM1 copies — the 205088B memalign crash). Ped/talk
   still copy (slots rotate). StartChannel scales voice freq 48000*f/base for
   bank samples. GC(!RVL) keeps native-rate ARAM path.
5. **Cutscene "2x" + speech cut mid-scene = ONE bug, fixed**: ms_cutsceneTimer
   used NonClipped dt; cut.log caught it jumping +68s across a 60-frame load
   stall → scene hits end-time mid-load → teardown kills the (real-time)
   audio halfway. Now clipped (GTA_OGC), same 60ms/frame cap as sim/anims.
   User confirmed pacing fixed ("a cena em 2x ta resolvido").
6. **Mission speech existed nowhere**: convert_audio.py only did .adf/.mp3 —
   all 1120 .wav (intro1-4, dialogue, scanner) never converted. Now .wav too
   (48k mono ogg, 28MB — NOT 400MB, measure properly), on card + staging.
7. **lengths.cache prebuilt host-side** (big-endian u32 ms, StreamedNameTable
   order, built by convert_audio.py build_lengths_cache) — REQUIRED for ISO:
   the game's own cache write fails on read-only media every boot. Also fixes
   the Flash-FM/Billie-Jean new-game rule (NewGameRadioTimers % length was
   % 0 before). 1224 tracks, 0 missing.
8. **Fail-loud audio (AUDIO_FAIL_LOUD in sampman_gamecube.cpp)**: any audio
   that cannot be served → gcFatalPark maroon screen naming it (path, ov rc,
   libc-free) + crash.log. User-requested diagnostic mode; one #define to
   ship-silence. gcFatalPark made non-static for this.
9. **MusicManager guard**: ChangeMusicMode(GAME) mid-cutscene w/ preloaded
   track is blocked+logged (frontend teardown ran during cutscenes on OGC).
   IsStreamPlaying: paused reads NOT-playing (OAL parity).
10. **OPEN — THE blocker at session end: libc HEAP CORRUPTION in MEM1.**
    Evidence chain, in order: (a) ov_open on water.ogg dies OV_EREAD(-128)
    with fordblks=441K; (b) mallinfo uordblks reads impossible garbage
    (308265K "used" in a 24MB arena); (c) audio.log gets binary junk blocks
    written into it; (d) final boot: **Invalid write to 0x0000000C,
    PC=0x80313d70 = _calloc_r** — allocator walking a smashed free list.
    Someone writes through a stale/oob pointer into heap metadata.
    VERDICT (session revc-gamecube-ad, 14:38): resampler and pointer-channels
    CLEARED by static audit (alloc-sum matches writes; every free is
    pcmOwned-guarded). Root cause: UNGUARDED dvd:/ stdio racing the CdStream
    worker inside libfat — the disease the fsLock was born for, regressed by
    the night's new logging (AHB heartbeat every 5s of gameplay = the junk
    blocks in audio.log; cut.log; MMODE log; watchdog hang.log; gcFatalPark
    crash.log; screendroplets one-shots; ExportStats). All sites now guarded
    via new primitive CdStreamFsTryLock (CdStream.h/CdStream_gamecube.cpp);
    watchdog uses try-lock skip-when-busy to preserve the empty-log-means-
    fs-wedged signal. A live user crash at __sflush_r (stdio glue) matched
    the class. Fixed build 14:38 — VALIDATION BOOT PENDING at handoff time:
    confirm no _calloc_r/__sflush_r crash, then re-run the audio checklist
    (intro speech full-length, car radio Flash-FM/Billie-Jean rule, menu
    beeps, engine, quality pass by ear).
11. **Hang WATCH updated**: hang.log now non-empty — phase=2dafterfade,
    gp rd=1 cmd=1 under=1 (GP FIFO underrun, GX side, NOT fs). endofframe
    spam variant seen too. Separate from audio.
12. github-3f GC-ISO front, CLOSED for the day (full bisection): boot now
    reaches mount ✓ mcprobe ✓ Populate(1x) ✓ neo.txd open+rows ✓ deep load
    471MB ✓ then PINS at window 0x1c120000 = /models/hud.txd +0x28000 —
    minutes rereading ONE 32KB window. Eliminated: DVD_ReadPrio failures
    (never fail), md5 diffs (identical), alignment (fills now fixed 16-sector
    windows in rewritten __read), gxNativeFail retry (never fires, routed to
    mc:/diag). Remaining fork: parser re-seek loop with GOOD bytes vs DI
    delivering WRONG bytes that double-read misses. NEXT (1 boot): CRC probe
    in __read vs host-known ISO bytes → decides the fork; if wrong bytes,
    root-fix DVD_ReadPrio's one-late (try DVD_ReadAbsAsyncPrio + own
    completion flag instead of the sync LWP wait). TEMP crumbs to remove at
    bring-up close: re3.cpp (NEOCK/diag), custompipes.cpp (NEO_CRUMB),
    gxraster mc-mirror of nativeFail.
    LATE CLOSURE: CRC probe ran — pinned window's bytes are byte-IDENTICAL
    to the host ISO (crc b0506cfe both sides, zero diffs across fills). The
    ISO/DI layer is EXONERATED; the consumer loops on good bytes. Convergent
    hypothesis with item 10: hud.txd load alloc-fails (MEM1), caller retries
    forever, gxNativeFail never fires because OOM doesn't route through it.
    One mallinfo log at the txd-load fail confirms. The GC-ISO blocker and
    the heap/margin disease are likely ONE workstream. MEM1 diet candidates
    if margin is needed: the 630KB ped slots. (carddev.c DONE by github-3f
    at close: on-demand alloc — read = file size, write = 8KB growing to
    256KB cap, pad at flush; ~248KB less pressure per card open, both GCI
    paths boot-verified.)
13. **ISO budget**: mini-DVD 1.46GB. ISO was 1.51GB; +28MB speech. ~80-90MB
    to trim. Lever: radio q4.5→q3.5 re-encode (~90-120MB) — user cares about
    quality, get their sign-off first. lengths.cache MUST ship on the ISO.
14. Boot etiquette while 2 sessions test: NO boot.sh (its pkill -9 kills all
    Dolphins) — manual launch + kill by OWN pid only; gecko 55020 belongs to
    the wii session; GC session uses mc:/ as its channel.

## FIXED this night — user-verified where noted

1. **Corner square + ball: DEAD (user: "RESOLVIDO O QUADRADO E A BOLA").**
   Three contributors, in discovery order: CCutsceneShadow RTT drew to the
   main screen (Create() refuses on GTA_OGC); FrameUpdate's degenerate-blend
   guard seeded zero quats (guard v2 seeds identity); and the LAST and main
   one — `CustomPipes::EnvMapRender` re-rendered the world into a 128×128
   camera texture (= EFB top-left corner) every frame and multiplied it by
   CarReflectionMask (ZERO/SRCCOLOR = the corner hunter's first catch,
   misattributed to ShadowCamera). Nothing on GX consumes EnvMapTex, so it
   is stubbed on RW_GAMECUBE (custompipes.cpp). It fired every frame because
   VehiclePipeSwitch defaulted to NEO (github-3f boot 24) — trigger and
   artifact were introduced together, which is why it looked "new".
   The corner hunter in gx.cpp drawIm2D stays armed (gecko+card, caller
   return address; `powerpc-eabi-addr2line -e build/wii/src/reVC.elf -f -C`).

2. **Twisted limbs / upside-down peds (quiropraxia): root-caused + fixed.**
   Compressed keyframes never get RemoveQuaternionFlips at load, and stock
   `CalcDeltasCompressed` wrote `SetRotation(-rotB)` — the original value
   back (no-op) — leaving theta computed from a flipped pair while
   UpdateCompressed slerped the raw pair: up to 180° error mid-interpolation
   (numeric proof ran in-session). IDLE_stance carries 75 flip pairs (most
   in ped.ifp — the stretch fidget that "always bugged"), KO_*/HIT_*/CAR_*
   next; anims play compressed constantly because the uncompressed LRU cache
   is 25 hierarchies. FIX (AnimBlendNode.cpp): hemisphere-correct rotB
   LOCALLY in BOTH CalcDeltasCompressed and UpdateCompressed. **NEVER write
   the flip to storage** — sequences are shared across peds at different
   phases and the loop seam (last→0) cascades the flip around the ring:
   tried, produced upside-down peds, reverted same night. FrameUpdate's
   near-zero guard stays as belt-and-suspenders for true QZERO nodes.
   Post-fix captures (~200 frames: walking, idling, NPCs) all clean; an
   in-car reverse close-up on the fixed build is still worth one user look.

3. **Streaming eviction churn: 243 → 2.6 evictions/beat (measured).**
   The ledger (ms_memoryUsed) sat at cap while the heap truly had 2.4MB
   free: every load evicted a visible model, which was re-requested next
   frame — the dark/bright LOD flicker on distant buildings AND interior
   floors, the rainbow bands (live texobj sampling a freed tiled buffer),
   and constant disc churn. FIX (Streaming.cpp MakeSpaceFor): probe
   malloc(want+512K) and skip eviction when the block exists. NOT
   mallinfo/fordblks — total-free ignores fragmentation and can live-lock a
   large load with eviction gated off. Loads fell 230 → 6/beat.

4. **P-line X vararg bug**: X%u (worst frame per beat) rode with no matching
   argument since it was added — every later column shifted one arg left (X
   showed the limiter; w printed stack garbage; github-3f's "X miswired"
   readings were this). gxWorstSnap/100 is now actually passed. Full column
   map: s sky, b begin, r render, m sim, F frame, X worst-frame, L limiter,
   V vsync-pref, C colourfilter, M blur, li list, pr pre, a audio, fd fade,
   af after, ti tile, st stream, cp copy, vs vsync-wait, d drops, q fxQueued,
   w fxDrawn (tenths of ms).

5. **Small ones**: FPS box hugs the number (wrapx conditional; wide box only
   in verbose — GetTextRect's right edge IS wrapX for left-justified text);
   carddev.c truncates to actually-read bytes on CARD_Read failure so a
   corrupt card file parses as short → defaults; duplicate hb.log HB line
   removed (it was already written at the block end).

6. **MBlur water/blood lens drops: KEPT** (user rule: fix, don't remove).
   The goal session briefly refused the whole fx queue on GC chasing the
   corner ball (wrong suspect — it was EnvMapRender) and deleted the OGC
   one-pass TEV draw; both restored verbatim. Note the cost that remains
   real: ANY queued fx = one full-screen EFB grab per frame in
   MotionBlurRender. The user's "hydrant → stutters ficam frequentes"
   report may be this (burst hydrants queue splash fx forever) or may have
   been the now-fixed eviction churn — re-test hydrants before touching it.

## From github-3f (still current, see its full notes in git history of this file)

- Audio subsystem live end-to-end (channel count, MEM2 bank shim, LE→BE
  swap, AESND 1152-multiple chunks, callback-side swap). Radios re-encoded
  **48kHz** stereo q4.5 — deliberate deviation from the "32kHz" spec: the
  DSP resampler is zero-order-hold and 32kHz aliased ("metallic"),
  user-approved direction. Boot audio machine-verified by DumpAudio RMS;
  the goal session measured the user's own 8.7-min session at 39% loud.
  STILL OWED: user-ear pass per category (radio in car, dialogue, frontend).
- Both-zero-volume settings recovery (the "all muted" fossil) in LoadSettings.
- ISO/GC-mode: build/gc + revc-full.iso boots; Dolphin GC DI delivers reads
  one late (iso9660_dbg double-read is a workaround, root cause open); NEW
  blocker: endless two-sector retry loop (0x2e800/0x07021000) pre-frontend.
  SYS_STDIO_Report printf does NOT reach Dolphin's log (OSREPORT never
  patches our libogc — count 0 across all runs), so the loop's failing file
  cannot be named that way: fix DVD_ReadPrio's one-late delivery at the root
  (kills the double-read AND likely the loop) or give gxNativeFail an
  EXI/gecko path that works without the listener.
  The loop is characterized: minutes of re-reads of ONE 32KB region =
  /neo/neo.txd data (LBA 57410, 28968B), between mcprobe and big-data loads;
  only two call-once readers exist (re3.cpp existence check via casepath,
  CustomPipeInit RwStreamOpen+FindChunk). CODE-RULED-OUT: EOF masking —
  _ISO9660_read_r clamps to entry.size and returns 0 exactly once, so
  FindChunk cannot spin on short reads. Remaining: DVD_ReadPrio returning
  <=0 (each __read fill = TWO ReadPrio probes; error resets the cache, so a
  retrying caller re-hits the DVD every time) — find the retry layer or fix
  the DI one-late root.
  GC-mode render crash atomicRenderCB gx.cpp:1897 (nil+0x14) gates ALL
  GC-mode verification (incl. the memcard GCI write proof). ISO is 1.51GB —
  50MB over a real mini-DVD; trim before hardware.
- Memcard (goal 5): mc:/ devoptab + _psGetUserFilesFolder("mc:") wired and
  PROVEN BOTH DIRECTIONS on the GC-ISO boot: LoadSettings reads through mc:/
  and a one-shot probe created 01-GRVC-mcprobe.bin.gci on Card A (8256B,
  payload verified) with the carddev truncation fix compiled in. Remaining:
  the in-game journey (menu exit → gta_vc.set GCI), gated on the GC-ISO
  read-loop; the GS_INIT_FRONTEND SaveSettings one-shot self-verifies it
  once that's fixed. REMOVE the mcprobe one-shot in gamecube.cpp when
  in-game saving is demonstrable. Dev target (Wii .dol) settings on SD work
  end-to-end.
- Screen droplets rewritten on the im2D path (user: "RESOLVIDO CARALHO");
  CMPR banned ≤64px in txdconv (square halos on additive textures).
- STATS menu row (OFF/FPS/VERBOSE, ini-persisted).

## WATCH — the one open instability

One hang observed (gecko "HANG endoff", hang.log EMPTY = fs wedged with the
main thread inside, then Dolphin ITSELF died: "libc++abi: terminating" right
after "Stopping DSP Audio logging"). Happened ONCE on the fordblks-gate
build; a 10-min walking soak on the probe-gate build ran clean past 7min
(check soak result in the goal session's last messages). Suspects, ordered:
the replaced fordblks gate (gone), the file-write freeze class
(c2725998/902b9c29), Dolphin's DSP-dump teardown racing shutdown
(DumpAudio=True still on in Dolphin.ini — github-3f suggests capturing one
more repro before turning it off, else it becomes unreproducible).

## Remaining user reports / goal items

- **Pink save-point light pops with distance** (no smooth falloff) —
  untouched; needs a live A/B at the hotel save room. Suspect per-mesh
  binary point-light application in the GX path or marker draw distance.
- **Interior floor flicker** — likely the (now fixed) eviction churn;
  verify an interior on the probe-gate build; if it survives, A/B menu
  Graphics → NeoLightMaps / NeoRoadGloss.
- **Stutters** — churn fixed; X column now real. Known secondary: gxSnapTile
  40–60ms/beat when textures stream in (tiling on main thread). Get the
  user's exact scenario with X>250 beats from hb.log.
- **Anim**: user look at reverse-legs + idle stretch on the current build.
- **Audio**: user-ear category pass; ARAM stream ring (cache.aramAddr never
  AR_Alloc'd) still dead code — wire or delete.

## Traps (all of github-3f's still apply)

- `git checkout <file>` destroyed uncommitted work once. Tree is ALL
  uncommitted. gxBeginUs/gxListUs increment sites are still lost (read 0).
- mtype/mcopy from the card while Dolphin runs shows STALE data for files
  Dolphin buffers; gecko is the realtime channel. (hb.log pulls mid-run
  worked for the goal session, but treat as possibly stale.)
- `pkill -x Dolphin` (boot.sh does it) kills EVERY instance — the user's,
  the scratch GC one, and any soak. Coordinate between sessions first;
  kill by PID when two instances must coexist.
- Gecko mangles concatenated lines: `grep -c "HB t="` undercounts (multiple
  HBs glue into one line). strings + count occurrences, not lines.
- Two Claude sessions share this tree. `ListAgents` → SendMessage before
  boot.sh, before editing files the other named, and before rewriting this
  handoff.

## How to work on this

Measure before theorising; one variable per boot; an empty log is not a
passing log; trust the screen over the code reading; the user's eyes are the
best instrument this project has — their one-line reports solved three hunts
this night. When they take the keyboard, stop injecting input instantly; when
they say "vou dormir, se vira", the pad is yours.

## Part 2 (early morning 08-20, after the user's "test it yourself" directive)

- **Anim layer proven 1:1 offline**: 283,746 interpolation samples across every
  anim/bone/keyframe of ped.ifp — STOCK compressed path diverges >2° in 8,105
  of them (worst 180°); the FIXED path's worst error is 0.04° (int16
  quantization floor). The visible "legs out of the car" in the user's morning
  screenshot is NOT the blend layer: a live skeleton probe (dvd:/autoanim.txt
  arms it; ANIM lines = player @250ms, BADBONE = any ped out of bounds) shows
  thigh/calf/foot distances sane through reverse, KO and death. Distances are
  rotation-invariant though — a twisted-but-bone-lengthed pose passes — so the
  remaining suspect list is: the jacked/ejected DRIVER ped lying by the door
  (KO/jacked anims), or the render/skin side. Reproduce by jacking an OCCUPIED
  car and looking at the passenger side.
- **wasm-revc save-loss bug FIXED and verified live**: SetSaveDirectory used
  "%s\\%s" outside _WIN32; on emscripten saves became a literal file
  "userfiles\GTAVCsf1.b" OUTSIDE the persistent IDBFS mount (lost on tab
  close) and slot probes could never match. PCSave.cpp now branches on _WIN32;
  runtime log confirmed /gtavc/userfiles/GTAVCsf.b paths. Rebuilt into
  web/public + web/dist. (Repo rule there: commits sole-author Erasmo, no AI
  trailers.)
- **wasm menu anomaly found**: page transitions never complete visually (the
  doubled "main menu" title = two pages drawn); input and state advance under
  a stale frame. Separate from the in-game stutter report. Stutter analysis so
  far: menu frames are 6.9ms median/9ms max (clean); the streamer spin-waits
  the main thread on cache misses by design (256K chunks, 400MB LRU,
  readahead) — in-game data still needed, blocked on menu navigation
  (keyboard only reaches the engine after a real canvas click, and the
  transition bug hides progress).
- **GC menu: FED_WIS relabelled "ASPECT RATIO"** on card (american+portuguese)
  via the new tools/gamecube/gxtpatch.py (repoint or add keys; TKEY stays
  sorted for the binary search). build_sd.py should run it so rebuilt cards
  keep the label + the three new keys FED_R48/FED_R52/FED_R72.
- **RESOLUTION row added** (Graphics): 480P / 528P SUPERSAMPLED / 720P
  (DOLPHIN IR), int8 rw::gx::gxEfbResPref, INI key Graphics/EfbHeight.
  MEASURED DEAD END on the apply side: efbHeight=528 with DispCopyYScale
  480/528 rendered a solid magenta frame — GX's YScale only scales UP
  (interlace doubling); the EFB->XFB copy cannot vertically downsample.
  The row persists the pref but 528/720 are INERT (behave as 480p) until a
  real path exists: PAL 576 output modes, or an EFB->texture resample pass
  drawn back before the display copy. 720p on the GP is impossible either
  way (640x528 raster cap) — that entry names Dolphin's IR scaler.
- User request log: default stays 480p.
- **NEVER mcopy-WRITE the card while Dolphin runs** — Dolphin's buffered view
  of the raw image clobbers host writes on its next flush (an INI hand-edit
  and two GXT pushes silently vanished this way). Reads lie too (older trap);
  writes CORRUPT. Card writes only with Dolphin dead, fsck after.

## GC × wasm parity audit (wasm = base) — 02:20

ANIMATION: SAME, now provably. Hierarchy/Association/RpAnimBlend byte-equal;
ANIM_COMPRESSION is off in BOTH configs so ped anims play uncompressed in
both; the compressed (keepCompressed cutscene) math carried the same stock
180°-flip bug in BOTH — fixed on GC during the night and now PORTED to the
wasm base (AnimBlendNode.cpp local hemisphere correction, both sites;
rebuilt + deployed to web/public and web/dist). Offline differential across
all of ped.ifp: 0/283,746 samples divergent (worst 0.04° = int16 floor).
GC-only deltas are defensive (IFP loader hardening, FrameUpdate degenerate-
sum guard) and do not alter output for valid data.

AUDIO: decision layer (AudioManager/AudioLogic/MusicManager/AudioCollision/
sampman.h/soundlist.h) is 0-diff — identical what/when/volume/priority.
SFX source data byte-identical (sfx.raw 340,245,502B + sfx.sdt 198,820B match
the install; GC byteswaps at load only). Deliberate divergences, documented:
radio/streams are 48kHz stereo Vorbis re-encodes on GC (base plays install
originals; 32kHz aliased through the DSP's zero-order-hold), and the voice
cap is AESND's 29 game channels vs the PC pool. Backends differ by necessity
(OAL vs AESND) under the same interface.
