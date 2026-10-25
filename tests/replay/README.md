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
| `sonic_to_ec1_start` | The maintainer's third run: title, Sonic, Station Square at night, Chaos 0 arrival and fight (won), Station Square by day, Emerald Coast start (stopped before the act's end) | 18402 |
| `ec_scripted` | the scripted route into Emerald Coast: the M1 presses, then the stick held from frame 5450 (collects three rings at 5524-5544). Needs the stage pokes, which a tape does not carry: `tools/replay_test.py` ROUTE_POKES on the desktop, `--poke-ec` in the oracle tools; no check file, so `replay_test.py` skips it | 6600 |
| `sonic_ec1` | The maintainer's fifth run, the full M2 route: title, Sonic, Station Square at night (1673), Chaos 0 arrival and fight, won (4515; the result screen at 11993), Station Square by day (14967), Emerald Coast (17488) through both parts of act 1 (the second at 22991) to the goal (the result screen at 32535), back to Station Square (33150). **Route changed with authentic FSCA (2026-10-24), re-record pending:** recorded when fsca used the host's sinf/cosf; with the console's fsca table (`fsca_sh4.bin`, `recomp/emitter/runtime/sh4_trig.h`) Sonic no longer wins the Chaos 0 fight on this input (nor does the oracle), so the check file now follows the authentic run (the fight lost and retried). It stays as the long regression tape and the desktop-vs-PSP equality test; a winning route will be re-recorded on the authentic build | 36478 |

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

## Save states

A save state (`docs/savestate.md`) is the run at one frame hook: guest RAM,
the HLE's state outside it and the native stacks of the game's tasks. It
loads back to exactly that point, so a debug run can start where the
problem is instead of at the title. States are tied to one build of
`sa1psp` (they hold native stacks; the loader checks the executable's
hash) and hold guest RAM, so `*.state` is git-ignored.

```
sa1psp <1ST_READ.BIN> <data root> --strict --save 3000:ss.state --press 500:8:4 ...
sa1psp <1ST_READ.BIN> <data root> --strict --load ss.state --frames 6000 --dump 6000:ram.bin ...
```

* `--save FRAME:FILE` (repeatable) writes the state at guest frame FRAME,
  at the point where `--dump` writes: after that frame's `--poke`s.
* `--load FILE` restores it before the first frame and continues; the
  options of the loading run apply from there (`--frames`, `--dump`,
  `--check`, `--replay` with the tape the saving run used, `--press`/`--poke`
  for later frames). `--record` cannot be combined with `--load` (a tape
  starts at frame 1). Check rows due before the loaded frame are skipped.
* In the window, **F5** saves and **F8** loads `quick.state` in the current
  directory (`--quick FILE` to rename). F8 works at any time in the run.
* A load with another `--strict`/`--epoch`/`--aspect` setting than the save
  is logged and allowed; strict runs then differ from the saving one.
* `python tools/state_diff.py A.state [B.state]` lists a state's chunks or
  reports the first difference between two.

Proof (`python tools/replay_test.py --savestate`, with `SA1_DUMP`): the
scripted route is recorded to a tape in strict mode with `--save 3000`,
`--save 6000` and `--dump 6000`; a second run replays the tape from the
3000 state to 6000 with the same `--dump` and `--save`. The 18 MB dumps
are byte-identical, and the two 6000 states agree in every chunk but the
coroutine stacks (28 dead bytes between frames). The 3000 state is 17 MB
(36 MB of guest window and 6 MB of textures, packed as byte runs). CI runs
the same machinery on synthetic data (`tests/savestate/`).

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
| Sound (M3) | `hle/sound/` | the audio core renders 735 samples per guest frame; what the guest sees of a player (playing, ended, time) is computed from a clock of guest frames and the stream's header, not from the device or the core | the same; and the tape's `sound-timing` line (below) says whether the players take their streams' length (`real`) or report stopped at once (`none`, how the game saw sound before M3) |

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
sound-timing real               # M3 and later; absent = none
fpu-round zero                  # 2026-10-19 and later; absent = nearest
fmac fused                      # 2026-10-21 and later; absent = follows fpu-round
stick-gate round                # 2026-10-22 and later; absent = square
aspect 4:3                      # 2026-10-21 and later; absent = wide
slow-substep 7530               # oracle tapes only; may repeat
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

`sound-timing` records what the guest saw of sound playback when the
tape was made. Before M3 there was no ADX player, so every voice line
ended the moment it started; with real playback a cutscene waits for its
line (`docs/formats/sound.md` §2.2), which moves everything after it.
Replaying a pre-M3 tape with real timing desyncs it (the Chaos 0 fight of
`sonic_ec1` drifts from tape frame 7200), so a tape without the line
replays as `none`: the players are still created and still heard (BGM,
voices, effects all render), but they report "stopped" and no time to the
game, as before. `--record` writes `real` (or what `--sound-timing` says),
and `--sound-timing real|none` overrides a tape's line. All the tapes in
this directory predate M3.

`fpu-round` is the host FPU's rounding during the run. The console runs
its float code with FPSCR.RM = 1, round toward zero (the Katana reset
value 0x00040001), and the recompiled code computes on the host FPU, so
since 2026-10-19 the desktop sets the host to round toward zero too
(`backend/desktop/main.c`). It matters: on the scripted Emerald Coast
route, round-to-nearest drifts Sonic's position by tenths of a unit from
game frame 2844, he stops 3.4 units further on at the beach deck chairs
and at 2859 gets free and runs on, while the console stays stuck; with
round toward zero he stops where the console does (z 403.80 against
403.84) and stays, and the routes agree to 2900 (position within 4 units,
none of the 325-unit split). Tapes recorded before carry no line and
replay with round-to-nearest, which they were recorded with; `--record`
writes `zero`, and `--fpu-round zero|nearest` overrides a tape's line.
For console comparisons of an old tape (`tools/flycast/`), replay it with
`--fpu-round zero --sound-timing real` to get the console's behaviour.

`fmac` is how the recompiled `fmac` rounds: `fused` once, as Flycast
does, `split` twice, as every desktop build before e8a141a (and the
PSP build always). One ulp in a rare tie was enough for `sonic_ec1`
(recorded split, to nearest) to lose its route at frame 24000 on the
fused form. A tape without the line follows its rounding: `zero` fused,
`nearest` split. `--fmac fused|split` overrides it.

`stick-gate` is the stick's range. A Dreamcast controller's stick moves in
a round gate, so the pad never reports a vector longer than 128: Flycast's
controller scales a longer one back onto that circle, and (-127,-127), a
full diagonal, reaches the game as (-91,-91). With `round` the pad server
does the same (`hle/shinobi/pad.c`); with `square` it passes the tape's
axes as they are, as every run before 2026-10-22 did. On the spliced
Emerald Coast route (a private tape: `ec_scripted` then the `sonic_ec1`
run from the beach) the first full diagonal, at game frame 2774, put the
desktop off the console's path at 2776; with `round` the two agree within
0.003 units until 2900 and the `rand()` seeds until 2914. `--record`
writes `round`, a run without a tape uses it, and a tape without the line
replays as `square`: `sonic_ec1` and `sonic_to_ec1_start` hold full
diagonals and were recorded square. `--stick-gate round|square` overrides
the line; console comparisons of those two tapes need `round`
(`tools/flycast/run_oracle.py --stick-gate round`).

`slow-substep F` makes the profiling timer TMU1 read two vblanks late in
guest frame F (`hle/shinobi/hw.c`). The game's catch-up loop (0x8C04EA4A)
runs a third logic sub-step in a game frame whose second one ends 1.85
vblanks or more after the timer reload; on the console a long GD-ROM load
does that, but the desktop's loads take no time, so after one the console
runs a 1/60 s step ahead. On `sonic_to_ec1_start` at 4:3 the load at game
frame 3761 lasts 7 vblanks on the console and 3 on the desktop: the
camera then turns one sub-step earlier, and once Sonic runs, the
camera-relative stick sends him off the console's route at 3956. With
`slow-substep 7530` the desktop matches the console exactly through 3962.
`tools/tape_diff.py slow` derives the lines from a desktop and an oracle
trace that both watch the sub-step counter 0x8C752BEC. `--record` writes
none (live play has no such frames); `--slow-substep F` adds one.

The aspect is game logic too. The wide default widens the clip rectangle
the game's own culling tests (0x8C88F600, `hle/ninja/kamui.c`), and some
objects update only while that test passes: Emerald Coast's 0x8C0FDCE0
blends its animation only on screen, which moves its `rand()` calls and,
through them, the Egg Keeper's turns. So a tape carries its aspect: the
`aspect` line, which `--record` writes and a replay without `--aspect`
takes (absent: wide, what every tape before 2026-10-21 was recorded
with). `ec_scripted` says `4:3`, the console's, so the oracle compares it
like for like; `sonic_ec1` and `sonic_to_ec1_start` say `wide`: the maintainer
played them wide, and at 4:3 the route leaves his inputs (it traps at
guest frame 31310). The oracle tools' `--aspect` overrides the line. The oracle also copies the
desktop's `rand()` seed into the console at each game-mode change
(`tools/flycast/tape_oracle.lua`): a load lets vblanks pass on the console
and the adventure field then runs catch-up updates, a few more `rand()`
calls than the instant desktop load makes (19 at the Station Square to
Emerald Coast change).

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

## Tolerance compare (`tape_diff.py band`)

Since 2026-10-25 a run need not be bit-identical to count as the same
game: RNG and float exactness are no longer required (native and VFPU
math, native rewrites of hot routines). What must hold is what a player
sees. `tools/tape_diff.py band` judges that from two watch traces of the
same tape, a reference and the run under test (desktop before and after
a change, desktop and PSP):

```
sa1psp ... --replay T.tape --watch-list tests/replay/watch.txt --watch-trace ref.tsv
sa1psp ... --replay T.tape --watch-list tests/replay/watch.txt --watch-trace run.tsv   # the change
python tools/tape_diff.py band ref.tsv run.tsv
```

Both traces are aligned by game frame (`game_frame`; a Flycast trace by
its `oracle_g` column). The run passes when:

* **route**: the changes of the `milestone` watches (game mode, level,
  act) come in the same order, each within the check files' slack
  (300 + 2 % of the frame, here in game frames);
* **events**: every integer `exact` watch (rings, lives; a lives change
  is a death or a 1-up) runs through the same values in the same order,
  each change within `--event-slip` game frames (default 30);
* **positions**: the float `exact` watches, taken as one vector (the
  player's X, Y, Z), stay within `--band` (default: the watch list's
  largest tolerance, 4.0) of the reference, or of the reference up to
  `--slip` game frames earlier or later (default 2); game frames within
  `--settle` (30) of a milestone change, where the game teleports the
  player, and frames where the runs are in different scenes are left out;
* **end**: the run reaches the reference's last game frame, within
  `--event-slip` (no trap, no soft-lock); `--until G` compares up to game
  frame G only. Unmatched route changes and events near the end of the
  shorter run are not counted against it.

It prints the largest position deviation and its game frame whether it
passes or not: that is the band a native routine holds, the number its
landing reports. The verdict is one line, `band: PASS ...` or
`band: FAIL route|events|positions|end ...`; exit 0, 1, or 2 when there
is nothing to compare. `--exact` is the bit-exact compare, kept for
debugging: every column but the vblank (rng_seed included) equal on every
frame, aligned by vblank (by game frame for an oracle trace).
`tape_diff.py diff` is unchanged.

Reference traces hold guest RAM values, so they stay outside the
repository: `../private/p3/` (name them `TAPE_desk_COMMIT.tsv`). Record a
fresh one from the build before a change; a trace from an older build is
only a reference while the route still matches it.

### Save-state segments (`tools/segment_test.py`)

A whole-tape compare stops meaning anything once the run under test
moves the rand() sequence: the tape's inputs are indexed by vblank, so
after an RNG-driven fight (Chaos 0) they reach other moments of play.
For game-code changes (plan-30fps.md section 8) the tape is judged scene
by scene instead:

```
SA1_DUMP=..\private\dump\sa1_us_1005 python tools/segment_test.py sonic_to_ec1_start --b=--fmac --b=fused
```

replays the tape once for a reference trace, cuts it at each milestone
change (segments of at least `--min-len` 300 frames, the first from
boot), replays it again with a save state at each cut, then replays every
segment from its state twice in the same build: with the A options and
with A + B, B being the change under test as a runtime switch. The A run
must equal the reference bit for bit (the state restores the game), and
the B run must pass `tape_diff.py band` (4.0 units, +-2 slip, events 30)
up to the game frame before rand() first splits, or to the segment's end.
A band exit before that fails the segment. With no `--b` the driver checks
itself: every segment identical. One line per segment; exit 0 when all
pass. `--starts F,F,...` sets the cuts by hand.

On `sonic_to_ec1_start` (5 segments, about 2 minutes): no B, all
identical; `--fmac fused` all pass, with rand() splitting in the Chaos 0
fight (game frame 4246) and in Emerald Coast (8227), largest deviation
1.51 units before a split; a `--poke` of the player's X in Station
Square fails exactly that segment.

`--psp` runs the same on the console (`tools/psp_run.py`, the build in
`build-psp/sa1psp`; states of ~25 MB stay in its `psplink-run/`). The
self-check there passes on `sonic_to_ec1_start` (5 segments, each loaded
run identical to the straight one, rng_seed included; about 31 minutes):
the PSP's save states restore the game exactly. The PSP's cuts fall where
its own run has its milestones (it leaves the Chaos 0 fight later than
the desktop), so desktop and PSP are each judged against themselves.

## Flycast differential (the oracle)

```
SA1_DUMP=..\private\dump\sa1_us_1005 python tools/flycast/run_oracle.py tests/replay/title_menu.tape
```

replays a tape on the desktop build and in Flycast 2.7 and reports the
first game frame where they disagree. Flycast's Lua API can inject the
controller every frame (`flycast.input.pressButtons`/`releaseButtons`
write the active-low button word, `setAxis` the 16-bit stick and trigger
values, from a `vblank` callback on the emulation thread), so
`tools/flycast/tape_oracle.lua` drives the console side; `run_oracle.py`
runs Flycast from its own copy (`../flycast/oracle-2.7`, made from
`../flycast/release-2.7`, VMUs unplugged like the desktop build) with
`-config config:LuaFileName=tape_oracle.lua`.

Alignment is by the game's frame counter (`game_frame`), not by vblank:
Flycast emulates GD-ROM load times and the desktop loads instantly. The
desktop build also skips whole phases the console plays, so the alignment
is re-anchored at each game-mode change both runs share
(`tools/tape_diff.py gpad` writes the desktop's mode changes as `sync`
lines; the Lua script matches Flycast's mode changes to them and writes the
aligned game frame as an `oracle_g` column). The anchor is the first count
after the last one seen in the old mode: the counter can step by two
between two vblanks at a mode change (the Emerald Coast entry does on the
console), and anchoring on the count the vblank shows put the console one
game frame early. Flycast latches the pad a vblank after the script sets
it, so each vblank presents the desktop's pad of the next vblank (`gpad`
writes each game frame's first-vblank pad as `first` lines). That holds
while the console runs a game frame's logic in the counter's later vblank,
which changes the rand() seed there; when the console falls behind and runs
it together with the counter step (the seed then stays put in the later
vblank), the next logic step is still a vblank away and takes the pad of
the frame's last vblank instead. With both, the scripted route's Sonic
matches the console bit for bit into Emerald Coast.
One case no pad rule can cover: the adventure field runs two logic steps
per game frame, each reading the pad after one vblank of the frame's wait
(0x8C0517F6), and a console that runs behind does both in one vblank
period, so both read the same latched pad. A tape whose pad changes
between a frame's two vblanks then reaches the console one logic step
off. For exact comparisons, `tools/tape_diff.py quantize TAPE TRACE -o
OUT` moves every pad change to the first vblank of its game frame
(replay the output and quantize again to check that it is stable). On the
Emerald Coast splice route this took the exact match (within 1 ulp, same
rand() count) from game frame 3002 to the end of the run, 3300.
Library states saved before this rule carry the old offset: rebuild a
tape's library (`oracle_states.py build`) before trusting `--from-state`
past a shifted anchor. In a mode the desktop never
entered the pad is neutral, with Start tapped every 60 vblanks after 120
(console-only prompts). `tools/tape_diff.py diff` compares the watched
values at the last vblank of each aligned game frame, within the watch
list's tolerances; a value the other run has within `--slip` (default 2)
game frames is reported as a slip, not a divergence.

What the console does that the desktop build does not, found this way:

| Game mode | Console (Flycast) | Desktop |
|---|---|---|
| 12, 13 | from about vblank 166, before the logos; 13 waits for a button (without one Flycast stayed there 15,000 vblanks; the oracle's Start tap passes it) | never entered |
| 8 (intro movie) | about 6,900 vblanks, game frame counter stopped | skipped in 2 vblanks (`hle/overlay/movie.c`) |
| 10 | about 46 vblanks between the movie and the title | never entered |

First result, `title_menu`: with those phases skipped, the runs agree on
every watched value for all 1,497 compared game frames (title, menus, file
select, Station Square: game mode, level, act, rings, lives, player
position), with one slip: the opening event moves Sonic to (362.5, 1450.7)
one game frame later on Flycast (game frame 1232).

### Starting the console from a save state

A comparison at tape frame N need not replay the console from boot:

```
SA1_DUMP=..\private\dump\sa1_us_1005 python tools/flycast/oracle_states.py build tests/replay/sonic_to_ec1_start.tape
SA1_DUMP=..\private\dump\sa1_us_1005 python tools/flycast/run_oracle.py tests/replay/sonic_to_ec1_start.tape --from-state 5400 --until 2800
```

`oracle_states.py build` runs the tape once in Flycast and saves a state at
each of a tape's scenes (by default the title, the Station Square arrival,
the Chaos 0 cutscene start, the Chaos 0 fight, the Emerald Coast start and
the frame before the fourth run's crash for `sonic_to_ec1_start`; every
3000 frames for a tape it does not know; `--frames NAME:FRAME,...` to
choose).  The states live outside the repository in `../flycast/states/`
(they hold the console's RAM) under a `manifest.json` that names, for each
state, the tape, the tape's SHA-256, the tape (guest) frame and the desktop
game frame it stands for, where exactly the console was when the state was
written, and the oracle script's alignment variables.  `run_oracle.py
--from-state N` loads the nearest state at or before frame N into Flycast
from the script's first overlay callback (Flycast's own auto-load setting
does not take effect from the command line, and a load from the vblank
callback deadlocks the emulator), restores the alignment, and feeds the
tape from there; `--until G` stops at desktop game frame G.  When the tape's
hash no longer matches the manifest the library of that tape is rebuilt
first.  `oracle_states.py list` shows the library, `check TAPE` whether it
is current (exit 1 when not).  A frame the console never reaches on a tape
is reported and left out: on `sonic_to_ec1_start` the console loses the
Chaos 0 fight, so nothing after it has a state; the scripted `ec_scripted`
tape (with `--poke-ec`) provides the Emerald Coast one.

Limits: the console clock and the RTC are Flycast's (host time), not the
tape's epoch; Flycast's own flash (HLE BIOS) stands in for the desktop's
erased one; and the counter alignment assumes the game frame counter only
counts up (true on the routes traced so far).

## Watched addresses (`watch.txt`)

Each was confirmed by watching it change in strict runs of the scripted
route (Emerald Coast entered by `--poke` at 5290, then the stick held with
`--stick`), against the HUD in screenshots:

| Address | Size | What | How it was confirmed |
|---|---|---|---|
| `0x8C760970` | 2 | game mode | 1 boot, 11 title, 5 adventure field at 2444, 4 action stage after the poke (`docs/m1-boot-report.md`) |
| `0x8C7493BA` / `BC` | 2 | level / act | 26/3 in Station Square, 1/0 in Emerald Coast (`docs/m2-render-report.md`) |
| `0x8C7A64E4` | 4 | the game's frame counter | +1000 over frames 1000–3000, +1499 over 3000–6000: one per game frame (two vblanks), one lag frame |
| `0x8C5FBB08` | 4 | the `rand()` seed (trace only) | the LCG state of 0x8C10D334 (×0x41C64E6D + 0x3039); the Station Square field calls it 19 times per update; `tools/flycast/tape_oracle.lua` copies the desktop's value into the console at each mode change |
| `0x8C752BE4` | s2 | rings | 3 on the HUD at frames 5900 and 6000, 0 after Sonic is hit at 6200; never moves with the stick neutral |
| `0x8C752BE0` | s1 | lives | 4 on the HUD; rings poked to 99 at 5505, the next ring makes it 5 |
| `0x8C6BAD10` / `14` / `18` | f | player X / Y / Z | (-9, 4, 4) standing at the Emerald Coast start; (-681, -7, 847) after holding up 1100 frames; inside a static struct at `0x8C6BACF0` that game code addresses directly |
