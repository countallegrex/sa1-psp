# tests/replay: input tapes and state checks

TAS-style regression tests for the desktop build. A **tape** is the pad of
every guest frame of a run; replaying it in strict mode reproduces the run
exactly, so a **check file** recorded from a good run can say where the game
must be (mode, level, act) and what state it should hold (rings, lives,
position) at given frames. Tapes are inputs, not game data, and are
committed. They need the game data to run.

```
SA1_DUMP=..\private\dump\sa1_us_1005 python tools/replay_test.py
```

runs every `NAME.tape` that has a `NAME.check` and prints pass/fail per tape;
without `SA1_DUMP` it prints `skip` and exits 0 (CI). The executable is
`--exe`, `$SA1_EXE`, or the first of `build-desktop/` and `build-tape/`
that has one.

| Tape | Route | Frames |
|---|---|---|
| `title_menu` | title, Start, new Sonic story, file select (no VMU), Station Square loads and runs | 3000 |
| `sonic_ec1` | *to be recorded by the maintainer*: title, Sonic, Station Square, Chaos 0 win, Emerald Coast act 1 complete | |

## Recording a tape

```
sa1psp <1ST_READ.BIN> <data root> --record tests/replay/NAME.tape
```

Play with the controller or keyboard; close the window (or Esc) to stop.
The run is in strict mode, so it plays exactly like its replay. The window
is vsync-paced: on a monitor faster than 60 Hz the game runs fast while
recording (the tape is still exact; set the display to 60 Hz to play at
console speed). Then generate the check file from a replay:

```
python tools/replay_test.py --update NAME
```

which replays the tape with `--watch-list tests/replay/watch.txt
--watch-trace` and runs `tools/tape_expect.py` on the trace. Read the
milestones it wrote before committing: they should tell the route's story.

A scripted run can be turned into a tape too: `--record` writes whatever
the pad was, including `--press` and `--stick` (that is how `title_menu` was
made: the M1 presses, headless, `--frames 3000`).

### For the maintainer: recording `sonic_ec1`

Once the M2 renderer fixes are in: build the desktop target, then

```
build-desktop\backend\desktop\sa1psp.exe ..\private\dump\sa1_us_1005\data\1ST_READ.BIN ..\private\dump\sa1_us_1005\data --record tests\replay\sonic_ec1.tape
```

Title → New game → Sonic → Station Square → find Chaos 0 → win → Emerald
Coast act 1 to the capsule/goal; close the window a few seconds after the
results screen. Then `python tools/replay_test.py --update sonic_ec1` and
`python tools/replay_test.py sonic_ec1` (must pass), and commit both files.
If the replay does not follow the recording, that is a determinism bug:
keep the tape and report it.

## Strict mode and what had to be pinned

`--strict` (implied by `--record` and `--replay`) makes a run a function of
the title, the data, the tape and the build, nothing else. Proof, with the
scripted route (the M1 presses plus the `--poke 5290:…` jump to Emerald
Coast) to frame 6001, RAM dumped at frames 1000, 3000 and 6000:

* two strict headless runs: byte-identical at all three frames;
* a strict run with the window open and unpaced (`SA1_NOVSYNC=1`, so the
  renderer runs and the host timing is completely different): identical;
* two more strict headless runs at the same time as the windowed one
  (host under load): identical.

Without `--strict` the same pair differs in 3 to 6 bytes per dump: the
Ninja timer stamps `0x8C88FE1C`/`0x8C88FE20` and the game's frame-time words
at `0x8C78C600`/`0x8C78C604`, all fed by the timer.

Every source of host dependence, and what strict mode does about it:

| Source | Where | Normal run | Strict run |
|---|---|---|---|
| Free-running timer (syTmr TMU0, TMU1 the vblank-in callback samples) | `hle/shinobi/timer.c` | 1/60 s per frame plus host time elapsed inside the frame, capped at 16 ms | 1/60 s per frame plus 10 µs per timer read inside the frame, capped at 16 ms: busy-waits still see time move, and how far depends only on what the guest code did |
| Real-time clock (syRtc) | `hle/shinobi/hw.c` | host `time()` | the tape's `epoch` (default 2000-01-01 00:00 UTC, `--epoch` to change) plus guest time |
| Pad | `backend/desktop/main.c` | SDL controller/keyboard each frame | the tape (`--replay`), with `--press` buttons ORed on top |
| gdFs async reads (`gdFsReqRd32`) | `hle/shinobi/gdfs.c` | complete inside the request call | the same: completion is at a fixed offset of zero frames, so nothing to pin; if reads ever become asynchronous, strict mode must complete them a fixed number of frames after the request |
| Random numbers | game code | the game's own generator in guest code, seeded from guest state; the HLE has no `rand()`/host entropy | nothing needed (any seed it takes from the timer or RTC is covered above) |
| Renderer | `hle/ninja/`, `draw_gl.c` | writes only guest-deterministic values (buffer pointers, list state) back to guest RAM | verified: windowed and headless dumps are identical |
| Guest RAM at boot | `main.c` | `calloc`: zero | the same |
| Frame pacing (vsync, `SA1_NOVSYNC`) | `draw_gl.c` | wall-clock only | the same: frames are counted, never timed |

Things that still depend on the host and do not affect guest state: the
log's timestamps, `--seconds`, and the spin watchdog (10 s of host time
without a guest frame), which only decides that a run failed.

A tape is tied to a build only through behaviour: a different compiler or
optimisation level can change float rounding in recompiled code, which is
why positions are checked with a tolerance and only as secondary rows.

## Tape format (`.tape`, version 1)

```
sa1psp-tape 1
title-sha256 45bc5d3c…          # SHA-256 of 1ST_READ.BIN; replay refuses another title
epoch 946684800                 # RTC start, Unix seconds
tool sa1psp input_tape 1
# FRAME BUTTONS STICK_X STICK_Y LTRIG RTRIG [COUNT]
1 0 0 0 0 0 499
500 8 0 0 0 0 4
…
end 3000
```

One record per run of identical frames: from guest frame `FRAME`, for
`COUNT` frames (default 1), the pad holds `BUTTONS` (hex, `hle.h`: C 1, B 2,
A 4, Start 8, Up 0x10, Down 0x20, Left 0x40, Right 0x80, Z 0x100, Y 0x200,
X 0x400, D 0x800), the left stick at `STICK_X`/`STICK_Y` (-128..127, y
positive down) and the triggers at `LTRIG`/`RTRIG` (0..255). Frames are
guest frames as the HLE counts them (`hle_frame_count`, the frame hook
runs first in each), and the pad of frame F is what the game's pad server
samples during F. Frames no record covers are a neutral pad. `end N` is the
last frame recorded; a replay stops there unless `--frames` says
otherwise. `#` starts a comment.

## Check format (`.check`)

```
[primary]
2096..2792 8c760970 2 5 - game_mode@2444
[secondary]
3000 8c6bad10 f 362.5 4 player_x
```

`FRAME ADDR SIZE EXPECTED [TOLERANCE] [NAME]`, evaluated in the frame hook
where `--dump` writes:

* `FRAME` is `N` (at frame N exactly), `<=N` (at some frame up to N) or
  `M..N` (at some frame from M to N). The last two are milestones.
* `SIZE` is `1`, `2`, `4` (unsigned), `s1`, `s2`, `s4` (signed) or `f`
  (float32); `TOLERANCE` is an absolute difference, `-` for exact.
* Rows after `[secondary]` are reported as drift and do not fail the run.

`sa1psp --check FILE` fails (exit 1) if a primary row misses, and
`--expect-frame N` fails a run that stops before frame N; a trap aborts
(exit 3) after printing the verdict, and the spin watchdog exits 3. The
verdict line is `check: PASS|FAIL at frame F: …`.

`tools/tape_expect.py` writes the primary rows from the changes of the
`milestone` watches that hold at least 30 frames: value V first seen at
frame F becomes `F-s..F+s V`, with s = 300 frames + 2 % of F, so the
milestones survive HLE changes that shift timing (a few lag frames, a
faster load). Secondary rows sample the `exact` watches every 600 frames
and at the last frame. It reads a `--watch-trace` file (exact to the
frame) or a set of `--dump` files.

## Watched addresses (`watch.txt`)

Each was confirmed by watching it change in strict runs of the scripted
route (Emerald Coast entered by `--poke` at 5290, then the stick held with
`--stick`), against the HUD in screenshots:

| Address | Size | What | How it was confirmed |
|---|---|---|---|
| `0x8C760970` | 2 | game mode | 1 boot, 11 title, 5 adventure field at 2444, 4 action stage after the poke (`docs/m1-boot-report.md`) |
| `0x8C7493BA` / `BC` | 2 | level / act | 26/3 in Station Square, 1/0 in Emerald Coast (`docs/m2-render-report.md`) |
| `0x8C7A64E4` | 4 | the game's frame counter | +1000 over frames 1000–3000, +1499 over 3000–6000: one per game frame (two vblanks), one lag frame |
| `0x8C752BE4` | s2 | rings | 3 on the HUD at frames 5900 and 6000, 0 after Sonic is hit at 6200; never moves with the stick neutral |
| `0x8C752BE0` | s1 | lives | 4 on the HUD; rings poked to 99 at 5505, the next ring makes it 5 |
| `0x8C6BAD10` / `14` / `18` | f | player X / Y / Z | (-9, 4, 4) standing at the Emerald Coast start; (-681, -7, 847) after holding up 1100 frames; inside a static struct at `0x8C6BACF0` that game code addresses directly |
