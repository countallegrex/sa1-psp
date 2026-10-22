# M3 Desktop audio

Exit test (README §8): BGM and jump/ring sounds in Emerald Coast on
desktop; with M3 also the voice-language switch (README §8 roadmap).

Status: **passed (2026-10-19).** The exit test (§6) holds on desktop and against console captures; M3 is ticked in README §8. Everything below was measured on the US v1.005
dump with `build-release/backend/desktop/sa1psp.exe` (clang 23, Release;
recompiled game code `-O1`) unless it says Debug, on 2026-10-19.

## 1. What the game does with sound

The full description is `docs/formats/sound.md`; in short:

- **Music** is 109 stereo ADX files played by name; **voice lines** are
  mono ADX entries of an AFS archive (`EVENT_ADX_US.AFS` English,
  `EVENT_ADX.AFS` Japanese, 2,044 lines each) played by index. The game's
  ADX manager creates one player of each at every area load and polls
  their status every frame; a cutscene does not go on until its line has
  ended.
- **Effects and character voice clips** are programs in the program banks
  (SMPB) of the multi-unit files (`STATIONSQUARE.MLT`, `P_SONICTAILS.MLT`,
  `V_SONICTAILS_E.MLT`, ...), which the driver copies to fixed addresses in
  the AICA's 2 MB sound RAM. The game plays them as **plain MIDI** on two
  ports: bank select (CC 32 = the bank number), program change, pan, pitch
  bend, note on (key 60), volume ramps, note off. The ring is bank 0
  program 7, the jump bank 0 program 17 (40 of the 54 A presses in
  Emerald Coast are followed by it within four frames).
- The Manatee driver binary (`MASARU.DRV`) and its sequence banks are
  never needed: no sequence is started on the route.

## 2. The seam: every sound entry on the route

33 sound entries execute on the maintainer's full route (`tests/replay/
sonic_ec1.tape`), up from 13 at M1: with a player now created, the game
reaches the rest of the ADX API. Calls over the 36,478 frames:

| Entry | Calls | Name (behavioural) | Body |
|---|---|---|---|
| `0x8C644500` | 55,382 | ADXT_GetStat | real: 0 / 3 / 5 from the frame clock (§3) |
| `0x8C643F98` | 19,067 | sdSysServer | fixed answer (0) |
| `0x8C6446E2` | 19,066 | ADXT_ExecServer | real: status bytes, voice-language word |
| `0x8C643C18` | 11,243 | sdMidiMakeMessage | real: the bytes into the game's buffer |
| `0x8C643B9C` | 11,243 | sdMidiSendMessage | real: MIDI to the audio core |
| `0x8C6436E8` | 141 | sdSysGetInfo? | fixed answer (0, as before) |
| `0x8C643E08` | 86 | sdMidiStop (sequence ports) | fixed answer: no sequence plays |
| `0x8C643D88` | 56 | sdMidiSetVol (sequence ports) | fixed answer |
| `0x8C6445EA` | 54 | ADXT_SetOutVol | real |
| `0x8C644904` | 39 | ADXT_StartAfs | real: AFS table, stream start |
| `0x8C64413C` | 36 | ADXT_Create | real: a player record in the SDK's table at `0x8C6737B0` |
| `0x8C644602` | 36 | (player, 20) | stores the word the library stores |
| `0x8C6446D2` | 36 | (player, 2 or 1) | stores the byte the library stores |
| `0x8C6442F2` | 34 | ADXT_Destroy | real |
| `0x8C6434E0`, `0x8C643624`, `0x8C6435BC` | 29 each | sdMemBlkCreate / SetPrm / Destroy | real |
| `0x8C643970` | 28 | sdMultiUnitDownload | real: units into the sound-RAM image |
| `0x8C643754` | 28 | sdSysSetVolume | real (unit taken as 1/8 dB) |
| `0x8C644968` | 20 | ADXT_StartFname | real |
| `0x8C65C25C` | 18 | ADXF file start | real: records the partition's file name; still answers -1 (below) |
| `0x8C64489C` | 18 | ADXT_EntryErrFunc | fixed answer |
| `0x8C643AC0` | 8 | sdPortOpen | real: eight distinct ports |
| `0x8C644468` | 8 | ADXT_Stop | real |
| `0x8C6437F0` | 7 | (0, 0) before the volumes | fixed answer |
| `0x8C643E6C` | 2 | sdDrvDownload | fixed answer: the driver is not run |
| `0x8C643B08` | 2 | sdMidiPlay (sequence ports, all-zero arguments) | logged, fixed answer |
| `0x8C643F6C`, `0x8C6436D0`, `0x8C64387C` | 1 each | driver init, mode, stereo/mono | fixed answers |
| `0x8C644D84`, `0x8C64401C`, `0x8C644022` | 1 each | library set-up | fixed answers |

So 19 entries have real bodies and 14 return the fixed answer the game
accepts. Not called on the route: `0x8C643904` (single-bank download; a
body exists), `0x8C644504` ADXT_GetTime (a body exists), `0x8C65C1CA`
(ADXSTM, the stream layer under the recompiled ADXF code).

The CRI partition loader (`0x8C10C1C8`, CRI code linked into block A and
recompiled) asks `0x8C65C25C` where its AFS file is; the answer stays -1,
so it stops before reading anything, exactly as before M3, and the HLE
takes the file name from the question. Partition 0 is the only one.

## 3. Design

```
guest (recompiled game)                         audio core (hle/sound/snd_core.c)
  sd* / ADXT* entries  ── sd.c, adxt.c ──┐       ┌─ 64 tone voices: PCM16/PCM8/ADPCM from sound RAM,
                                         │       │  envelope, pan, bend, MIDI channel state
  snd_host.c (main side)                 │       ├─ 2 ADX stream decoders (music stereo, voice mono),
   - sound-RAM image (2 MB) ◄── bank downloads   │  loop, resampling to 44.1 kHz
   - feeders: file → block groups ──► byte rings ┤
   - commands (MIDI, start/stop/vol) ──► command ring
   - frame clock: guest status from it          └─ mixer → 735 stereo samples per guest frame
   - once per guest frame: snd_core_render ──► backend/audio.h (SDL queue) and --wav
```

- **Portable core, ME-ready.** `snd_core.c` and `adx.c` touch only their
  own state (8 KB), the shared block (140 KB: a 1,024-entry command ring,
  two 64 KB stream rings, a status block) and the sound-RAM image (2 MB,
  2.2 MB in all). Per sample they use integer arithmetic only and no
  64-bit products, call no library function after init, and allocate
  nothing; every ring has one producer and one consumer, with a barrier
  macro to redefine for the Media Engine's uncached memory. Today the
  core runs on the game's thread at the frame boundary; moving it means
  calling `snd_core_render` from the other processor instead.
- **Determinism.** What the guest sees of a player (playing, ended, time)
  is computed on the main side from a clock of guest frames (735 samples
  each) and the stream's header, never from the core's progress or the
  device. So a run is the same whether the core runs on this thread, on
  another processor and late, or not at all (`audio = off` in
  `sa1psp.ini`, for profiling). Under `--strict` the rendered samples are
  identical between runs (the WAV of two replays is byte-identical; the
  unit test checks the same on synthetic data).
- **Tapes.** Real voice timing changes the game: cutscenes wait for their
  lines, so a tape recorded before M3 desyncs when replayed with it (the
  Chaos 0 fight of `sonic_ec1` drifts from tape frame 7200). Tapes now
  carry `sound-timing real|none`; a tape without the line replays as
  `none`: the players exist and are heard, but report "stopped" to the
  game as before M3. All four tapes pass that way with audio on
  (`tools/replay_test.py`), and the save-state proof passes with the sound
  state in it (`--savestate`: dumps and states identical at 6000). New
  recordings write `real`.
- **Effects are synthesised, not pre-rendered.** README §6 planned
  clips captured from an emulator (option 2). The banks turned out to be
  simple: a split is the AICA channel registers of one tone plus a key
  range and base key (`docs/formats/sound.md` §4), and the game drives
  them with plain MIDI, so the core plays them directly, which is option 1
  without a sequencer. Every effect of every bank works, the launcher has
  nothing to render, and pitch, pan and volume ramps are the game's own.
  `tools/sfx_render.py` renders any program to WAV through the same core
  (into `..\private\sfx\`), for listening and comparison.

## 4. Voice language

The game sets its voice word `0x8C754C00` at boot from the console's
system language (0 → Japanese voices, `EVENT_ADX.AFS` and `V_*.MLT`;
otherwise English, `EVENT_ADX_US.AFS` and `V_*_E.MLT`); the text language
is a separate word. The setting writes the voice word every frame from
`ADXT_ExecServer`, so the text stays English:

- `sa1psp.ini` in the data root: `voice = en | ja | game` (default
  `game`, which is English on this disc); the file is read by the HLE, so
  the PSP build gets it too.
- desktop command line: `--voice en|ja|game`, over the file.

Check (`sonic_to_ec1_start.tape`, frames 1-3300, both languages): the
Japanese run opens `V_SONICTAILS.MLT` and `EVENT_ADX.AFS`, the English run
`V_SONICTAILS_E.MLT` and `EVENT_ADX_US.AFS`; the first Station Square
cutscene line (#400) starts at tape frame 2833 in both, and the two WAVs,
identical elsewhere, differ only while lines play (difference 12,642 RMS
in the second after 2833, 0 before): the Japanese take of #400 is
28,746 bytes of ADX, the English 14,454.

## 5. The cutscene-timing item (M2 report §10)

Measured by the voice status word the game polls (`0x8C5FBAA4`, traced
per vblank with `tests/replay/watch_sound.txt`): on the console from the
library state `sonic_to_ec1_start__chaos0_cutscene` (release Flycast 2.7
oracle, console game frame = aligned + 62), on the desktop by replaying
`sonic_to_ec1_start.tape` with `--sound-timing none` (before M3) and
`real` (M3). Start of each line of the Chaos 0 arrival cutscene, aligned
game frames:

| Line | Console | Desktop, none | Desktop, real | Console − none | Console − real |
|---|---|---|---|---|---|
| #404 | 2287 | 2288 | 2288 | −1 | −1 |
| #405 | 2350 | 2351 | 2351 | −1 | −1 |
| #407 | 2683 | 2684 | 2684 | −1 | −1 |
| #408 | 3057 | 3058 | 3058 | −1 | −1 |
| #409 | 3136 | 3078 | 3133 | +58 | +3 |
| #406 | 3333 | 3274 | 3329 | +59 | +4 |
| #410 | 3365 | 3304 | 3359 | +61 | +6 |

Most lines are started by the event's own timers and were always on
time. #409 waits for #408 to end, and so does everything after it: with
lines ending at once (before M3) the rest of the cutscene ran 58 to 61
game frames ahead of the console; with real playback it is 3 to 6 frames
ahead. The remainder is the console's start-up time per line: its status
steps 1 → 2 → 3 over about four game frames before a line plays (the CRI
library reading the file), where ours starts at once; #408 lasts 78 game
frames on the console from the request, 74 here. Item closed: the cause
was the instant voices; a few frames per line of read latency remain and
are not emulated.

## 6. Exit test

Desktop evidence (done): `sonic_ec1.tape` replayed with audio on and
recorded with `--wav` passes its check (0/20 primary, 0/305 secondary).
Stems (`SA1_SND_MUTE`, `SA1_SND_SOLO`) show music at every start
(Station Square `S_SQUARE.ADX` from frame 1700, -13.6 dBFS over 10 s;
Emerald Coast `ECOAST1.ADX` from 17517, -12.2 dBFS), all 39 voice lines
(-8 to -20 dBFS), 306 ring notes (bank 0 program 7; median 8.4 dB under
the rest of the mix) and 59 jump notes (bank 0 program 17; 11.1 dB under).

Console comparison. Console references are Flycast 2.7 captures of the
AICA mix (a local patch writes every output sample to a WAV with a
vblank/game-frame map; tape-driven with the Lua oracle, or from library
save states; `..\private\m3\console\`). Our side is the same tape on the
Release build, aligned by game frame. Montages (waveform and spectrogram,
ours above the console, 10 s each), in `..\private\m3\montage\` (not
committed: game audio):

| Montage | Slice (aligned game frames) | Verdict |
|---|---|---|
| `ec_rings_10s.png` | Emerald Coast 2730-3030, the three ring pickups at 2759-2770 | Same music and ring pickups at the same time (ring within 3-6 ms); mix -11.2 dBFS ours, -11.3 console |
| `title_menu_10s.png` | menus 430-730, menu effect at 448 | Effect sample-coherent with the console (cancels to -45.6 dB after a gain fit); the console title runs 0.4 s slower over 10 s (its lag frames) |
| `chaos0_10s.png` | Chaos 0 cutscene 2675-2975, `--sound-timing real` | Same lines, music and effects; voices within 0.5 dB of the console's level |

Levels, by fitting our stems to the console WAVs (least squares after
alignment by cross-correlation), before calibration:

| | Ours against the console |
|---|---|
| Music (three areas) | 0.56-0.58 dB louder |
| Voice lines (7) | 0.1 dB louder |
| Ring (16-bit PCM tone) | 0.6-0.8 dB quieter |
| Menu effect, Chaos 0 ambience, Emerald Coast effects (ADPCM tones) | 4.2-4.9 dB quieter |

Spectral envelopes agree (menu effect 0.9999, voices 0.996, ambience
0.99), so the ADPCM decode (low nibble first) is right; the ADPCM level
gap has the same decode step as Flycast's. Its cause was the level law,
not the format: Flycast's AICA channel registers (TL, DISDL, DIPAN) logged
over the Chaos 0 cutscene fades and the Emerald Coast rings show the driver
writing TL = split TL + 2 x (127 - curve[CC 7]), with the bank's 128-byte
curve (header +0x18), about 57 log10 against our 40 log10 (64 fade samples
within 1.3 TL steps rms), and no trace of the -6 dB group level the game
sets on MIDI ports 0 and 1. The ADPCM tones sat at middling volumes on
ports 0/1, where the two errors added up. `snd_core.c` uses that law now,
with no group level and no effect trims, only music -0.6 dB. Effects energy
over the music-subtracted console capture: Emerald Coast rings +0.3 dB (with
the old trims -2.1), Chaos 0 ambience -1.6 dB (+0.7). Part of the ambience
residual is a DSP-only voice (DISDL 0) that Flycast, with its DSP off,
plays from the send level and we leave silent. The driver's code
(`MASARU.DRV` key-on and CC 7) gives the exact law, which `snd_core.c`
follows: c = (T[vol]+1)(T[expr]+1)/128 - 1 with the driver's table T (bank
curve 0 but for entries 1-4), level = ((V[vel]+1)(c+1)(256 - split TL) - 1)
>> 14 with the split's velocity curve V (split byte 42, bank curve n), and
TL = 255 - level. Near TL 0 that is the fitted law; at higher split TLs it
attenuates less (TL 40, CC 7 100: 3.5 dB). Timing against the console's game-frame map: rings -6 to -3 ms,
menu effects +46 ms, music 79-142 ms late on the console (its stream
start-up), voice lines as in §5.

Exit test: BGM in Station Square and Emerald Coast, ring and jump sounds
in Emerald Coast, voice lines, both voice languages: **passed**.

## 7. Latency and CPU cost

| Measure | Value |
|---|---|
| Core, whole route, Release, mean per guest frame (735 samples) | 28.8 µs |
| Same, Debug build | 126 µs |
| Bench (`-O2`): music + voice streams only | 15 µs per frame |
| Bench: + 8 / 16 / 32 effect notes re-triggered every 30 frames | 41 / 61 / 104 µs per frame |
| SDL output latency, steady state | 2,048 queued + 512-frame device buffer = 58 ms; capped at 93 ms + buffer |
| Memory | 2 MB sound RAM + 140 KB shared + 8 KB core |

Per frame that is 0.17 % of a 16.7 ms frame on this PC for the route's
mean. The PSP's Allegrex is single-issue at 333 MHz; at an assumed 10 to
15 times slower per sample the route's mean becomes about 0.3 to 0.45 ms
per guest frame (2 to 3 % of a frame, 4 to 5 % of a 30 fps game frame),
with peaks near 1.5 ms when many effects sound at once. That is small
enough to stay on the main CPU for M5 and worth moving to the Media
Engine when the frame budget is tight; the design allows it (§3). The PSP
build already runs the core (its backend has no audio device yet, so the
weak defaults in `hle/sound/audio_null.c` discard the samples); `audio =
off` in `sa1psp.ini` removes the cost without changing the game.

## 8. Open items

- The tone level law (§6) comes from the driver code; the console data
  only cover split TLs up to 5 and velocity and expression 127.
- Pan follows the driver (MASARU.DRV key-on and CC 10 code): q = pan >> 2;
  below 16 the right side drops by 15 - q DIPAN steps, from 16 the left by
  q - 16, 3 dB a step, step 15 mutes. A channel without a CC 10 since its
  reset (the driver stores 0xC0) plays each split at the split's own DIPAN.
  The rings' CC 10 of 34 / 94 give DIPAN 0x17 / 7 (far side 21 dB down), as
  logged on the console; we lowered it by 5.5 dB before. The per-port pan
  offset the driver also adds (default 0x80, none) is not modelled.
- The console's per-line stream start-up (a few frames, §5) and music
  start-up (80-140 ms) are not emulated.
- DSP reverb (`SFPB`/`SFOB`) not rendered; sequence banks not played (none
  on the route); envelope rate table approximate; the console's few
  frames of per-line read latency not emulated (§5).
- All tapes predate M3 and replay with `sound-timing none`; a full-route
  tape with real timing needs a new recording by the maintainer.
- The PSP backend has no audio device yet: the core runs there and its
  samples are discarded (`audio = off` in `sa1psp.ini` skips it).
- The audio-capture Flycast (`..lycastudio-2.7`) has no Lua, so it
  cannot replay tapes; installing Lua needs the maintainer's approval.
