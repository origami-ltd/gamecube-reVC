# reVC on GameCube — next session

Everything is COMMITTED and pushed to `private` (github.com/ebellumat/gamecube-reVC),
branch `gamecube`. `origin` is mrxenginner's upstream — do not push there.

Branches: `gamecube` (current), `floor-08-20` (the user's named PISO — a known-good
state), `wip-08-20` (everything the 08-19/20 sessions produced, including discarded
MEM2 experiments; cherry-pick only). Tarball backup:
`/Users/ebellumat/revc-backup-0820-1730/worktree-full.tar.gz`

The librw submodule sits on a LOCAL-ONLY commit (`9a6e550`, branch `wip-08-20`); its
remote is mrxenginner's. A fresh clone will not build until that is resolved.

## Rules the user set, learned the hard way

- **MEM2 DOES NOT EXIST.** GameCube = 24MB MEM1 + 16MB ARAM (DMA-only, not CPU/GX
  addressable). MEM2 is Wii-only. The Wii dev target is a valid proxy ONLY while MEM2
  is untouched, because Wii MEM1 == GameCube MEM1. MEM2 may stand in for ARAM alone,
  capped at 16MB. A whole day was lost to ignoring this.
- **One Dolphin at a time.** `pgrep -x Dolphin` before launching; if one is alive and
  it is the user's, wait. Never `pkill` blindly while they are playing.
- **Dolphin's window opens on the MacBook built-in display** (AppleScript-move to
  {2570,-50} right after launch).
- **Never write the SD image while Dolphin runs** — its buffered view clobbers host
  writes. Reads may be stale. `fsck_msdos -y` after every write.
- **No timed background waits.** Tail logs live; the user closing Dolphin is the signal.
- **Texture quality is untouchable.** Memory pressure is solved by eviction/offload.
- Ask before installing anything (e.g. an audio loopback driver).

## Audio: what is measured, what is fixed, what is open

The instrument to use — do not go back to guessing:

- `dvd:/audiotest.txt` on the card arms **AUDIOTEST**: before the game boots it plays
  every category (9 stations, ambience, mission voice, effects across the rate/size
  range), measures the RMS of what it actually decoded, and writes
  `dvd:/audiotest.log`. Opening a stream proves nothing; the RMS does.
- `tools/gamecube/audio_census_compare.py` puts that next to the SAME asset decoded
  the way wasm-vice-city does (stations from `.adf` = MP3 XOR 0x22, its `CADFFile`;
  ambience/mission from `.mp3`; effects from `sfx.raw`). Last run: **25/25 within
  3dB, worst 0.2dB**; every effect −0.0 to −0.2dB.
- `tools/gamecube/audio_compare.py` measures aliasing against a host-side reference
  that mirrors the console's own polyphase windowed sinc.
- The heartbeat (`dvd:/audio.log`, AHB line) carries `conv=ok/total pool=peak/budget`.

Facts established by measurement:

- AESND's ucode resamples by repeating samples. Measured against a proper
  reconstruction it injects **47x to 185x** the energy above the source's own Nyquist.
  That is the "robotic" timbre. Each channel is therefore converted ONCE, on the way
  in, to `DSP_DEFAULT_FREQ` — 54MHz/1124 = **48042.7Hz** on GameCube, not a flat
  48000, so the ucode's resampler runs at ratio 1.0 and never engages. 48kHz 16-bit
  is the machine's ceiling.
- Conversion draws on a 2MB MEM1 pool. It was reading `conv=287/1338` — 79% of sounds
  falling through to the DSP — because buffers of sounds that had already finished
  were never returned. Service reclaims them now: **`conv=271/271`**, pool peak 1464K.
- Radio never played because `MusicManager` gates it on `GetMusicVolume() != 0`, that
  getter reads the class member `m_nMusicVolume`, and this backend only ever wrote the
  file static `gMusicVolume`. Both setters mirror now.
- The menu's white noise was the first Vorbis block after `ov_pcm_seek_page`: a page
  seek lands mid-stream with the previous block's overlap half missing, so the first
  decode is noise. One chunk is discarded after the seek. (The page seek itself stays:
  `ov_pcm_seek` decodes forward to an exact sample and measured up to 148ms in a
  single frame over a 70MB station — that was the stutter.)

### OPEN — cutscene speech, the live bug

User: the intro lines "play from the middle onwards, never whole", and earlier "the
line is skipped entirely, then the next one plays", and once "a CLICK where the line
should have been". Three fixes have landed on this and the user has NOT yet confirmed
the last one:

1. `AESND_SetVoiceStop(voice, false)` does not re-arm a stream voice — resuming a
   preloaded line left its position frozen at the primed samples, no callback ever
   came. `gcStreamArm` now re-arms properly.
2. A dry decoder is not the end of the sound. Priming decodes chunks before a line
   starts and a short line can be shorter than that (`intro2.wav` is 22374 samples);
   stopping the voice when the decoder ran dry threw away decoded-but-unplayed audio.
   `st->eof` now lets the callback drain what it has.
3. Preload was arming the voice AND the start was arming it again, so the play pointer
   advanced past chunk one — 8064 samples, 366ms, off the front of every line. Preload
   primes without arming now. Trace confirms preload leaves `pos=8064` and start
   advances to `16128`.

**Next step:** get the user to listen again. If lines are still clipped, the trace is
already in place — `gcStreamTrace` logs every transition on slots 1 and 2 (preload,
start, and the first `IsStreamPlaying` answers) to `audio.log` as `MA s<n> ...`. Read
`pos=`/`len=` there. Note the state machine gives a stream **30 frames**
(`nCheckPlayingDelay`, AudioLogic.cpp ~10375) to report itself playing before it
writes the line off as FINISHED and moves on.

**TEMP, remove at bring-up close:** `gcStreamTrace` and its call sites; the bounded
`dvd:/chan.log` StartChannel log if still present.

### OPEN — user reports not yet closed

- **Bike engine loop.** `SetChannelLoopPoints` was a stub; it now hands AESND only the
  loop region (points arrive as byte offsets, and are scaled when the sample has been
  converted). NOT verified by ear — the user reported the engine "always restarts from
  the beginning, or a high-pitch engine sample is missing".
- **Menu still emitted noise on cursor moves** as of the last report. The conversion-pool
  fix landed after that report and plausibly covers it (menu blips were among the 79%
  falling through to the aliasing path) — needs confirmation.
- **Colour filter / world lightmaps** only take effect after toggling away and back to
  the value already selected: the variable holds the right value at boot but nothing
  applies it. The user has turned them off and deferred this. Rows are registered in
  `src/core/re3.cpp` (`FrontendOptionAddSelect`, "NeoLightMaps") and
  `MenuScreensCustom.cpp` (POSTFX_SELECTORS → `CPostFX::EffectSwitch`).
- **Graphics settings page makes no menu selector sound** while other pages do.

### Vorbis for the whole sample bank — TRIED, REVERTED, and why

`tools/gamecube/pack_sfx.py` + `test_sfxpack.py` exist and work: they encode each
sample and keep whichever is smaller, per sample, because the choice is not uniform —
a Vorbis stream carries three header packets however short the payload, so a
1400-byte effect encodes to 3817 bytes while the long ped speech comes out 4x to 7x
smaller. Measured: **324.5MB → 80.2MB**, 9726 of 9941 samples worth encoding.

It was reverted (commit `b0d1a8a8`) because the resident bank — samples **0..524**,
14.3MB, decoded into ARAM at boot — became ~1000 `ov_open`/`ov_clear` pairs, and MEM1
fragmented badly enough that the next allocation after the bank wedged the boot. The
fix, if this is picked up again, is already known and half-built: keep samples below
525 as raw PCM (`--raw-below 525`, which the packer supports) and compress only the
ped region. That measured 340MB → 91MB with no Vorbis decoding at boot at all.

The user's stated reason for wanting it: disc space, load speed, and a belief that
native PCM sounds worse. Be honest with them — Vorbis is lossy and the decoded audio
measured identical (0.30dB); the win is space and read time, not timbre.

## Non-audio state

- Boots and plays at 60fps in-world.
- **Legs twisting on reverse** was an IFP parser regression: the port had widened the
  bone-tag read from stock's `== 44` to `>= 44`. In the real ped.ifp, 4322 ANIM chunks
  are 44 bytes and 709 are 48, and offset 40 means different things — at 44 it is the
  bone tag, at 48 it is a hierarchy tree link. All 709 bound to the wrong bone (Root
  drove the Neck), and `CAR_LB`, the reverse look-behind, is one of them. Fixed.
- **Options are loaded at boot now** — `FrontEndMenuManager.LoadSettings()` was never
  called in the GameCube skeleton, so settings were written every menu change and
  discarded at the next boot. The options file is `gta_vc.set`, separate from the story
  slots `GTAVCsf*.b`. NOTE: an older handoff claimed the memory card was "PROVEN BOTH
  DIRECTIONS" — that was wrong. `mc:` is still rejected by crossplatform.cpp's path
  normaliser; the proven round trip is the dev target's SD.
- **Debug HUD hang**: `CFont::GetNumberLines` and `GetTextRect` wrap by resetting x and
  advancing y WITHOUT advancing the string, so a word wider than the wrap box spins
  forever. `PrintString` already guarded this with `!first`; the two measuring passes
  did not, and they only run when a background box is on — which the HUD turns on with
  a ~24-unit box. Fixed, `tools/gamecube/test_font_wrap.py` covers it.
- The RESOLUTION row is gone from the graphics menu: the EFB-to-XFB copy can only scale
  UP, so 528p could not reach the 480 output and 720p is beyond a GP that tops out at
  640x528.
- MEM1 stays tight on a 24MB machine. Levers if allocation failures return: streaming
  budget, the 1.3MB of frontend textures never freed (Frontend.cpp UnloadTextures), ped
  slots. NOT texture quality, NOT MEM2.

## Host-side checks that exist (run them, they are fast)

    python3 tools/gamecube/test_adpcm.py        # voice decoder, bit-exact vs ffmpeg
    python3 tools/gamecube/test_resample.py     # in-place 48k conversion safety
    python3 tools/gamecube/test_font_wrap.py    # the wrap-loop hang
    python3 tools/gamecube/test_sfxpack.py      # only if the pack is revived

## How to work on this

Measure before theorising. An empty log is not a passing log. The user's ears have
been right every single time and their one-line reports have named the mechanism more
than once — "it stops when I enter the radio menu" was the page-seek noise, "from the
middle onwards" was the double-arm. When they take the keyboard, stop injecting input.
